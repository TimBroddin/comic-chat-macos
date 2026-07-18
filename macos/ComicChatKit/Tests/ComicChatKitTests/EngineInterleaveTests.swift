import Testing
import Foundation
import CoreGraphics
import cchat_engine
@testable import ComicChatKit

// Plan 4a Task 1 — THE HIGHEST-RISK UNKNOWN (D1 R1, D2 top-risk 1): Plan 3
// only ever ran drain-then-render (collect the FULL event stream first, THEN
// feed it into ProtocolStripBridge — see ProtocolToStripTests.swift and the
// old ProtocolStripBridge.swift:9-22 contract comment). A live app CANNOT do
// that: it must call `cc_strip_*` (via ProtocolStripBridge) WHILE a
// ProtocolSession is still mid-session, i.e. interleaved with
// `cc_session_feed_bytes` on the very same process-global, single-threaded
// engine. That interleaving has never been executed before this test.
//
// This test drives ONE scripted conversation two ways and byte-compares the
// resulting PNGs:
//   - `interleaved: false` — the Plan 3 reference: drain the whole session to
//     completion, THEN build/compose the strip.
//   - `interleaved: true`  — the live-app shape: apply each event to the
//     strip (via `session.performOnEngineQueue`) as soon as it arrives, while
//     the session is still connected/mid-conversation.
// Byte-identical PNGs prove that serializing every `cc_session_*` AND every
// `cc_strip_*` call through the SAME serial queue (`ProtocolSession`'s
// `sessionQueue`, now exposed as the "engine queue" via
// `performOnEngineQueue`/`enqueueEngineWork`) is safe — i.e. the shared
// process-global engine state tolerates interleaving as long as there is only
// ever one call in flight at a time, which is exactly what one serial queue
// guarantees regardless of which "half" (session vs strip) the call belongs
// to.
//
// SERIALIZATION (load-bearing, same reasoning as StripTests/ProtocolSessionTests/
// ProtocolToStripTests): nested inside EngineGlobalStateSelfTests (.serialized,
// declared in BodyDrawTests.swift) so this suite never races the C selftests,
// the Strip tests, or the protocol tests over the same process-global engine
// state (avatar registry, metrics canvas, cc_session's file-static g_session).
extension EngineGlobalStateSelfTests {
  @Suite(.serialized)
  struct EngineInterleaveTests {
    /// Advance `iterator` until the next event, throwing if the stream ends
    /// first (mirrors ProtocolSessionTests's own `collectUntil` shape, scoped
    /// down to "give me exactly one event").
    private struct StreamEndedError: Error, CustomStringConvertible {
        var description: String { "event stream ended before expected event arrived" }
    }

    private func nextEvent(_ iterator: inout AsyncStream<ProtocolEvent>.AsyncIterator) async throws -> ProtocolEvent {
        guard let ev = await iterator.next() else { throw StreamEndedError() }
        return ev
    }

    /// Drives one scripted conversation over a real loopback socket and
    /// returns the rendered PNG bytes. `interleaved: false` is the Plan 3
    /// drain-then-render reference; `interleaved: true` applies each event to
    /// the strip (on the engine queue) mid-session, as a live app would.
    private func runConversation(interleaved: Bool) async throws -> Data {
        let server = try LoopbackIRCServer()
        let session = ProtocolSession(host: "127.0.0.1", port: server.port,
                                      nick: "Mac", encoding: .cp1252)
        try await session.connect()

        let root = repoRoot5Up()
        let art = root.appendingPathComponent("v2.5-beta-1-modern/comicart").path

        var bridge: ProtocolStripBridge? = interleaved
            ? try ProtocolStripBridge(resolver: .init(comicartDir: art)) : nil

        var iterator = session.events.makeAsyncIterator()

        try await server.send(":srv 001 Mac :Welcome")
        _ = try await nextEvent(&iterator)   // .loggedIn

        try await session.join("#p4")
        try await server.send(
            ":Mac!u@h JOIN #p4",
            ":srv 353 Mac = #p4 :Mac")
        // Drain until .selfJoined is observed (NAMES/end-of-names may also
        // arrive; only .selfJoined is load-bearing for this test).
        while true {
            let ev = try await nextEvent(&iterator)
            if case .selfJoined = ev { break }
        }

        // Two annotated says from a peer + one plain line — one `.text` event
        // per scripted server line.
        let lines = [
            ":Win!u@h PRIVMSG #p4 :(#G295E193M1)hello there",
            ":Win!u@h PRIVMSG #p4 :(#G012E345M1)second panel",
            ":Win!u@h PRIVMSG #p4 :plain trailer",
        ]
        var events: [ProtocolEvent] = []
        for line in lines {
            try await server.send(line)
            let ev = try await nextEvent(&iterator)
            events.append(ev)
            if interleaved, let bridge {
                // Mid-session strip work: the session is still connected (no
                // disconnect() yet) — this is the interleaving under test.
                session.performOnEngineQueue { try? bridge.apply(ev) }
            }
        }

        session.disconnect()

        if bridge == nil {
            // Reference: drain first (already done above — `events` holds the
            // full stream), THEN build/render, matching Plan 3's proven
            // drain-then-render shape exactly.
            let refBridge = try ProtocolStripBridge(resolver: .init(comicartDir: art))
            try refBridge.apply(events)
            bridge = refBridge
        }

        let (w, h) = bridge!.size
        let canvas = CGCanvas(widthTwips: w, heightTwips: h, scale: 2.0)
        return try session.performOnEngineQueue {
            try bridge!.compose(onto: canvas)
            return canvas.pngData() ?? Data()
        }
    }

    @Test(.timeLimit(.minutes(1)))
    func interleavedMatchesDrainThenRender() async throws {
        // `metricsBox` must outlive every engine call below (both
        // `runConversation`s do synchronous text-measurement callbacks into it
        // via Strip's layout) — same requirement `withExtendedLifetime`
        // documents elsewhere, satisfied here by ARC's own "kept alive until
        // last use" rule: its last use is the `#expect` block at the end of
        // this function, after both `await`s. (`withExtendedLifetime`'s
        // closure-based API only composes with a synchronous body, so it
        // can't wrap the async work directly the way other tests wrap a
        // synchronous `try withExtendedLifetime(metricsBox) { ... }` block.)
        let metricsCanvas = RecordingCanvas()
        let metricsBox = CanvasBox(metricsCanvas)
        cc_set_metrics_canvas(metricsBox.handle)

        let reference = try await runConversation(interleaved: false)
        let live = try await runConversation(interleaved: true)

        #expect(!reference.isEmpty)
        #expect(reference == live, """
            interleaved-strip-work PNG differs from drain-then-render reference PNG \
            (reference: \(reference.count) bytes, live: \(live.count) bytes) — \
            serialized interleaving of cc_session_* and cc_strip_* is NOT safe
            """)
        _ = metricsBox   // last use — keeps the box (and its installed metrics canvas) alive through both runConversation calls above
    }
  }
}
