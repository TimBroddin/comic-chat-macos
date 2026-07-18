import Foundation
import cchat_engine
import ComicChatKit

// `cc-dumpart --replay <capture.jsonl> <out.png>`: Plan 3 Task 9's CLI
// exit-milestone driver. Parses a Wine capture rig JSONL file (Task 8's
// format — see CaptureFile.swift), feeds its `s2c` bytes to a real
// `ProtocolSession` over a `LoopbackReplayServer` (a real NWConnection round
// trip, the same seam the test suite's replay tests use), collects the FULL
// resulting `ProtocolEvent` stream, then — only after the session has been
// driven to completion and disconnected — bridges those events through
// `ProtocolStripBridge` into a `cc_strip` and PNG-exports the composed page.
//
// PHASE SEPARATION (binding, per the Task 9 brief): `cc_session_*` and
// `cc_strip_*` are two different engine C APIs, but BOTH mutate the SAME
// process-global engine state (comicchat.h's single-thread contract covers
// every `cc_*` call). Interleaving a `cc_session_feed_bytes` call (driven by
// the replay's receive loop, on `ProtocolSession`'s private serial
// `sessionQueue`) with a `cc_strip_*` call (driven by this function, on
// whatever queue/thread runs it) would be a real, unguarded data race on that
// shared global state -- not merely bad style. This function avoids that by
// construction: phase 1 (below) ONLY drives the session (collects every
// event into a plain `[ProtocolEvent]` array); ONLY AFTER phase 1 fully
// completes -- the session has been disconnected and its receive loop has
// stopped -- does phase 2 start touching `cc_strip_*` at all. No
// `cc_session_*` call and no `cc_strip_*` call are ever in flight at the same
// moment; the two phases don't even overlap in wall-clock time, let alone on
// the same call stack.

enum ReplayError: Error, CustomStringConvertible {
    case emptyStrip
    var description: String {
        switch self {
        case .emptyStrip:
            return "composed strip is empty (zero size) -- capture produced no renderable participants/lines"
        }
    }
}

