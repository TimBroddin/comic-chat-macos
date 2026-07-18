import Testing
import Foundation
import CoreGraphics
@testable import ComicChatKit

// Plan 4b Task 4 — whisper routing: `ChatSessionModel.whisperHistories`/
// `onWhisper`/`sendWhisper`, and the `acceptWhispers` drop path.
//
// WIRE-CLASSIFICATION NOTE (verified against the engine parser, NOT assumed):
// a plain PRIVMSG targeted at our own nick (`PRIVMSG <ownNick> :text`, no
// channel prefix) is classified `CC_EV_TEXT` by `ircsock.cpp`'s `cmdidPrivMsg`
// handler (`bChannel = CHANNELPREFIX(args[1][0])` is false for a nick target,
// so `msgType` never gets the `MT_WHISPER` bit, and `OnTextMsg`/`ccProcessSay`
// -> `ccPayloadSay` there always builds `CC_EV_TEXT`, never `CC_EV_WHISPER`).
// `CC_EV_WHISPER` is ONLY built by the separate `cmdidWhisper` handler, which
// exists for the dedicated IRCX `WHISPER <chan> <targetlist> :<text>` command
// (ircsock.cpp:1135-1176). This is also what the outbound side actually
// speaks: `cc_session_send_whisper` (cc_session.cpp:543) calls
// `bChatSendPrivMesg` -> `bChatSendToTarget(nicks[i], ...)`, which sends a
// plain `PRIVMSG <nick> :...` -- so `sendWhisper`'s own wire line is a
// PRIVMSG, and the SERVER-side inbound-whisper wire form this test drives is
// the IRCX `WHISPER` command, not a PRIVMSG-to-self (the brief's sketch used
// the latter, which is wrong -- see the Task 4 report's deviations section).
//
// SERIALIZATION: nested inside `EngineGlobalStateSelfTests` (.serialized),
// matching every other suite touching process-global engine state via
// ProtocolSession/ChatSessionModel (ChatSessionModelTests/OutboundEncodingTests).
extension EngineGlobalStateSelfTests {
    @Suite(.serialized)
    struct WhisperRoutingTests {
        /// Polls `server.receivedBytes` until it contains `substring`, returning
        /// the accumulated c2s bytes split into lines — same shape as
        /// `ChatSessionModelTests.waitForReceivedLine`.
        private func waitForReceivedLine(
            _ server: LoopbackIRCServer,
            containing substring: String
        ) async throws -> [String] {
            while true {
                let text = String(data: server.receivedBytes, encoding: .isoLatin1) ?? ""
                if text.contains(substring) {
                    return text.components(separatedBy: "\r\n").filter { !$0.isEmpty }
                }
                try await Task.sleep(nanoseconds: 5_000_000)
            }
        }

        @Test(.timeLimit(.minutes(1)))
        func inboundWhisperRoutesToHistoryAndCallback() async throws {
            let server = try LoopbackIRCServer()
            let art = repoRoot5Up().appendingPathComponent("v2.5-beta-1-modern/comicart").path
            let model = ChatSessionModel(config: .init(host: "127.0.0.1", port: server.port,
                                                       nick: "Mac", room: "#p4", artDir: art))
            let receivedBox = WhisperReceivedBox()
            model.onWhisper = { peer, line in receivedBox.append(peer: peer, line: line) }
            try await model.start()
            try await server.replyToProbeWith451ThenWelcomeAndJoin(nick: "Mac", channel: "#p4")

            // The IRCX WHISPER wire form (verified above): WHISPER <chan>
            // <targetlist> :<text>. Sent to our own nick's targetlist.
            try await server.send(":Bob!u@h WHISPER #p4 Mac :psst")

            while model.whisperHistories["Bob"] == nil {
                try await Task.sleep(nanoseconds: 5_000_000)
            }
            #expect(model.whisperHistories["Bob"] == [WhisperLine(nick: "Bob", text: "psst", isOwn: false)])
            #expect(receivedBox.entries.contains { $0.peer == "Bob" && $0.line.text == "psst" && !$0.line.isOwn })

            model.shutdown()
            server.stop()
        }

