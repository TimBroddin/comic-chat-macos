import Foundation
import Network

/// Offline demo/E2E server: replays a Wine capture rig's recorded `s2c` bytes
/// to a single connecting client (Plan 4a Task 9). Lets `cc-dumpart`/the app
/// demo a full login->join->comic session with no real IRC server, and lets
/// `FixtureReplayServerTests` exercise `ChatSessionModel`'s live loop against
/// a fixture instead of a hand-scripted `LoopbackIRCServer` sequence.
///
/// FIXTURE FORMAT: one JSON object per line (the Wine capture rig's format,
/// `.superpowers/rig/capture-proxy.ts`; also parsed test-side by
/// `CaptureReplay.swift`): `{"t":<ms>,"dir":"c2s"|"s2c"|"meta","hex":"...",
/// "latin1":"..."}`. Only `dir == "s2c"` lines are replayed; the `hex` field
/// is authoritative (decoded to raw bytes, sent VERBATIM — no re-framing, no
/// re-encoding, matching `LoopbackIRCServer.sendRaw`'s posture in the test
/// suite). `c2s`/`meta` lines are read (so a whole real capture file can be
/// pointed at directly) but never sent.
///
/// ACTUAL BEHAVIOR (final review, Plan 4a: corrects the pacing design below,
/// which reads as intentional but is dead code): `onClientReady`'s "send
/// every s2c chunk before the first trigger fires" loop runs to completion —
/// i.e. sends EVERY s2c chunk in the fixture — before `receiveLoop()` starts
/// listening for any client bytes at all. That means all `s2c` chunks are
/// always sent, unpaced, immediately on connect, and the trigger machinery in
/// `onClientBytes` (the "MODE ISIRCX" / "NICK " / "JOIN " substring watches)
/// never observes a client byte before `nextChunkIndex` has already reached
/// `s2cChunks.count` — every trigger's `sendNextChunkLocked()` call is
/// therefore always a no-op (`nextChunkIndex < s2cChunks.count` is already
/// false). This is proven safe for this server's actual job: every fixture
/// used here (see AppState's replay hook) is a canned login->join->comic
/// sequence for an EVENT-DRIVEN client (`ProtocolSession`'s parser reacts to
/// whatever bytes arrive whenever they arrive — it has no wall-clock
/// expectations about WHEN the 451/001/JOIN-echo show up), so dumping the
/// whole fixture on connect and letting the client's own state machine work
/// through it byte-by-byte produces the same observable session as a
/// carefully paced delivery would. Confirmed by both `FixtureReplayServerTests`
/// and the live `--replay-fixture` demo (Task 12's exit milestone).
///
/// The paragraph below documents the ORIGINAL pacing design intent — kept for
/// history/rationale, but per the above, `onClientBytes`'s trigger checks
/// never fire in practice; do not rely on them:
///
/// a full capture's `c2s` lines are what the ORIGINAL client sent, which is
/// almost never byte-identical to what `ProtocolSession`'s auto-login
/// produces (e.g. `smoke-2.jsonl`'s "client 2" slice ends in a PING/PONG
/// keepalive exchange and a QUIT/ERROR teardown that a live `ChatSessionModel`
/// session never sends) — so pacing CANNOT simply be "replay c2s line N, then
/// send s2c chunk N+1" against a raw capture. The intent was instead to
/// replay s2c CHUNKS paced by watching the CONNECTING client's own outbound
/// bytes for three simple substring triggers, in order, each firing at most
/// once:
///   1. on connect (no trigger needed) -> send every s2c chunk BEFORE the
///      first trigger fires (a fixture's pre-probe-answer chunk, if any).
///   2. first "MODE ISIRCX" seen from the client -> send the NEXT s2c chunk
///      (`smoke-2-replay.jsonl`'s 451 "not registered" reply -- our client
///      always probes IRCX first, Plan 4a Task 2).
///   3. first "NICK " seen from the client -> send the NEXT s2c chunk (the
///      001 welcome + MOTD block).
///   4. first "JOIN " seen from the client -> send the NEXT s2c chunk (the
///      JOIN echo + NAMES/353/366 block, and everything else remaining).
/// This was intended to NOT be a general capture-replay engine (it doesn't
/// try to pace WHO/PING/etc. — a fixture prepared for this server, like
/// `smoke-2-replay.jsonl`, should carry only the s2c chunks this trigger set
/// can drive: pre-probe/451/001/join-echo, in that order). A fixture with
/// MORE than 4 s2c chunks sends its extra chunks immediately after the
/// join-echo chunk (best-effort, undifferentiated) rather than silently
/// dropping them.
///
/// THREADING: `@unchecked Sendable` because every mutable field
/// (`connection`, `receivedText`, `nextChunkIndex`, the three `saw*` flags)
/// is touched ONLY from closures that run serialized on `queue` — the
/// `NWListener`/`NWConnection` callbacks are all started with `queue:` (or
/// hop onto it via `queue.async` first thing), matching `LoopbackIRCServer`'s
/// identical pattern in the test target.
public final class FixtureReplayServer: @unchecked Sendable {
    public enum FixtureReplayServerError: Error, CustomStringConvertible {
        case notUTF8
        case unknownDirection(String)
        case badHex(String)
        case listenerFailed(String)
        public var description: String {
            switch self {
            case .notUTF8: return "FixtureReplayServer: fixture file is not valid UTF-8"
            case .unknownDirection(let d): return "FixtureReplayServer: unknown \"dir\" value: \(d)"
            case .badHex(let h): return "FixtureReplayServer: malformed hex string: \(h)"
            case .listenerFailed(let m): return "FixtureReplayServer: listener failed: \(m)"
            }
        }
    }