/// Phase 1: replay `capture`'s `s2c` bytes into a fresh `ProtocolSession`
/// over an in-process `LoopbackReplayServer`, collecting every emitted
/// `ProtocolEvent` in order. Returns once the session has been disconnected
/// (a final synthetic `.disconnectedHint` this function itself triggers, if
/// the capture's own bytes didn't already produce one) -- i.e. once the
/// session's receive loop has definitely stopped touching `cc_session_*`.
func replayCaptureToEvents(_ lines: [CaptureLine]) async throws -> [ProtocolEvent] {
    let server = try LoopbackReplayServer()
    let session = ProtocolSession(host: "127.0.0.1", port: server.port,
                                  nick: "Anonymous", encoding: .cp1252)
    try await session.connect()

    var iterator = session.events.makeAsyncIterator()
    var events: [ProtocolEvent] = []

    // The real client always knows which channel it's about to join BEFORE
    // asking (ProtocolSession.join's own doc comment: the engine's inbound
    // JOIN/NAMES handlers only ever RESOLVE an already-registered room
    // token, never register one reactively) -- so, exactly like
    // ProtocolParseTests's real-capture replay, this driver must call
    // `session.join(channel)` once after `.loggedIn` and before the
    // JOIN-echo/NAMES chunks are fed, or `.selfJoined`'s room_token (and
    // everything room-scoped after it) never resolves. A capture doesn't
    // hand the channel name to us directly on the s2c side before that
    // point, but it DOES appear verbatim in the client's OWN c2s "JOIN
    // <channel>" line -- reading that (not sending it -- only s2c bytes ever
    // reach cc_session_feed_bytes) is how this driver learns what a real
    // client already knew going in.
    let channel = findJoinChannel(in: lines)

    let s2cChunks = lines.filter { $0.direction == .s2c }
    var sentCount = 0

    if let channel {
        // Send only up through the chunk containing "001" (login welcome) --
        // mirrors ProtocolParseTests.splitAtLoginChunk's two-phase split, so
        // `join` runs strictly after .loggedIn and strictly before whatever
        // chunk contains the JOIN echo.
        while sentCount < s2cChunks.count {
            let chunk = s2cChunks[sentCount]
            try await server.sendRaw(chunk.bytes)
            sentCount += 1
            if containsLoginWelcome(chunk.bytes) { break }
        }
        events.append(contentsOf: try await collectUntil(&iterator) {
            if case .loggedIn = $0 { return true } else { return false }
        })
        try await session.join(channel)
    }

    // Replay every remaining s2c chunk in order.
    while sentCount < s2cChunks.count {
        try await server.sendRaw(s2cChunks[sentCount].bytes)
        sentCount += 1
    }

    // SEQUENCING CAVEAT #1 (load-bearing -- the exact lesson CaptureReplay.swift's
    // own doc comment documents from Task 8): `server.sendRaw` awaiting only
    // means the SERVER side handed the bytes to the OS -- it says nothing
    // about whether the CLIENT (ProtocolSession's NWConnection) has actually
    // received and cc_session_feed_bytes-processed them yet. An earlier
    // version of this function called `session.disconnect()` immediately
    // after the loop above returned, racing the client's own receipt of the
    // LAST chunk just sent: `NWConnection.cancel()` could win, tearing down
    // the connection before the final PRIVMSG/annotation bytes were ever fed
    // to the engine (observed directly against the real hand-authored
    // annotation fixture: no "Got message" engine trace at all for the final
    // chunk, and no event ever emitted for it).
    //
    // SEQUENCING CAVEAT #2 (also load-bearing, found by direct observation --
    // NOT a documented Task 7/8 finding, so recorded here in full): waiting for
    // `session.disconnect()` to produce a terminal `.disconnectedHint` event
    // (the seemingly-obvious fix for caveat #1) is ALSO not reliable. Verified
    // directly: even after giving the client a generous grace period to fully
    // drain every event the sent bytes produce (confirmed via debug tracing --
    // Bob's annotated `.text` event DID arrive, with the correct decoded
    // Annotations, well within the grace window) and only THEN calling
    // `session.disconnect()`, `.disconnectedHint` still never arrived --
    // `NWConnection.cancel()` racing its own already-pending `receive()`
    // apparently does not always re-invoke that receive's completion handler
    // with an error on this platform, so `ProtocolSession.receiveLoop`'s
    // documented error-completion path (the only place that emits
    // `.disconnectedHint`) can simply never fire. A real capture that itself
    // ends in a server-sent ERROR line (smoke-2.jsonl) DOES produce
    // `.disconnectedHint` naturally from the wire bytes -- but a driver can't
    // assume every capture ends that way (the hand-authored fixture doesn't),
    // so waiting on that event unconditionally is not a safe general strategy.
    //
    // Fix: give the client a bounded grace period to drain whatever the sent
    // bytes produce, using a WALL-CLOCK DEADLINE rather than waiting for any
    // specific terminal event. `AsyncStream.AsyncIterator` is not `Sendable`
    // (Task 7/8's own documented constraint), which rules out racing
    // `iterator.next()` directly in a `TaskGroup` child task (tried first;
    // does not compile -- "escaping closure captures 'inout' parameter", "risks
    // causing data races") -- so instead of splitting the iterator across
    // tasks, `EventDrainBox` OWNS the iterator entirely inside one unstructured
    // background `Task` that keeps calling `iterator.next()` forever, pushing
    // each event into a lock-protected `@unchecked Sendable` box. The caller
    // (this function) never touches the iterator itself again after handing
    // it to the box -- it only polls the box's count across a `Task.sleep`
    // deadline from the OUTSIDE, which is safe precisely because the box, not
    // the iterator, is what crosses the concurrency boundary.
    let drainBox = EventDrainBox(iterator: iterator)
    try await Task.sleep(for: .milliseconds(300))
    var lastCount = drainBox.snapshot().count
    // Keep extending the grace window as long as new events keep arriving
    // (a slow-to-process capture still gets everything it produces); stop
    // once a full quiet period passes with no new event.
    while true {
        try await Task.sleep(for: .milliseconds(300))
        let snap = drainBox.snapshot()
        if snap.count == lastCount { break }
        lastCount = snap.count
    }
    events.append(contentsOf: drainBox.snapshot())
    session.disconnect()

    server.stop()
    return events
}

/// Owns an `AsyncStream<ProtocolEvent>.AsyncIterator` inside one unstructured
/// background `Task` (started at `init`) that drains it forever, appending
/// each event to `events` under `lock`. See the SEQUENCING CAVEAT #2 comment
/// at `replayCaptureToEvents` for why this exists: the iterator can't be
/// raced against a timeout directly (not `Sendable`), so instead this type
/// takes exclusive, permanent ownership of it and lets callers poll a
/// thread-safe snapshot from outside instead.
private final class EventDrainBox: @unchecked Sendable {
    private let lock = NSLock()
    private var events: [ProtocolEvent] = []

