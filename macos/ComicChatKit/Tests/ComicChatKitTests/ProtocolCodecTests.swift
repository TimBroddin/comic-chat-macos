import Testing
import Foundation
import cchat_engine
@testable import ComicChatKit

// Plan 3 Task 8 (spec §8.2): decode a captured annotation blob, re-encode it
// via the engine's real outbound builder, and BYTE-COMPARE the result
// against the captured bytes. This freezes wire-level codec fidelity for the
// "#G<gp><ge><gi>E<ep><ee><ei>[R]M<m>[T<nick>,...]" annotation grammar
// (state-and-codec.md §3.3) across a full decode -> re-encode round trip.
//
// *** HAND-AUTHORED FIXTURE, NOT A REAL CAPTURE ***
// The Wine capture rig (docs/superpowers/plans/2026-07-18-plan3-discovery/
// wine-capture-rig.md) has NOT yet captured a real annotated comic message
// (a PRIVMSG carrying a "(#...)" prefix, or a "DATA ... CCUDI1" line) -- that
// needs GUI interaction (typing into the compose bar with a pose/emotion
// selected) the headless discovery session couldn't perform. So this test
// runs against Fixtures/captures/hand-authored-annotation.jsonl, a small
// capture-SHAPED fixture this task constructed by hand, byte-exact to the
// documented grammar (state-and-codec.md §3.3) and to the real encoder's
// output (ircproto.cpp:554-556's "(#...) <text>" plain-IRC transport,
// verified against bridge/cc_session.cpp's ccEncodeAnnotations, which
// reproduces protsupp.cpp:3057-3099's bInsertAnnotations sprintf format
// byte-for-byte) -- NOT verbatim bytes from the real cchat.exe client. This
// is marked here explicitly rather than silently treated as a real capture.
// STRENGTHENING FOLLOW-UP: replace/augment this fixture with a genuine
// annotated capture once one exists (an interactive rig session per
// wine-capture-rig.md's "Interactive (Tim)" recipe, or a scripted second
// IRC client the real cchat.exe annotates messages to -- flagged as
// execution-task work for Task 9 or a dedicated capture campaign, not
// discovery). When that capture lands, this test's fixture should be
// swapped for the real one (or a new test added alongside it) so the
// byte-compare is anchored to genuine 1998-client wire bytes, not a
// hand-verified reconstruction of the documented grammar.
//
// ACCENTED-BYTE GATE (Task 2 review): CharUpperBuff case-folds ASCII only;
// CP-1252 bytes >= 0x80 pass through unchanged vs. the real client's
// uppercasing. This fixture is pure ASCII throughout (nicks "Anon"/"Bob",
// channel "#comicrig", message text "hello") -- no accented bytes appear
// anywhere a capitalization path could diverge from the real client.
//
// SERIALIZATION: nested inside EngineGlobalStateSelfTests (.serialized in
// BodyDrawTests.swift), same reasoning as ProtocolSessionTests/ProtocolParseTests.
extension EngineGlobalStateSelfTests {
    @Suite(.serialized)
    struct ProtocolCodecTests {
        private func loadFixture() throws -> [CaptureEvent] {
            let url = try #require(Bundle.module.url(
                forResource: "hand-authored-annotation", withExtension: "jsonl", subdirectory: "Fixtures/captures"))
            return try CaptureEvent.parse(jsonlFile: url)
        }

        // Decode the hand-authored ":Bob!bob@h PRIVMSG #comicrig
        // :(#G295E193M1) hello\r\n" line (plain-IRC transport, parenthesized
        // annotation block -- ircproto.cpp:554-556) through a real
        // ProtocolSession, then re-encode the DECODED Annotations via
        // `session.say(...)` (which drives cc_session_send_say ->
        // ccEncodeAnnotations, the public builder that reproduces
        // bInsertAnnotations's sprintf format byte-for-byte -- see
        // bridge/cc_session.cpp's doc comment on that function) and
        // byte-compare the outbound wire bytes the session actually sent
        // against the same "(#G295E193M1) hello" text.
        //
        // "#G295E193M1" wire arithmetic (IndexToByte(v) = v + '0',
        // protsupp.cpp:1023), matching cc_selftest_annotation_codec's own
        // hand-verified vector:
        //   gesture_pose=2, gesture_emotion=9 (EM_NEUTRAL), gesture_intensity=5
        //   face_pose=1,    face_emotion=9 (EM_NEUTRAL),    face_intensity=3
        //   mode=1 (SM_SAY)
        // This byte-compare only needs DECODE to be exact for these values
        // (it is -- ProcessUDIData's ByteToIndex is a direct, unambiguous
        // inverse) and ENCODE to reproduce the SAME already-decoded index
        // values byte-for-byte -- it does NOT require the EmotionToBytes
        // encode-from-a-live-avatar path (which has the documented
        // EM_HAPPY/EM_NEUTRAL collision bug, see cc_selftest.cpp's comment
        // above cc_selftest_annotation_codec) because
        // ccEncodeAnnotations/cc_session_send_say re-encodes the DECODED
        // index values directly, never re-deriving them from a live avatar
        // object -- see comicchat.h's cc_session_send_say doc comment and
        // bridge/cc_session.cpp's ccEncodeAnnotations comment for why this
        // is the correct, fidelity-preserving choice for this boundary.
        @Test(.timeLimit(.minutes(1)))
        func annotationDecodeReencodeByteCompare() async throws {
            let fixture = try loadFixture()
            let s2cChunks = fixture.filter { $0.direction == .s2c }
            #expect(s2cChunks.count == 3, "expected exactly 3 s2c chunks in the hand-authored fixture (login, join, annotated say); got \(s2cChunks.count)")

            let server = try LoopbackIRCServer()
            let session = ProtocolSession(host: "127.0.0.1", port: server.port, nick: "Anon", encoding: .cp1252)
            try await session.connect()
            // Start collecting from the very beginning (rather than right
            // before `say()`): the outbound JOIN/MODE/WHO bytes `session.join`
            // triggers below can already be sitting in the server-side
            // kernel socket buffer by the time a later `startCollecting...`
            // call arms the first `receive()` -- NWConnection's first
            // `receive()` call picks up whatever the kernel already buffered
            // regardless of when `receive()` itself was invoked, so starting
            // late does not actually exclude those earlier bytes. Collecting
            // from the start and checking the tail of the accumulated stream
            // (`hasSuffix`, below) is the reliable way to isolate "what did
            // THIS re-encode call send" from everything sent before it.
            await server.startCollectingReceivedBytes()

            var cursor = CaptureReplayCursor(session: session, server: server)

            // Chunk 0: 001 welcome.
            try await cursor.send([s2cChunks[0]])
            _ = try await cursor.collectUntil { if case .loggedIn = $0 { return true } else { return false } }
            try await session.join("#comicrig")

            // Chunk 1: JOIN echo + 353 + 366.
            try await cursor.send([s2cChunks[1]])
            _ = try await cursor.collectUntil { ev in
                if case .endOfNames(let channel) = ev { return channel == "#comicrig" }
                return false
            }

            // Chunk 2: Bob's annotated PRIVMSG -- the blob under test.
            try await cursor.send([s2cChunks[2]])
            let sayEvents = try await cursor.collectUntil { ev in
                if case .text(let nick, _, _, _, _, let annotations) = ev { return nick == "Bob" && annotations != nil }
                return false
            }

            let decoded: Annotations = try #require(sayEvents.compactMap { ev -> Annotations? in
                if case .text(let nick, _, _, let text, _, let annotations) = ev, nick == "Bob", text == "hello" {
                    return annotations
                }
                return nil
            }.first, "expected a decoded .text event for Bob's annotated \"hello\" message; saw: \(sayEvents)")

            // DECODE assertions -- exact index/emotion/mode values the
            // "#G295E193M1" blob encodes (matches cc_selftest_annotation_codec).
            #expect(decoded.gesturePose == 2)
            #expect(decoded.gestureEmotion == 9)
            #expect(decoded.gestureIntensity == 5)
            #expect(decoded.facePose == 1)
            #expect(decoded.faceEmotion == 9)
            #expect(decoded.faceIntensity == 3)
            #expect(decoded.mode == 1)
            #expect(decoded.addressees.isEmpty)

            // RE-ENCODE: say the same text with the decoded annotations
            // attached, through the real cc_session_send_say builder.
            try await session.say("#comicrig", text: "hello", annotations: decoded)

            // BYTE-COMPARE: what the session actually sent to the wire must
            // END WITH bytes byte-identical to the fixture's own
            // "(#G295E193M1) hello" text -- full PRIVMSG line, since
            // IsIRCX() is false in this plain-IRC-fallback session (no IRCX
            // negotiation occurred), matching bChatSendToTarget's transport
            // selection (ircproto.cpp:574, "if (*szAnnotations && IsIRCX())"
            // is false here, so the parenthesized-prefix branch at :588 is
            // taken). Checking the SUFFIX (not full equality against
            // `receivedBytes`) is deliberate: the session also sent
            // JOIN/MODE/WHO earlier (triggered by this test's own
            // `session.join` call above) which legitimately precede the
            // re-encoded line in the same accumulated byte stream -- see
            // `startCollectingReceivedBytes`'s call site comment above for
            // why collection can't be started late enough to exclude them.
            let expectedLine = "PRIVMSG #comicrig :(#G295E193M1) hello\r\n"
            let sentBytes = try await waitForBytes(server, suffix: Data(expectedLine.utf8))
            let sentText = String(decoding: sentBytes, as: UTF8.self)

            #expect(sentText.hasSuffix(expectedLine),
                     "re-encoded outbound bytes do not byte-compare against the fixture's annotation grammar; got \(String(reflecting: sentText)), expected a suffix of \(String(reflecting: expectedLine))")

            session.disconnect()
            server.stop()
        }

        /// Polls `server.receivedBytes` until the accumulated stream ends
        /// with `suffix` or a bounded number of short waits elapse
        /// (backstopped by the calling test's own `.timeLimit` trait).
        /// `sendRaw`/`say` completing only means the WRITE was handed off on
        /// one side of the loopback socket, not that the OTHER side's read
        /// callback has fired yet -- the same real socket-timing gap
        /// `CaptureReplayCursor`'s doc comment documents for the send
        /// direction, mirrored here for the receive-and-verify direction.
        private func waitForBytes(_ server: LoopbackIRCServer, suffix: Data) async throws -> Data {
            for _ in 0..<200 {
                let bytes = server.receivedBytes
                if bytes.count >= suffix.count && bytes.suffix(suffix.count) == suffix { return bytes }
                try await Task.sleep(for: .milliseconds(50))
            }
            throw ProtocolCodecTestsError.timedOutWaitingForBytes(got: server.receivedBytes)
        }

        private enum ProtocolCodecTestsError: Error, CustomStringConvertible {
            case timedOutWaitingForBytes(got: Data)
            var description: String {
                switch self {
                case .timedOutWaitingForBytes(let got):
                    return "ProtocolCodecTests: timed out waiting for outbound bytes; got so far: \(String(decoding: got, as: UTF8.self))"
                }
            }
        }
    }
}
