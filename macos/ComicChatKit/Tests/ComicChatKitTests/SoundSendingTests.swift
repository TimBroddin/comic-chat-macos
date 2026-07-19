import Testing
import Foundation
@testable import ComicChatKit

// Outbound-sound task — `ChatSessionModel.sendSound(file:text:)`: the
// wire-shape loopback round-trip (send -> server receives the CTCP SOUND
// line matching the engine's own grammar -> feed that SAME line back ->
// `.sound` event fires), own-echo dedup, and transcript append.
//
// SERIALIZATION: nested inside `EngineGlobalStateSelfTests` (.serialized),
// same posture as `WhisperRoutingTests`/`ChatSessionModelTests`/
// `OutboundEncodingTests` — every suite touching process-global engine state
// via ProtocolSession/ChatSessionModel lives here.
extension EngineGlobalStateSelfTests {
    @Suite(.serialized)
    struct SoundSendingTests {
        /// Polls `server.receivedBytes` until it contains `substring`, returning
        /// the accumulated c2s bytes split into lines — same shape as
        /// `WhisperRoutingTests`/`ChatSessionModelTests`'s own helper.
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

        /// The true round-trip: `sendSound` puts the exact VECTOR-17-shaped
        /// CTCP on the wire (`cc_session_send_sound`'s comicchat.h doc
        /// comment cites the grammar byte-for-byte), and feeding that same
        /// captured line back into the session (as a peer's own PRIVMSG)
        /// fires a real `.sound` event through the engine's OWN
        /// `ccPrepareSound` parser — not just a hand-typed literal on either
        /// side.
        @Test(.timeLimit(.minutes(1)))
        func sendSoundRoundTripsThroughEngineWireGrammar() async throws {
            let server = try LoopbackIRCServer()
            let art = repoRoot5Up().appendingPathComponent("v2.5-beta-1-modern/comicart").path
            let model = ChatSessionModel(config: .init(host: "127.0.0.1", port: server.port,
                                                       nick: "Mac", room: "#p4", artDir: art))
            try await model.start()
            try await server.replyToProbeWith451ThenWelcomeAndJoin(nick: "Mac", channel: "#p4")

            try await model.sendSound(file: "boing.wav")

            let sent = try await waitForReceivedLine(server, containing: "SOUND")
            guard let wireLine = sent.first(where: { $0.hasPrefix("PRIVMSG #p4 :") && $0.contains("SOUND") }) else {
                Issue.record("expected a captured PRIVMSG #p4 wire line containing the SOUND CTCP")
                return
            }
            // Exact wire shape (comicchat.h's cc_session_send_sound doc
            // comment): "\x01SOUND \"<file>\" <text>\x01" as the message.
            #expect(wireLine == "PRIVMSG #p4 :\u{01}SOUND \"boing.wav\" \u{01}")

            // Feed the REAL captured wire line back, framed as a peer's own
            // PRIVMSG, exactly as a server that echoes a client's own PRIVMSG
            // would.
            try await server.send(":Bob!bob@h \(wireLine)")

            let soundBox = SoundReceivedBox()
            model.onSound = { nick, file in soundBox.append(nick: nick, file: file) }

            while !soundBox.entries.contains(where: { $0.nick == "Bob" && $0.file == "boing.wav" }) {
                try await Task.sleep(nanoseconds: 5_000_000)
            }
            #expect(soundBox.entries.contains { $0.nick == "Bob" && $0.file == "boing.wav" })

            model.shutdown()
            server.stop()
        }

        /// `sendSound` renders/plays the OWN line immediately via a synthetic
        /// `.sound` event through the same `onSound` callback path an inbound
        /// event takes (no separate playback call) — see that method's own
        /// doc comment.
        @Test(.timeLimit(.minutes(1)))
        func sendSoundFiresOwnSoundCallbackImmediately() async throws {
            let server = try LoopbackIRCServer()
            let art = repoRoot5Up().appendingPathComponent("v2.5-beta-1-modern/comicart").path
            let model = ChatSessionModel(config: .init(host: "127.0.0.1", port: server.port,
                                                       nick: "Mac", room: "#p4", artDir: art))
            let soundBox = SoundReceivedBox()
            model.onSound = { nick, file in soundBox.append(nick: nick, file: file) }
            try await model.start()
            try await server.replyToProbeWith451ThenWelcomeAndJoin(nick: "Mac", channel: "#p4")

            try await model.sendSound(file: "boing.wav")

            while soundBox.entries.isEmpty {
                try await Task.sleep(nanoseconds: 5_000_000)
            }
            #expect(soundBox.entries.contains { $0.nick == "Mac" && $0.file == "boing.wav" })

            model.shutdown()
            server.stop()
        }

        /// Own-echo no-double-play: if the server echoes our own SOUND
        /// PRIVMSG back to us, `handleLocked`'s `pendingLocalSoundEchoes`
        /// dedup (same shape as the pre-existing own-say dedup) must drop it
        /// — `onSound` must fire EXACTLY ONCE for the own send, not twice.
        @Test(.timeLimit(.minutes(1)))
        func ownSoundEchoIsDedupedNotDoublePlayed() async throws {
            let server = try LoopbackIRCServer()
            let art = repoRoot5Up().appendingPathComponent("v2.5-beta-1-modern/comicart").path
            let model = ChatSessionModel(config: .init(host: "127.0.0.1", port: server.port,
                                                       nick: "Mac", room: "#p4", artDir: art))
            let soundBox = SoundReceivedBox()
            model.onSound = { nick, file in soundBox.append(nick: nick, file: file) }
            try await model.start()
            try await server.replyToProbeWith451ThenWelcomeAndJoin(nick: "Mac", channel: "#p4")

            try await model.sendSound(file: "boing.wav")

            let sent = try await waitForReceivedLine(server, containing: "SOUND")
            guard let wireLine = sent.first(where: { $0.hasPrefix("PRIVMSG #p4 :") && $0.contains("SOUND") }) else {
                Issue.record("expected a captured PRIVMSG #p4 wire line containing the SOUND CTCP")
                return
            }
            // Echo it back framed as OUR OWN nick (the server-echo case the
            // dedup exists for), not a different peer's.
            try await server.send(":Mac!mac@h \(wireLine)")

            func ownMatchCount() -> Int {
                soundBox.entries.filter { $0.nick == "Mac" && $0.file == "boing.wav" }.count
            }
            while ownMatchCount() < 1 {
                try await Task.sleep(nanoseconds: 5_000_000)
            }
            // Settling window long enough for a double-fire (pre-fix-style
            // regression) to have landed, before asserting the final count.
            try await Task.sleep(nanoseconds: 200_000_000)
            #expect(ownMatchCount() == 1, "expected exactly one own-sound callback, got \(ownMatchCount())")

            model.shutdown()
            server.stop()
        }
    }
}

/// Thread-safe accumulator for `onSound`'s `(nick, file)` callback — same
/// lock-guarded-box shape as `WhisperRoutingTests`' `WhisperReceivedBox`.
private final class SoundReceivedBox: @unchecked Sendable {
    private let lock = NSLock()
    private var storage: [(nick: String, file: String)] = []

    func append(nick: String, file: String) {
        lock.lock(); defer { lock.unlock() }
        storage.append((nick, file))
    }

    var entries: [(nick: String, file: String)] {
        lock.lock(); defer { lock.unlock() }
        return storage
    }
}