    init(iterator: AsyncStream<ProtocolEvent>.AsyncIterator) {
        // `AsyncStream<ProtocolEvent>.AsyncIterator` is not `Sendable`, which
        // makes the compiler's data-race checker reject capturing it in the
        // `Task` closure below even though, by construction, exactly one
        // place ever touches it (this Task body -- the outer `init` never
        // reads `iterator` again after this point, and `EventDrainBox` calls
        // `iterator.next()` from nowhere else). `nonisolated(unsafe)` records
        // that as a deliberate, verified exception rather than silencing the
        // check with a broader escape hatch.
        nonisolated(unsafe) var iterator = iterator
        Task {
            while let ev = await iterator.next() {
                if ProcessInfo.processInfo.environment["CC_REPLAY_DEBUG"] != nil {
                    FileHandle.standardError.write(Data("DEBUG DRAINED: \(ev)\n".utf8))
                }
                self.lock.withLock {
                    self.events.append(ev)
                }
            }
        }
    }

    func snapshot() -> [ProtocolEvent] {
        lock.withLock { events }
    }
}

/// Drains `iterator` until `predicate` matches (inclusive) or the stream
/// finishes (returns everything seen so far, without throwing -- a replay
/// driver should still render whatever it collected rather than fail outright
/// if a capture ends without ever producing the awaited event).
private func collectUntil(
    _ iterator: inout AsyncStream<ProtocolEvent>.AsyncIterator,
    matching predicate: (ProtocolEvent) -> Bool
) async throws -> [ProtocolEvent] {
    var collected: [ProtocolEvent] = []
    while let ev = await iterator.next() {
        if ProcessInfo.processInfo.environment["CC_REPLAY_DEBUG"] != nil {
            FileHandle.standardError.write(Data("DEBUG DRAINED: \(ev)\n".utf8))
        }
        collected.append(ev)
        if predicate(ev) { return collected }
    }
    return collected
}

/// Finds the channel this capture's client joins, so this driver can
/// pre-register it (`session.join`) before any room-scoped `s2c` reply that
/// depends on the room token being resolved (JOIN echo, NAMES, PRIVMSG, ...).
/// Prefers the client's own `c2s` `"JOIN <channel>"` command (the most
/// direct signal -- exactly what a real client already knows before asking);
/// falls back to scanning `s2c` for the first `"JOIN :<channel>"` (or
/// `"JOIN <channel>"`) IRC line -- covering captures with no `c2s` side at
/// all (e.g. a hand-authored s2c-only fixture) by reading the self-join echo
/// the server itself sends back. Returns `nil` if the capture never joins a
/// channel (e.g. a connect-only smoke capture).
private func findJoinChannel(in lines: [CaptureLine]) -> String? {
    for line in lines where line.direction == .c2s {
        for rawLine in line.latin1.components(separatedBy: "\r\n") {
            if rawLine.hasPrefix("JOIN ") {
                let rest = rawLine.dropFirst("JOIN ".count)
                let channel = rest.split(separator: " ").first.map(String.init) ?? String(rest)
                if !channel.isEmpty { return channel }
            }
        }
    }
    for line in lines where line.direction == .s2c {
        for rawLine in line.latin1.components(separatedBy: "\r\n") {
            // ":nick!user@host JOIN :#channel" or "...JOIN #channel" -- find
            // the "JOIN" token and take whatever follows (stripping a
            // leading ':' trailing-param marker).
            guard let range = rawLine.range(of: " JOIN ") else { continue }
            var rest = rawLine[range.upperBound...]
            if rest.hasPrefix(":") { rest = rest.dropFirst() }
            let channel = rest.split(separator: " ").first.map(String.init) ?? String(rest)
            if channel.hasPrefix("#") { return channel }
        }
    }
    return nil
}

/// True if `bytes` (a raw s2c chunk) contains a numeric "001" reply anywhere
/// -- the login welcome, used to find the chunk boundary `session.join` must
/// be sequenced after (mirrors ProtocolParseTests.splitAtLoginChunk's own
/// "the 001 chunk" search, done here on raw bytes instead of a pre-decoded
/// String since this driver has no test-target CaptureEvent to reuse).
private func containsLoginWelcome(_ bytes: Data) -> Bool {
    guard let text = String(data: bytes, encoding: .isoLatin1) else { return false }
    return text.contains(" 001 ")
}

