import Foundation
@testable import ComicChatKit

// Plan 3 Task 8 (spec §8.2): parses the Wine capture rig's JSONL format
// (`.superpowers/rig/capture-proxy.ts`, documented in
// docs/superpowers/plans/2026-07-18-plan3-discovery/wine-capture-rig.md) and
// replays a captured session's `s2c` bytes through a real `ProtocolSession`
// (over a `LoopbackIRCServer`, exactly like `ProtocolSessionTests.swift`),
// collecting the emitted `ProtocolEvent` stream.
//
// TEST SEAM CHOICE: `ProtocolSession` has no direct `cc_session_feed_bytes`
// injection point (by design — Task 7 the C engine only ever receives bytes
// via the `NWConnection` receive loop). Rather than add an intrusive
// bytes-only test seam to production code, this replays over the SAME
// loopback-socket seam `ProtocolSessionTests` already established: a
// `LoopbackIRCServer` accepts the `ProtocolSession`'s real `NWConnection`,
// and `sendRaw` writes the captured `s2c` hex bytes verbatim. This exercises
// the exact same code path a live connection would (framing, the serial
// `sessionQueue`, cc_session_feed_bytes) with no shortcuts, and the capture's
// authoritative `hex` bytes are never re-encoded or re-framed.

/// One line of a rig capture: `{"t":<ms>,"dir":"c2s"|"s2c"|"meta","hex":"...","latin1":"..."}`.
struct CaptureEvent {
    enum Direction: String, Decodable {
        case c2s, s2c, meta
    }

    let direction: Direction
    let bytes: Data       // decoded from `hex`; empty for `meta` lines
    let latin1: String    // readable view (or the `meta` line's "note")

    private struct Raw: Decodable {
        let t: Int?
        let dir: String
        let hex: String?
        let latin1: String?
        let note: String?
    }

    /// Parses the rig's JSONL format, one `CaptureEvent` per non-blank line,
    /// in file order. Throws on malformed JSON or an unrecognized `dir`.
    static func parse(jsonl data: Data) throws -> [CaptureEvent] {
        guard let text = String(data: data, encoding: .utf8) else {
            throw CaptureReplayError.notUTF8
        }
        var events: [CaptureEvent] = []
        for line in text.split(separator: "\n", omittingEmptySubsequences: true) {
            let lineData = Data(line.utf8)
            let raw = try JSONDecoder().decode(Raw.self, from: lineData)
            guard let direction = Direction(rawValue: raw.dir) else {
                throw CaptureReplayError.unknownDirection(raw.dir)
            }
            let bytes: Data
            if let hex = raw.hex {
                guard let decoded = Data(hexEncoded: hex) else {
                    throw CaptureReplayError.badHex(hex)
                }
                bytes = decoded
            } else {
                bytes = Data()
            }
            events.append(CaptureEvent(direction: direction, bytes: bytes,
                                        latin1: raw.latin1 ?? raw.note ?? ""))
        }
        return events
    }

    /// Convenience: load + parse a capture file from disk.
    static func parse(jsonlFile url: URL) throws -> [CaptureEvent] {
        try parse(jsonl: Data(contentsOf: url))
    }
}

enum CaptureReplayError: Error, CustomStringConvertible {
    case notUTF8
    case unknownDirection(String)
    case badHex(String)
    case streamEndedBeforeMatch(seenSoFar: [ProtocolEvent])

    var description: String {
        switch self {
        case .notUTF8: return "CaptureReplay: capture file is not valid UTF-8"
        case .unknownDirection(let d): return "CaptureReplay: unknown \"dir\" value: \(d)"
        case .badHex(let h): return "CaptureReplay: malformed hex string: \(h)"
        case .streamEndedBeforeMatch(let seenSoFar):
            return "CaptureReplay: event stream ended before the expected event arrived; seen so far: \(seenSoFar)"
        }
    }
}

private extension Data {
    /// Decodes a lowercase (or uppercase) hex string with no separators, as
    /// produced by the rig's `Buffer.toString('hex')`-equivalent logger. `nil`
    /// on odd length or a non-hex-digit character.
    init?(hexEncoded hex: String) {
        let chars = Array(hex.utf8)
        guard chars.count % 2 == 0 else { return nil }
        var out = [UInt8]()
        out.reserveCapacity(chars.count / 2)
        var i = 0
        while i < chars.count {
            guard let hi = Data.hexNibble(chars[i]), let lo = Data.hexNibble(chars[i + 1]) else {
                return nil
            }
            out.append((hi << 4) | lo)
            i += 2
        }
        self = Data(out)
    }

    static func hexNibble(_ c: UInt8) -> UInt8? {
        switch c {
        case 0x30...0x39: return c - 0x30           // '0'-'9'
        case 0x61...0x66: return c - 0x61 + 10      // 'a'-'f'
        case 0x41...0x46: return c - 0x41 + 10      // 'A'-'F'
        default: return nil
        }
    }
}