    /// One fixture line's decoded payload (only `s2c` lines carry bytes).
    private struct Chunk {
        let isS2C: Bool
        let bytes: Data
    }

    private let s2cChunks: [Data]
    private let listener: NWListener
    private let queue = DispatchQueue(label: "com.comicchat.FixtureReplayServer")
    private var connection: NWConnection?
    /// Buffers every byte the client has sent so far, so trigger substrings
    /// can be checked against the FULL running text even if a trigger's
    /// bytes straddle two separate `receive` calls.
    private var receivedText = ""
    private var nextChunkIndex = 0
    private var sawModeIsIrcX = false
    private var sawNick = false
    private var sawJoin = false

    public private(set) var port: UInt16 = 0

    /// Parses `fixtureURL` (jsonl) and prepares the listener (not yet
    /// started — call `start()`).
    public init(fixtureURL: URL) throws {
        let data = try Data(contentsOf: fixtureURL)
        guard let text = String(data: data, encoding: .utf8) else {
            throw FixtureReplayServerError.notUTF8
        }
        var chunks: [Data] = []
        for line in text.split(separator: "\n", omittingEmptySubsequences: true) {
            let lineData = Data(line.utf8)
            let raw = try JSONDecoder().decode(RawLine.self, from: lineData)
            guard raw.dir == "s2c" || raw.dir == "c2s" || raw.dir == "meta" else {
                throw FixtureReplayServerError.unknownDirection(raw.dir)
            }
            guard raw.dir == "s2c" else { continue }
            guard let hex = raw.hex, let bytes = Data(hexEncoded: hex) else {
                throw FixtureReplayServerError.badHex(raw.hex ?? "")
            }
            chunks.append(bytes)
        }
        self.s2cChunks = chunks

        let params = NWParameters.tcp
        params.allowLocalEndpointReuse = true
        guard let l = try? NWListener(using: params, on: .any) else {
            throw FixtureReplayServerError.listenerFailed("NWListener init failed")
        }
        self.listener = l
    }

    private struct RawLine: Decodable {
        let dir: String
        let hex: String?
    }

    /// Starts listening on `127.0.0.1` (an ephemeral port, reported via
    /// `port` once bound) and arms the accept handler. Synchronous: blocks
    /// briefly until the listener is ready (or throws).
    public func start() throws {
        let readySem = DispatchSemaphore(value: 0)
        let portBox = PortBox()

        listener.stateUpdateHandler = { [weak self] state in
            switch state {
            case .ready:
                portBox.port = self?.listener.port?.rawValue ?? 0
                readySem.signal()
            case .failed(let error):
                portBox.error = error
                readySem.signal()
            default:
                break
            }
        }
        listener.newConnectionHandler = { [weak self] conn in
            guard let self else { return }
            self.queue.async {
                self.acceptConnection(conn)
            }
        }
        listener.start(queue: queue)

        let result = readySem.wait(timeout: .now() + 5)
        if result == .timedOut {
            throw FixtureReplayServerError.listenerFailed("timed out waiting for listener readiness")
        }
        if let error = portBox.error {
            throw FixtureReplayServerError.listenerFailed("\(error)")
        }
        self.port = portBox.port
    }