/// Phase 2: bridge a fully-collected event stream (from `replayCaptureToEvents`,
/// AFTER the session is done) into a `cc_strip` and PNG-export it. Installs
/// `metricsCanvas` as the layout-time metrics canvas: real CoreText metrics
/// by default (Plan 4a Task 4), like every other Strip-driving path in this
/// package (`--strip`/`--script`'s demo paths), or the deterministic fake
/// RecordingCanvas table under `--fake-metrics`.
func renderReplayedEvents(_ events: [ProtocolEvent], toPath outPath: String, metricsCanvas: Canvas = CTMetricsCanvas()) throws {
    let art = comicartDir()
    let anna = "\(art)/anna.avb"
    let armando = "\(art)/armando.avb"

    let metricsBox = CanvasBox(metricsCanvas)
    cc_set_metrics_canvas(metricsBox.handle)

    try withExtendedLifetime(metricsBox) {
        if ProcessInfo.processInfo.environment["CC_REPLAY_DEBUG"] != nil {
            for e in events { FileHandle.standardError.write(Data("DEBUG EVENT: \(e)\n".utf8)) }
        }
        let resolver = ProtocolStripBridge.AvatarResolver(
            comicartDir: nil, defaultOrder: [anna, armando])
        let bridge = try ProtocolStripBridge(resolver: resolver)
        try bridge.apply(events)

        let (w, h) = bridge.size
        guard w > 0, h > 0 else {
            // No participants/lines rendered (e.g. a connect/join-only
            // capture with no chat messages, like smoke-2.jsonl per the
            // brief's own Step 5) -- report this distinctly rather than
            // crash on a zero-size canvas.
            FileHandle.standardError.write(Data(
                "cc-dumpart: replay produced no renderable strip content (0 participants said anything) -- writing nothing to \(outPath)\n".utf8))
            throw ReplayError.emptyStrip
        }

        let canvas = CGCanvas(widthTwips: w, heightTwips: h, scale: 2.0)
        try bridge.compose(onto: canvas)

        guard let png = canvas.pngData() else {
            throw NSError(domain: "ReplayStrip", code: 1, userInfo: [
                NSLocalizedDescriptionKey: "pngData() returned nil"])
        }
        try png.write(to: URL(fileURLWithPath: outPath))
        FileHandle.standardError.write(Data(
            "cc-dumpart: replayed \(events.count) events -> \(bridge.participantOrder.count) participants, \(bridge.panelCount) panels, wrote \(canvas.pixelWidth)x\(canvas.pixelHeight) strip to \(outPath)\n".utf8))
    }
}

func runReplayMode(jsonlPath: String, outPath: String, metricsCanvas: Canvas = CTMetricsCanvas()) throws {
    let lines = try CaptureLine.parse(jsonlFile: URL(fileURLWithPath: jsonlPath))
    let events = try runAsync { try await replayCaptureToEvents(lines) }
    do {
        try renderReplayedEvents(events, toPath: outPath, metricsCanvas: metricsCanvas)
    } catch ReplayError.emptyStrip {
        // Not a CLI failure -- report already printed above; exit 0 with an
        // informational message is more useful than an error exit for a
        // legitimately connect/join-only capture (brief Step 5's
        // smoke-2.jsonl case).
        return
    }
}

/// Runs an `async throws` closure to completion from synchronous `main.swift`
/// code, blocking the calling thread until it finishes. `cc-dumpart` has no
/// existing async entry point (every other mode is synchronous top-to-bottom),
/// so this is the minimal bridge rather than restructuring the whole CLI's
/// `main` into `@main struct ... async`. `T: Sendable` and `@Sendable body`
/// are required so the closure can safely cross into the `Task`'s executor —
/// `ProtocolEvent` (this file's only real use of `T`) is already `Sendable`.
func runAsync<T: Sendable>(_ body: @escaping @Sendable () async throws -> T) throws -> T {
    let semaphore = DispatchSemaphore(value: 0)
    let box = ResultBox<T>()
    Task {
        do {
            box.result = .success(try await body())
        } catch {
            box.result = .failure(error)
        }
        semaphore.signal()
    }
    semaphore.wait()
    return try box.result.get()
}

/// Hands a `Result` back from the `Task` in `runAsync` to the waiting thread.
/// Safe as `@unchecked Sendable`: the semaphore wait/signal pair guarantees
/// the write (inside the `Task`) happens-before the read (after `wait()`
/// returns), with no concurrent access to `result` from both sides at once.
private final class ResultBox<T>: @unchecked Sendable {
    var result: Result<T, Error> = .failure(CancellationError())
}