/// A capture replay in progress: wraps the one `AsyncStream` iterator that
/// must be threaded sequentially through an entire replay (constructing more
/// than one iterator over the same `ProtocolSession.events` stream would
/// split its events across two consumers, silently dropping some). Used
/// directly by tests that need to inject a side effect (e.g.
/// `session.join(...)`) at a specific point mid-replay; see
/// `replay(_:into:server:until:)` below for the no-interleaving convenience.
///
/// SEQUENCING CAVEAT (load-bearing, the reason this type exists rather than
/// a single `send-everything-then-drain` function with an interleave
/// callback keyed on "chunk N was just sent"): `LoopbackIRCServer.sendRaw`
/// awaiting only means the SERVER side handed the bytes to the OS — it says
/// nothing about whether the CLIENT (`ProtocolSession`'s `NWConnection`) has
/// received and `cc_session_feed_bytes`-processed them yet (a real
/// loopback-socket gap, not a same-process/same-queue guarantee). An
/// earlier version of this replay helper fired its interleave hook right
/// after `sendRaw` returned for the chunk containing `001`, racing the
/// engine's own parsing of that (multi-line) chunk — the outbound `JOIN`
/// bytes landed on the wire WHILE the framer still had a partial line
/// buffered, corrupting it. The only genuinely ordered, synchronized signal
/// available is the DECODED EVENT STREAM itself (ordered by
/// `ProtocolSession`'s serial queue) — so callers must sequence any
/// mid-replay side effect off `collectUntil` observing a specific event
/// (e.g. `.loggedIn`), never off a `send` call completing.
struct CaptureReplayCursor {
    // Plan 4b Task 7: `session.events` now yields `ScopedEvent`; this cursor
    // exposes the bare `ProtocolEvent` API its callers already use by
    // unwrapping `.event` at the one `iterator.next()` boundary below.
    private var iterator: AsyncStream<ScopedEvent>.AsyncIterator
    let server: LoopbackIRCServer
    /// Every event observed so far across the whole replay (all phases).
    private(set) var allEvents: [ProtocolEvent] = []

    init(session: ProtocolSession, server: LoopbackIRCServer) {
        self.iterator = session.events.makeAsyncIterator()
        self.server = server
    }

    /// Sends every `s2c` chunk in `chunks`, in order (skipping any
    /// non-`s2c` entries, so callers can pass a capture slice without
    /// pre-filtering). Does NOT wait for or drain any resulting events —
    /// pair with `collectUntil` to observe what these chunks produced.
    mutating func send(_ chunks: some Sequence<CaptureEvent>) async throws {
        for chunk in chunks where chunk.direction == .s2c {
            try await server.sendRaw(chunk.bytes)
        }
    }

    /// Drains events (appending to `allEvents`) until one matches
    /// `predicate`, returning every event seen in THIS call (inclusive) —
    /// same shape/posture as `ProtocolSessionTests.collectUntil`: relies on
    /// the calling test's own `.timeLimit` trait to fail (rather than hang)
    /// if the expected event never arrives, since `AsyncStream`'s
    /// non-`Sendable` iterator rules out a bespoke timeout race here.
    mutating func collectUntil(_ predicate: (ProtocolEvent) -> Bool) async throws -> [ProtocolEvent] {
        var collectedThisCall: [ProtocolEvent] = []
        while let scoped = await iterator.next() {
            let ev = scoped.event   // Plan 4b Task 7: unwrap the scoped event
            allEvents.append(ev)
            collectedThisCall.append(ev)
            if predicate(ev) { return collectedThisCall }
        }
        throw CaptureReplayError.streamEndedBeforeMatch(seenSoFar: allEvents)
    }
}

/// Replays a parsed capture's `s2c` bytes into a fresh `ProtocolSession`
/// (connected to `server`) in file order, skipping `meta`/`c2s` lines, then
/// drains `session.events` until `predicate` matches, returning everything
/// seen (inclusive) — same shape as `ProtocolSessionTests.collectUntil`.
///
/// `session` must already be connected to `server` (via `session.connect()`)
/// before calling this — construction/connection is left to the caller so
/// tests can interleave their own assertions exactly like
/// `ProtocolSessionTests` does.
///
/// This whole-capture convenience has no way to inject a side effect (like
/// `session.join(...)`) at a specific point mid-replay — tests that need
/// that (e.g. `ProtocolParseTests`, which must call
/// `session.join("#comicrig")` right after observing `.loggedIn` and BEFORE
/// the JOIN-echo chunk is even sent) should use `CaptureReplayCursor`
/// directly instead: `send(_:)` a capture slice, `collectUntil` the marker
/// event, act, `send` the rest, `collectUntil` the final predicate. See
/// `CaptureReplayCursor`'s doc comment for why the side effect MUST be
/// sequenced off the decoded event stream, not off a `send` call completing.
func replay(
    _ capture: [CaptureEvent],
    into session: ProtocolSession,
    server: LoopbackIRCServer,
    until predicate: (ProtocolEvent) -> Bool
) async throws -> [ProtocolEvent] {
    var cursor = CaptureReplayCursor(session: session, server: server)
    try await cursor.send(capture)
    return try await cursor.collectUntil(predicate)
}
