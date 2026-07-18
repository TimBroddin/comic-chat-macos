import Testing
import Foundation
@testable import ComicChatKit

// Plan 4b Task 1 — §8 prerequisite: outbound text (`say`/`whisper`/`setTopic`)
// must encode per the session's configured `WireEncoding`, not assume UTF-8.
// Before this task, `ProtocolSession.say`/`whisper`/`setTopic` called plain
// `String.withCString`, which ALWAYS encodes as UTF-8 regardless of
// `encoding` — so a `.cp1252` session sending "café" put UTF-8 bytes
// (0xC3 0xA9) on the wire instead of CP-1252's single-byte 0xE9. Real 1998
// clients (and any CP-1252 peer) expect the latter. `announceAvatar` already
// used the `withEncodedCString`/`withOptionalEncodedCString` helpers
// correctly (see that method's doc comment) — this task brings `say`/
// `whisper`/`setTopic` in line.
//
// SERIALIZATION (same reasoning as every other suite touching process-global
// engine state via ProtocolSession/cc_session): nested inside
// EngineGlobalStateSelfTests (.serialized), matching LoginSequencingTests'
// established scaffold exactly (connect -> plain-IRC 451 fallback -> login ->
// join -> exercise the call under test -> read the server's received-bytes
// log).
extension EngineGlobalStateSelfTests {
    @Suite(.serialized) struct OutboundEncodingTests {
        /// Polls `server.receivedBytes` until it contains `substring` — same
        /// helper shape as `LoginSequencingTests.waitForReceivedBytes`
        /// (duplicated here rather than shared: that one is `private` to its
        /// own suite).
        private func waitForReceivedBytes(
            _ server: LoopbackIRCServer,
            containing substring: String
        ) async throws -> String {
            while true {
                let text = String(data: server.receivedBytes, encoding: .isoLatin1) ?? ""
                if text.contains(substring) { return text }
                try await Task.sleep(nanoseconds: 5_000_000)
            }
        }

        /// Connects, drives the plain-IRC login fallback (451 -> NICK/USER ->
        /// 001, LoginSequencingTests.plainIrcFallback's scenario), then joins
        /// "#t" and waits for the server's JOIN echo — the shared preamble
        /// every test below needs before exercising the call under test.
        private func connectLoginAndJoin(_ server: LoopbackIRCServer, session: ProtocolSession) async throws {
            try await session.connect()
            await server.startCollectingReceivedBytes()
            _ = try await waitForReceivedBytes(server, containing: "MODE ISIRCX\r\n")

            try await server.send(":srv 451 * :not registered")
            _ = try await waitForReceivedBytes(server, containing: "USER ")
            try await server.send(":srv 001 Mac :Welcome")

            // `.loggedIn` (which flips `connectionStatus` to `.connected`) is
            // processed asynchronously on the session's own engine queue —
            // wait for it before calling `join`, which is gated on that same
            // status (CX_DISCONNECTED guard, `onQueueGated`).
            while session.currentConnectionStatus != .connected {
                try await Task.sleep(nanoseconds: 5_000_000)
            }

            try await session.join("#t")
            _ = try await waitForReceivedBytes(server, containing: "JOIN #t\r\n")
            try await server.send(
                ":Mac!mac@h JOIN :#t",
                ":srv 353 Mac = #t :Mac",
                ":srv 366 Mac #t :End of NAMES list")
        }

        // say(): "café" over a .cp1252 session must hit the wire as 0xE9, not
        // UTF-8 0xC3 0xA9.
        @Test(.timeLimit(.minutes(1)))
        func sayEncodesCP1252() async throws {
            let server = try LoopbackIRCServer()
            let session = ProtocolSession(host: "127.0.0.1", port: server.port, nick: "Mac", encoding: .cp1252)
            try await connectLoginAndJoin(server, session: session)

            try await session.say("#t", text: "café", annotations: nil)
            _ = try await waitForReceivedBytes(server, containing: "PRIVMSG")

            let bytes = server.receivedBytes
            #expect(bytes.contains([0x63, 0x61, 0x66, 0xE9]))          // c a f é(CP-1252)
            #expect(!bytes.contains([0xC3, 0xA9]))                     // no UTF-8 é

            session.disconnect()
            server.stop()
        }

        @Test(.timeLimit(.minutes(1)))
        func whisperEncodesCP1252() async throws {
            let server = try LoopbackIRCServer()
            let session = ProtocolSession(host: "127.0.0.1", port: server.port, nick: "Mac", encoding: .cp1252)
            try await connectLoginAndJoin(server, session: session)

            try await session.whisper(to: ["Bob"], text: "café", channel: "#t")
            _ = try await waitForReceivedBytes(server, containing: "PRIVMSG")

            let bytes = server.receivedBytes
            #expect(bytes.contains([0x63, 0x61, 0x66, 0xE9]))          // c a f é(CP-1252)
            #expect(!bytes.contains([0xC3, 0xA9]))                     // no UTF-8 é

            session.disconnect()
            server.stop()
        }

        @Test(.timeLimit(.minutes(1)))
        func topicEncodesCP1252() async throws {
            let server = try LoopbackIRCServer()
            let session = ProtocolSession(host: "127.0.0.1", port: server.port, nick: "Mac", encoding: .cp1252)
            try await connectLoginAndJoin(server, session: session)

            try await session.setTopic("#t", topic: "café")
            _ = try await waitForReceivedBytes(server, containing: "TOPIC")

            let bytes = server.receivedBytes
            #expect(bytes.contains([0x63, 0x61, 0x66, 0xE9]))          // c a f é(CP-1252)
            #expect(!bytes.contains([0xC3, 0xA9]))                     // no UTF-8 é

            session.disconnect()
            server.stop()
        }
    }
}