    public func stop() {
        queue.sync {
            connection?.cancel()
            connection = nil
        }
        listener.cancel()
    }

    deinit {
        listener.cancel()
    }

    /// Runs on `queue`. Only the FIRST connection is serviced (a fixture
    /// replay models one client session); later connections are cancelled
    /// immediately.
    private func acceptConnection(_ conn: NWConnection) {
        guard connection == nil else {
            conn.cancel()
            return
        }
        connection = conn
        conn.stateUpdateHandler = { [weak self] state in
            guard let self else { return }
            switch state {
            case .ready:
                self.queue.async { self.onClientReady() }
            default:
                break
            }
        }
        conn.start(queue: queue)
    }

    /// Runs on `queue`. In practice (see this type's doc comment) this loop
    /// always runs to completion — `hasAnyTrigger()` can only ever become
    /// true from `onClientBytes`, which cannot run until `receiveLoop()`
    /// below has been called at least once — so this sends the ENTIRE
    /// fixture's s2c bytes before the receive loop ever arms. Currently
    /// inert as a pacing mechanism — see doc comment.
    private func onClientReady() {
        while nextChunkIndex < s2cChunks.count, !hasAnyTrigger() {
            sendNextChunkLocked()
        }
        receiveLoop()
    }

    private func hasAnyTrigger() -> Bool {
        sawModeIsIrcX || sawNick || sawJoin
    }

    private func receiveLoop() {
        guard let conn = connection else { return }
        conn.receive(minimumIncompleteLength: 1, maximumLength: 65536) { [weak self] data, _, isComplete, _ in
            guard let self else { return }
            self.queue.async {
                if let data, !data.isEmpty {
                    self.onClientBytes(data)
                }
                if !isComplete {
                    self.receiveLoop()
                }
            }
        }
    }

    /// Runs on `queue`. Appends to the running received-text buffer and fires
    /// at most one NEW trigger per call (in the fixed order MODE ISIRCX ->
    /// NICK -> JOIN, matching the login sequence those substrings appear in),
    /// sending the next queued s2c chunk each time a trigger fires for the
    /// first time. Currently inert — see this type's doc comment: by the time
    /// any client bytes reach this method, `onClientReady` has already sent
    /// every s2c chunk, so every `sendNextChunkLocked()` call below is a
    /// no-op (`nextChunkIndex` is already `== s2cChunks.count`).
    private func onClientBytes(_ data: Data) {
        let text = String(data: data, encoding: .isoLatin1) ?? ""
        receivedText += text

        if !sawModeIsIrcX, receivedText.contains("MODE ISIRCX") {
            sawModeIsIrcX = true
            sendNextChunkLocked()
        }
        if !sawNick, receivedText.contains("NICK ") {
            sawNick = true
            sendNextChunkLocked()
        }
        if !sawJoin, receivedText.contains("JOIN ") {
            sawJoin = true
            // Every trigger has now fired -- flush any remaining chunks too
            // (see the type's doc comment: a fixture with more than 4 s2c
            // chunks sends the extras here, best-effort).
            while nextChunkIndex < s2cChunks.count {
                sendNextChunkLocked()
            }
        }
    }

    /// Runs on `queue`. Sends `s2cChunks[nextChunkIndex]` verbatim (no
    /// re-framing/re-encoding, matching `LoopbackIRCServer.sendRaw`) and
    /// advances the index. No-op if every chunk has already been sent.
    private func sendNextChunkLocked() {
        guard nextChunkIndex < s2cChunks.count, let conn = connection else { return }
        let bytes = s2cChunks[nextChunkIndex]
        nextChunkIndex += 1
        conn.send(content: bytes, completion: .contentProcessed { _ in })
    }
}

private final class PortBox: @unchecked Sendable {
    var port: UInt16 = 0
    var error: Error?
}

private extension Data {
    /// Decodes a lowercase (or uppercase) hex string with no separators —
    /// same shape as the test-side `CaptureReplay.swift`'s private helper of
    /// the same name (duplicated here rather than shared: `Sources/` cannot
    /// depend on the test target).
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
        case 0x30...0x39: return c - 0x30
        case 0x61...0x66: return c - 0x61 + 10
        case 0x41...0x46: return c - 0x41 + 10
        default: return nil
        }
    }
}
