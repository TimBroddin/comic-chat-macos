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

        /// §8 Topology A (plain IRC, no IRCX) reviewer finding: an inbound
        /// whisper arriving as a plain `PRIVMSG <ourNick> :text` (no channel
        /// prefix) is classified `CC_EV_TEXT` by the engine, NOT
        /// `CC_EV_WHISPER` (this file's own top doc comment, verified against
        /// `ircsock.cpp:907-954`) -- so it must still reach the whisper box
        /// via the `.text` case's routing, not only via `.whisper`. Before the
        /// fix, `handleLocked`'s `.text` case only did `bridge.apply +
        /// recompose`, so this landed in the main strip but never in
        /// `whisperHistories`/`onWhisper`.
        @Test(.timeLimit(.minutes(1)))
        func plainPrivmsgToSelfRoutesToWhisperBox() async throws {
            let server = try LoopbackIRCServer()
            let art = repoRoot5Up().appendingPathComponent("v2.5-beta-1-modern/comicart").path
            let model = ChatSessionModel(config: .init(host: "127.0.0.1", port: server.port,
                                                       nick: "Mac", room: "#p4", artDir: art))
            let receivedBox = WhisperReceivedBox()
            model.onWhisper = { peer, line in receivedBox.append(peer: peer, line: line) }
            try await model.start()
            try await server.replyToProbeWith451ThenWelcomeAndJoin(nick: "Mac", channel: "#p4")

            // The wire form §8 Topology A actually produces: a plain PRIVMSG
            // targeted at our own nick, no channel prefix.
            try await server.send(":Bob!u@h PRIVMSG Mac :psst")

            while model.whisperHistories["Bob"] == nil {
                try await Task.sleep(nanoseconds: 5_000_000)
            }
            #expect(model.whisperHistories["Bob"] == [WhisperLine(nick: "Bob", text: "psst", isOwn: false)])
            #expect(receivedBox.entries.contains { $0.peer == "Bob" && $0.line.text == "psst" && !$0.line.isOwn })

            model.shutdown()
            server.stop()
        }

        /// Own-echo no-double-count: `sendWhisper`'s wire line is a plain
        /// `PRIVMSG <peer> :...` (this file's own top doc comment) carrying
        /// COOKED SM_WHISPER-mode (mode == 2) annotations
        /// (`ChatSessionModel.sendWhisper`'s doc comment). If a server echoes
        /// that SAME line back to us, its `target` is the PEER ("Bob"), not
        /// our own nick -- so the new plain-IRC routing's `target ==
        /// currentOwnNick` detection does NOT fire for it, but its cooked
        /// mode-2 annotations WOULD match the `annotations?.mode == 2`
        /// detection if the existing own-echo dedup (which drops it before
        /// any routing runs) didn't get there first. This pins that the
        /// dedup's early `return` in `handleLocked` is what saves us here --
        /// the whisper history must contain the own line EXACTLY ONCE.
        @Test(.timeLimit(.minutes(1)))
        func ownWhisperEchoIsDedupedNotDoubleCounted() async throws {
            let server = try LoopbackIRCServer()
            let art = repoRoot5Up().appendingPathComponent("v2.5-beta-1-modern/comicart").path
            let model = ChatSessionModel(config: .init(host: "127.0.0.1", port: server.port,
                                                       nick: "Mac", room: "#p4", artDir: art))
            try await model.start()
            try await server.replyToProbeWith451ThenWelcomeAndJoin(nick: "Mac", channel: "#p4")

            try await model.sendWhisper(to: "Bob", text: "back")

            // Capture the REAL wire bytes the engine emitted (cooked
            // annotations included) and echo that exact line back, exactly as
            // a server that echoes a client's own PRIVMSG would.
            let sentLines = try await waitForReceivedLine(server, containing: "back")
            guard let wireLine = sentLines.first(where: { $0.hasPrefix("PRIVMSG Bob :") && $0.contains("back") }) else {
                Issue.record("expected a captured PRIVMSG Bob wire line containing 'back'")
                return
            }
            try await server.send(":Mac!mac@h \(wireLine)")

            func matchCount() -> Int {
                model.whisperHistories["Bob"]?.filter { $0.isOwn && $0.text == "back" }.count ?? 0
            }
            while matchCount() < 1 {
                try await Task.sleep(nanoseconds: 5_000_000)
            }
            // Settling window long enough for a double-append (pre-fix-style
            // regression) to have landed, before asserting the final count.
            try await Task.sleep(nanoseconds: 200_000_000)
            #expect(matchCount() == 1, "expected exactly one own whisper line for 'back', got \(matchCount())")
            #expect(model.whisperHistories["Bob"] == [WhisperLine(nick: "Mac", text: "back", isOwn: true)])

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

        /// Final-review Important #4b (RED before the fix) — COORDINATOR
        /// RULING: a SESSION-scoped (private) whisper renders in the whisper
        /// box ONLY, never on the main comic strip, matching the original's
        /// box-only private-whisper posture (`whisprbx.cpp` — a dedicated
        /// whisper window, no `CUnitPanel`/strip involvement at all). Pre-fix,
        /// `handleLocked`'s `.whisper` case applied to the strip whenever
        /// `isActiveRoom || channel == nil` -- the `channel == nil` half meant
        /// a session-scoped whisper balloon rendered ONCE on the strip and
        /// then silently vanished on the NEXT reflow (a session-scoped event
        /// is never IN any room's transcript, so `rebuildStripLocked`'s
        /// replay never re-applies it) -- worse than either fully-consistent
        /// behavior. This test drives a WHISPER wire form the engine parser
        /// classifies session-scoped: `WHISPER <arg> <targetlist> :<text>`
        /// where `<arg>` does NOT carry a channel prefix (`#`/`%`/`&`) --
        /// `ircsock.cpp`'s `cmdidWhisper` handler only resolves a room token
        /// when `CHANNELPREFIX(args[1][0])` holds (`defines.h:162`); here
        /// `args[1]` is the bare nick "Mac", so `room_token` stays 0 and the
        /// event is session-scoped, distinct from `inboundWhisperRoutesToHistoryAndCallback`'s
        /// `WHISPER #p4 Mac :psst` (which DOES carry a channel-prefixed first
        /// arg and is therefore room-scoped -- that test is unaffected by
        /// this fix, its strip balloon behavior is unchanged and it never
        /// asserted on `panelCount` anyway).
        ///
        /// Asserts: the line lands in `whisperHistories` (the box) exactly as
        /// before, AND `panelCount` never grows past the title-only baseline
        /// -- proving the strip stays untouched, box-only, for a
        /// session-scoped whisper.
        @Test(.timeLimit(.minutes(1)))
        func sessionScopedWhisperRendersBoxOnlyNeverOnStrip() async throws {
            let server = try LoopbackIRCServer()
            let art = repoRoot5Up().appendingPathComponent("v2.5-beta-1-modern/comicart").path
            let model = ChatSessionModel(config: .init(host: "127.0.0.1", port: server.port,
                                                       nick: "Mac", room: "#p4", artDir: art))
            try await model.start()
            try await server.replyToProbeWith451ThenWelcomeAndJoin(nick: "Mac", channel: "#p4")

            while model.panelCount == 0 {
                try await Task.sleep(nanoseconds: 5_000_000)
            }
            let baselinePanelCount = model.panelCount

            // Session-scoped IRCX WHISPER: first arg "Mac" carries no channel
            // prefix, so the engine resolves room_token == 0 (see this test's
            // doc comment for the verified ircsock.cpp citation).
            try await server.send(":Bob!u@h WHISPER Mac Mac :psst")

            while model.whisperHistories["Bob"] == nil {
                try await Task.sleep(nanoseconds: 5_000_000)
            }
            #expect(model.whisperHistories["Bob"] == [WhisperLine(nick: "Bob", text: "psst", isOwn: false)])

            // Settling window long enough for a pre-fix-style strip balloon
            // to have landed, before asserting panelCount never moved.
            try await Task.sleep(nanoseconds: 200_000_000)
            #expect(model.panelCount == baselinePanelCount,
                    "a session-scoped whisper must NOT add a strip panel (box-only ruling); baseline \(baselinePanelCount), got \(model.panelCount)")

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
