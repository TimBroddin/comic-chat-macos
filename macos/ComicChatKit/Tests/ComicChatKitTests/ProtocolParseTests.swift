import Testing
import Foundation
import cchat_engine
@testable import ComicChatKit

// Plan 3 Task 8 (spec §8.2): replay REAL captured bytes from the genuine 1998
// cchat.exe client (via the Wine capture rig,
// docs/superpowers/plans/2026-07-18-plan3-discovery/wine-capture-rig.md)
// through ProtocolSession/the C engine's parser, and assert on the resulting
// event stream. This is a wire-level interop proof, not a synthetic fixture:
// every byte replayed here is exactly what a real, unmodified Comic Chat 2.5
// client sent/received against ngircd.
//
// SERIALIZATION: nested inside EngineGlobalStateSelfTests (declared
// .serialized in BodyDrawTests.swift), same reasoning as
// ProtocolSessionTests -- cc_session activates a file-static g_session on
// entry to every cc_session_* call, so two of these can't run concurrently.
extension EngineGlobalStateSelfTests {
    @Suite(.serialized)
    struct ProtocolParseTests {
        /// The committed capture fixture (Fixtures/captures/smoke-2.jsonl,
        /// copied verbatim from the rig's `.superpowers/rig/captures/`,
        /// loopback-only bytes, no secrets). Contains TWO back-to-back
        /// sessions the rig captured in one run: an early scripted
        /// `smoketest` smoke check (IRC nick "smoketest", plain PRIVMSG, no
        /// IRCX probe), then the genuine `cchat.exe` connect handshake (nick
        /// "Anonymous") that this test targets -- see `session2Slice()`.
        private func loadCapture() throws -> [CaptureEvent] {
            let url = try #require(Bundle.module.url(
                forResource: "smoke-2", withExtension: "jsonl", subdirectory: "Fixtures/captures"))
            return try CaptureEvent.parse(jsonlFile: url)
        }

        /// Slices the fixture down to the second captured session (the real
        /// `cchat.exe` client) by finding the rig's own "client 2 connected"
        /// meta marker -- robust to any future curation/reordering of the
        /// file, rather than hardcoding a line count.
        private func session2Slice(_ capture: [CaptureEvent]) throws -> [CaptureEvent] {
            guard let markerIndex = capture.firstIndex(where: {
                $0.direction == .meta && $0.latin1.contains("client 2 connected")
            }) else {
                throw ProtocolParseTestsError.missingSessionMarker
            }
            return Array(capture[(markerIndex + 1)...])
        }

        /// Further splits the session-2 slice into "up to and including the
        /// 001-welcome chunk" and "everything after" -- the real cchat.exe
        /// client sends `JOIN #comicrig` (a c2s line, not replayed here)
        /// immediately after 001, and `ProtocolSession.join(_:)` must be
        /// called at that same point in THIS replay (registering the
        /// room_token<->channel mapping) before the JOIN-echo chunk arrives,
        /// or `.selfJoined` can never resolve a room token -- see
        /// `ProtocolSession.join`'s doc comment and `CaptureReplayCursor`'s
        /// doc comment (Support/CaptureReplay.swift) for why this split is
        /// necessary rather than an interleave callback keyed off "a chunk
        /// was just sent".
        private func splitAtLoginChunk(_ capture: [CaptureEvent]) throws -> (upToLogin: [CaptureEvent], rest: [CaptureEvent]) {
            guard let loginIndex = capture.firstIndex(where: {
                $0.direction == .s2c && $0.latin1.contains(" 001 ")
            }) else {
                throw ProtocolParseTestsError.missingLoginChunk
            }
            return (Array(capture[...loginIndex]), Array(capture[(loginIndex + 1)...]))
        }

        private enum ProtocolParseTestsError: Error, CustomStringConvertible {
            case missingSessionMarker
            case missingLoginChunk
            var description: String {
                switch self {
                case .missingSessionMarker:
                    return "smoke-2.jsonl fixture is missing its \"client 2 connected\" meta marker -- fixture layout changed?"
                case .missingLoginChunk:
                    return "smoke-2.jsonl fixture is missing an s2c chunk containing \" 001 \" -- fixture layout changed?"
                }
            }
        }

        // brief Step 3/4: replay smoke-2's real connect handshake and assert
        // the event stream includes .loggedIn(nick: "Anonymous"), a
        // .selfJoined("#comicrig"), and no unexpected .error events.
        //
        // This is the real cchat.exe wire sequence (wine-capture-rig.md):
        //   c2s  MODE ISIRCX                    <- IRCX probe, first bytes
        //   s2c  451 * :Connection not registered
        //   c2s  NICK Anonymous / USER Anonymous ... <- plain-IRC fallback
        //   s2c  001..004 / 005 / 251..266 / 375..376 <- registration + MOTD
        //   c2s  MODE Anonymous -i
        //   c2s  JOIN #comicrig
        //   s2c  JOIN echo / 332 / 333 / 353 / 366
        //   c2s  MODE #comicrig / WHO #comicrig
        //   s2c  324 / 329 / 352 / 315
        //   s2c  PING -> c2s PONG (x2)
        //   s2c  NOTICE + ERROR (server shutdown)
        // The parser never sees the c2s lines (those are what the real
        // client sent to the real ngircd) -- only s2c bytes are replayed
        // here, exactly as the brief specifies ("hex is authoritative for
        // the byte-compare tests; feed these bytes to the parser").
        @Test(.timeLimit(.minutes(1)))
        func smoke2RealCaptureReplayProducesExpectedEvents() async throws {
            let capture = try session2Slice(try loadCapture())
            #expect(capture.contains { $0.direction == .s2c })
            #expect(capture.contains { $0.direction == .c2s })
            let (upToLogin, rest) = try splitAtLoginChunk(capture)

            let server = try LoopbackIRCServer()
            let session = ProtocolSession(host: "127.0.0.1", port: server.port, nick: "Anonymous", encoding: .cp1252)
            try await session.connect()

            var cursor = CaptureReplayCursor(session: session, server: server)
            try await cursor.send(upToLogin)
            _ = try await cursor.collectUntil { if case .loggedIn = $0 { return true } else { return false } }

            // Real client's own behavior at exactly this point in the wire
            // sequence (see splitAtLoginChunk's doc comment): register the
            // room_token<->channel mapping before the server's JOIN-echo
            // chunk (sent next, in `rest`) arrives.
            try await session.join("#comicrig")

            try await cursor.send(rest)
            _ = try await cursor.collectUntil { ev in
                if case .selfJoined(let channel) = ev { return channel == "#comicrig" }
                return false
            }

            let seen = cursor.allEvents
            #expect(seen.contains { if case .loggedIn(let nick) = $0 { return nick == "Anonymous" } else { return false } },
                     "expected .loggedIn(nick: \"Anonymous\") in the replayed event stream; saw: \(seen)")
            #expect(seen.contains { if case .selfJoined(let channel) = $0 { return channel == "#comicrig" } else { return false } },
                     "expected .selfJoined(\"#comicrig\") in the replayed event stream; saw: \(seen)")

            let unexpectedErrors = seen.compactMap { ev -> String? in
                if case .error(let code, let text) = ev { return "error(\(code), \(text))" }
                return nil
            }
            #expect(unexpectedErrors.isEmpty,
                     "expected no .error events replaying the real client's connect handshake; saw: \(unexpectedErrors)")

            session.disconnect()
            server.stop()
        }

        // Companion assertion: the real capture's NAMES-353 reply for
        // #comicrig lists exactly "Anonymous" (wine-capture-rig.md's
        // transcript), so after the replay above, ProtocolSession's tracked
        // room state should show Anonymous as a member of #comicrig --
        // exercising the state-and-codec.md §1.4 membership tracking against
        // a real client's wire bytes, not just the raw event sequence.
        @Test(.timeLimit(.minutes(1)))
        func smoke2RealCaptureReplayPopulatesRoomMembership() async throws {
            let capture = try session2Slice(try loadCapture())
            let (upToLogin, rest) = try splitAtLoginChunk(capture)

            let server = try LoopbackIRCServer()
            let session = ProtocolSession(host: "127.0.0.1", port: server.port, nick: "Anonymous", encoding: .cp1252)
            try await session.connect()

            var cursor = CaptureReplayCursor(session: session, server: server)
            try await cursor.send(upToLogin)
            _ = try await cursor.collectUntil { if case .loggedIn = $0 { return true } else { return false } }

            try await session.join("#comicrig")

            try await cursor.send(rest)
            _ = try await cursor.collectUntil { ev in
                if case .endOfNames(let channel) = ev { return channel == "#comicrig" }
                return false
            }

            let members = session.members
            #expect(members["#comicrig"]?.contains("Anonymous") == true,
                     "expected Anonymous to be tracked as a member of #comicrig after replaying the real NAMES-353 reply; rooms: \(members)")

            session.disconnect()
            server.stop()
        }
    }
}
