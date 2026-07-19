import Testing
import Foundation
import cchat_engine
@testable import ComicChatKit

// Live-fix 3 (Plan 4b, crypthome.com live-reproduced): a real IRCX server
// answered our login attempt with `432 Anonymous :Reserved name` -- the
// engine already classifies this `CC_EV_NICK_REJECTED` (`ircsock.cpp`'s
// ERR_ERRONEUSNICKNAME/ERR_NICKNAMEINUSE handler), but `ChatSessionModel
// .handleLocked` had no case for it (falls through `default: break`) and the
// event's OWN 432 reply text (routed to `onStatus` via some other line
// elsewhere) is a transient status line that's easy to miss -- the session
// just dead-ends with no explanation. This wires `.nickRejected` to a
// dedicated `onNickRejected` callback carrying the rejected nick + a
// human-readable reason.
//
// SERIALIZATION: same reasoning as every other engine-touching suite (the C
// engine activates a process-global `g_session`/metrics-canvas per call) —
// nested inside `EngineGlobalStateSelfTests` (.serialized).
extension EngineGlobalStateSelfTests {
    @Suite(.serialized)
    struct NickRejectedTests {
        /// Loopback replies `432 Anonymous :Reserved name` to our login
        /// attempt (mirrors the exact live-reproduced wire form) — asserts
        /// `onNickRejected` fires with the bad nick and readable text.
        @Test(.timeLimit(.minutes(1)))
        func nickRejected432FiresCallback() async throws {
            let server = try LoopbackIRCServer()
            let art = repoRoot5Up().appendingPathComponent("v2.5-beta-1-modern/comicart").path
            let model = ChatSessionModel(config: .init(host: "127.0.0.1", port: server.port,
                                                       nick: "Anonymous", room: "#a", artDir: art))
            let box = NickRejectedBox()
            model.onNickRejected = { badNick, text in box.record(badNick: badNick, text: text) }

            try await model.start()

            try await server.startCollectingReceivedBytes()
            try await server.waitForClientLine(containing: "MODE ISIRCX\r\n")
            try await server.send(":srv 451 * :not registered")
            try await server.waitForClientLine(containing: "NICK Anonymous\r\n")
            // Matches the live-reproduced wire form exactly (brief: "crypthome
            // answered `432 Anonymous :Reserved name`") -- args[1] is the bad
            // nick (ircsock.cpp's ERR_ERRONEUSNICKNAME/432 handler uses index
            // 1, vs. index 2 for ERR_NICKNAMEINUSE/433's extra leading arg).
            try await server.send(":srv 432 Anonymous :Reserved name")

            try await pollUntil { box.badNick != nil }
            #expect(box.badNick == "Anonymous", "onNickRejected must carry the rejected nick, got \(String(describing: box.badNick))")
            #expect(box.text?.contains("Anonymous") == true,
                    "onNickRejected's text should mention the rejected nick, got \(String(describing: box.text))")

            model.shutdown()
            server.stop()
        }

        /// Polls `condition` (5ms cadence) until true; the test's own
        /// `.timeLimit` trait fails a never-true condition rather than hanging.
        private func pollUntil(_ condition: @escaping () -> Bool) async throws {
            while !condition() {
                try await Task.sleep(nanoseconds: 5_000_000)
            }
        }
    }
}

// Live-fix 4 (Tim's request: "show the MOTD like the original client"): the
// original client showed MOTD/server text in its status window; this port
// never surfaced `.motd` anywhere (`default: break` in `handleLocked`,
// pre-fix). Verifies the wire MOTD sequence (372 lines accumulate, 376 fires
// `CC_EV_MOTD`, the query cell the engine auto-enqueues right after 001 —
// `ircsock.cpp`'s RPL_WELCOME handler — is what makes 376 actually emit) ends
// up on `ChatSessionModel.onServerMessage`.
extension EngineGlobalStateSelfTests {
    @Suite(.serialized)
    struct ServerMessageTests {
        @Test(.timeLimit(.minutes(1)))
        func motdFiresOnServerMessageWithMotdText() async throws {
            let server = try LoopbackIRCServer()
            let art = repoRoot5Up().appendingPathComponent("v2.5-beta-1-modern/comicart").path
            let model = ChatSessionModel(config: .init(host: "127.0.0.1", port: server.port,
                                                       nick: "Mac", room: "#a", artDir: art))
            let box = ServerMessagesBox()
            model.onServerMessage = { text in box.append(text) }

            try await model.start()
            try await server.startCollectingReceivedBytes()
            try await server.waitForClientLine(containing: "MODE ISIRCX\r\n")
            try await server.send(":srv 451 * :not registered")
            try await server.waitForClientLine(containing: "USER ")
            try await server.send(
                ":srv 001 Mac :Welcome",
                ":srv 372 Mac :- Welcome to the crypthome network",
                ":srv 376 Mac :End of MOTD command")

            try await pollUntil { box.messages.contains { $0.contains("Welcome to the crypthome network") } }
            #expect(box.messages.contains { $0.contains("Welcome to the crypthome network") },
                    "onServerMessage must fire with the MOTD text, got \(box.messages)")

            model.shutdown()
            server.stop()
        }

        private func pollUntil(_ condition: @escaping () -> Bool) async throws {
            while !condition() {
                try await Task.sleep(nanoseconds: 5_000_000)
            }
        }
    }
}

/// Thread-safe accumulating server-messages box (mirrors
/// `AppState.serverMessages`'s own accumulate posture, at the Kit-test layer).
private final class ServerMessagesBox: @unchecked Sendable {
    private let lock = NSLock()
    private var storage: [String] = []
    func append(_ text: String) { lock.lock(); defer { lock.unlock() }; storage.append(text) }
    var messages: [String] { lock.lock(); defer { lock.unlock() }; return storage }
}

/// Thread-safe latest-nickRejected box (`onNickRejected` fires on the main
/// thread; the test body reads from its own task).
private final class NickRejectedBox: @unchecked Sendable {
    private let lock = NSLock()
    private var _badNick: String?
    private var _text: String?
    func record(badNick: String, text: String) {
        lock.lock(); defer { lock.unlock() }
        _badNick = badNick; _text = text
    }
    var badNick: String? { lock.lock(); defer { lock.unlock() }; return _badNick }
    var text: String? { lock.lock(); defer { lock.unlock() }; return _text }
}