        @Test(.timeLimit(.minutes(1)))
        func sendWhisperSendsWireLineAndAppendsOwnHistory() async throws {
            let server = try LoopbackIRCServer()
            let art = repoRoot5Up().appendingPathComponent("v2.5-beta-1-modern/comicart").path
            let model = ChatSessionModel(config: .init(host: "127.0.0.1", port: server.port,
                                                       nick: "Mac", room: "#p4", artDir: art))
            try await model.start()
            try await server.replyToProbeWith451ThenWelcomeAndJoin(nick: "Mac", channel: "#p4")

            try await model.sendWhisper(to: "Bob", text: "back at you")

            let sent = try await waitForReceivedLine(server, containing: "back at you")
            #expect(sent.contains { $0.hasPrefix("PRIVMSG Bob :") && $0.contains("back at you") })

            while (model.whisperHistories["Bob"]?.contains { $0.isOwn }) != true {
                try await Task.sleep(nanoseconds: 5_000_000)
            }
            #expect(model.whisperHistories["Bob"]?.contains(
                WhisperLine(nick: "Mac", text: "back at you", isOwn: true)) == true)

            model.shutdown()
            server.stop()
        }

        /// Task-5's `acceptWhispers` setting doesn't exist yet — until it
        /// lands, `ChatSessionModel` hard-codes the default `true` behind an
        /// internal seam (`_acceptWhispers`, TEST-ONLY reachable via
        /// `@testable import`) so this drop path can be exercised now.
        @Test(.timeLimit(.minutes(1)))
        func whisperDroppedWhenAcceptWhispersFalseSurfacesViaStatus() async throws {
            let server = try LoopbackIRCServer()
            let art = repoRoot5Up().appendingPathComponent("v2.5-beta-1-modern/comicart").path
            let model = ChatSessionModel(config: .init(host: "127.0.0.1", port: server.port,
                                                       nick: "Mac", room: "#p4", artDir: art))
            model._acceptWhispers = false
            let statusBox = StatusBox()
            model.onStatus = { text in statusBox.append(text) }
            let whisperFiredBox = WhisperReceivedBox()
            model.onWhisper = { peer, line in whisperFiredBox.append(peer: peer, line: line) }
            try await model.start()
            try await server.replyToProbeWith451ThenWelcomeAndJoin(nick: "Mac", channel: "#p4")

            try await server.send(":Bob!u@h WHISPER #p4 Mac :psst")

            while statusBox.entries.isEmpty {
                try await Task.sleep(nanoseconds: 5_000_000)
            }
            #expect(model.whisperHistories["Bob"] == nil)
            #expect(whisperFiredBox.entries.isEmpty)
            #expect(statusBox.entries.contains { $0.contains("Bob") })

            model.shutdown()
            server.stop()
        }
    }
}

/// Thread-safe accumulator for `onWhisper`'s (peer, WhisperLine) callback —
/// same lock-guarded-box shape as `ChatSessionModelTests`' `MembersBox`/`ImagesBox`.
private final class WhisperReceivedBox: @unchecked Sendable {
    private let lock = NSLock()
    private var storage: [(peer: String, line: WhisperLine)] = []

    func append(peer: String, line: WhisperLine) {
        lock.lock(); defer { lock.unlock() }
        storage.append((peer, line))
    }

    var entries: [(peer: String, line: WhisperLine)] {
        lock.lock(); defer { lock.unlock() }
        return storage
    }
}

/// Thread-safe accumulator for `onStatus`'s `String` callback.
private final class StatusBox: @unchecked Sendable {
    private let lock = NSLock()
    private var storage: [String] = []

    func append(_ text: String) {
        lock.lock(); defer { lock.unlock() }
        storage.append(text)
    }

    var entries: [String] {
        lock.lock(); defer { lock.unlock() }
        return storage
    }
}
