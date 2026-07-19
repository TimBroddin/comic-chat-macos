import Foundation
import Network

/// A tiny in-process TCP server for `ProtocolSessionTests`: listens on an
/// ephemeral loopback port, accepts exactly one connection (the
/// `ProtocolSession` under test), and lets the test script raw IRC lines at
/// it on demand via `send`. This is intentionally minimal — no IRC semantics,
/// just "accept a connection, write bytes when told to" — the actual
/// protocol behavior under test lives entirely in the engine + `ProtocolSession`.
///
/// Threading: `NWListener`/`NWConnection` callbacks run on `queue` (a private
/// serial queue owned by this helper); `send` hops onto that queue and
/// returns only after the write completes (or fails), so the test can call
/// `server.send(...)` and know the bytes are in flight before it proceeds.
final class LoopbackIRCServer: @unchecked Sendable {
    let port: UInt16

    private let listener: NWListener
    private let queue = DispatchQueue(label: "com.comicchat.LoopbackIRCServer")
    /// All mutable peer-connection bookkeeping lives in this box rather than
    /// directly on `self`, because `newConnectionHandler` (installed in
    /// `init`, below) is created BEFORE `self` finishes initializing (`port`
    /// isn't set yet at that point) — capturing `self` there is illegal
    /// ("used before being initialized"). The handler instead captures this
    /// box directly; `init` stashes it in `peerState` once `self` is fully
    /// formed, so every other method can reach the same mutable state through
    /// `self.peerState`.
    private let peerState: PeerState

    /// Starts listening immediately; throws if the listener can't be created
    /// or fails to come up within a short timeout.
    ///
    /// - Parameter port: bind to a SPECIFIC port instead of an OS-assigned
    ///   ephemeral one (`nil`, the default). The auto-reconnect test
    ///   (`ReconnectTests`) needs this: a drop-then-reconnect targets
    ///   `config.host`/`config.port`, so after the first server closes the
    ///   socket the test must resurrect a SECOND server on the SAME port the
    ///   `ProtocolSession` will dial back into. `allowLocalEndpointReuse`
    ///   (already set below) lets the fresh listener rebind that port even
    ///   though the just-closed one may linger in TIME_WAIT briefly.
    convenience init() throws {
        try self.init(port: nil)
    }

    init(port: UInt16?) throws {
        let params = NWParameters.tcp
        params.allowLocalEndpointReuse = true
        let l: NWListener
        if let port, let nwPort = NWEndpoint.Port(rawValue: port) {
            l = try NWListener(using: params, on: nwPort)
        } else {
            l = try NWListener(using: params, on: .any)
        }
        self.listener = l

        let portBox = PortBox()
        let readySem = DispatchSemaphore(value: 0)
        let state = PeerState()
        // Local alias: `self.queue` can't be referenced from these closures
        // (installed before `self` finishes initializing), so capture the
        // queue value directly instead.
        let sharedQueue = queue

        l.stateUpdateHandler = { lState in
            switch lState {
            case .ready:
                portBox.port = l.port?.rawValue ?? 0
                readySem.signal()
            case .failed(let error):
                portBox.startupError = error
                readySem.signal()
            default:
                break
            }
        }
        l.newConnectionHandler = { conn in
            sharedQueue.async {
                // One peer at a time. A SECOND accepted connection (the
                // auto-reconnect test's fresh `ProtocolSession` re-dialing this
                // same still-listening server after `dropPeer()`) REPLACES the
                // old one: reset the ready flag + re-arm the receive loop so the
                // new peer's bytes are collected too. `receivedBytes` is NOT
                // cleared here (the reconnect test wants a running record across
                // both peers — call `resetReceivedBytes()` explicitly to zero
                // it before observing the reconnect's own re-login bytes).
                state.peer?.cancel()
                state.peer = conn
                state.peerIsReady = false
                conn.stateUpdateHandler = { connState in
                    if case .ready = connState {
                        sharedQueue.async {
                            state.peerIsReady = true
                            let waiters = state.peerReadyContinuations
                            state.peerReadyContinuations.removeAll()
                            for cont in waiters { cont.resume() }
                            // Re-arm the receive loop for the NEW peer if the
                            // server was already collecting (the old peer's loop
                            // ended when its connection was cancelled).
                            if state.isCollectingReceives {
                                state.rearmReceive?()
                            }
                        }
                    }
                }
                conn.start(queue: sharedQueue)
            }
        }
        l.start(queue: sharedQueue)

        let waitResult = readySem.wait(timeout: .now() + 5)
        if waitResult == .timedOut {
            throw LoopbackError.startupTimedOut
        }
        if let startupError = portBox.startupError {
            throw startupError
        }
        self.port = portBox.port
        self.peerState = state
        // Now that `self` is fully formed, give `PeerState` a way to re-arm the
        // receive loop for a replacement peer (the auto-reconnect case). Always
        // called on `queue` from `newConnectionHandler`.
        state.rearmReceive = { [weak self] in self?.receiveLoop() }
    }

    enum LoopbackError: Error, CustomStringConvertible {
        case startupTimedOut
        case noPeerConnected
        case sendFailed(String)
        var description: String {
            switch self {
            case .startupTimedOut: return "LoopbackIRCServer: listener did not become ready"
            case .noPeerConnected: return "LoopbackIRCServer: no peer connection accepted yet"
            case .sendFailed(let m): return "LoopbackIRCServer: send failed: \(m)"
            }
        }
    }

    /// Wait for the accepted connection (the `ProtocolSession` under test) to
    /// reach `.ready`. Call this after `session.connect()` and before the
    /// first `send`.
    func waitForPeer() async {
        await withCheckedContinuation { (inner: CheckedContinuation<Void, Never>) in
            queue.async { [peerState] in
                if peerState.peerIsReady {
                    inner.resume()
                } else {
                    peerState.peerReadyContinuations.append(inner)
                }
            }
        }
    }

    /// Sends each line, CRLF-terminated (IRC wire framing), concatenated into
    /// one write. Waits for the peer connection if it hasn't been accepted
    /// yet (bounded wait — governed by the caller's own test timeout).
    func send(_ lines: String...) async throws {
        try await send(lines)
    }

    func send(_ lines: [String]) async throws {
        let payload = lines.map { $0 + "\r\n" }.joined()
        guard let data = payload.data(using: .isoLatin1) else {
            throw LoopbackError.sendFailed("non-Latin1 test payload")
        }
        try await sendRaw(data)
    }

    /// Sends `data` verbatim, with no line-framing or re-encoding applied —
    /// the byte-exact counterpart to `send(_:)` (which re-joins/CRLF-terminates
    /// and re-encodes as ISO-Latin1, lossy for arbitrary captured bytes). Used
    /// by `CaptureReplay` (Plan 3 Task 8) to feed a captured `s2c` chunk's
    /// authoritative `hex` bytes exactly as the real server emitted them,
    /// preserving whatever multi-line/partial-line TCP chunking the capture
    /// recorded.
    func sendRaw(_ data: Data) async throws {
        await waitForPeer()
        try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
            queue.async { [peerState] in
                guard let peer = peerState.peer else {
                    continuation.resume(throwing: LoopbackError.noPeerConnected)
                    return
                }
                peer.send(content: data, completion: .contentProcessed { error in
                    if let error {
                        continuation.resume(throwing: LoopbackError.sendFailed("\(error)"))
                    } else {
                        continuation.resume()
                    }
                })
            }
        }
    }

    /// The bytes the peer (the `ProtocolSession`/engine under test) has sent
    /// to this server so far, in receive order. Populated by a background
    /// receive loop started on first access — `CaptureReplay` uses this to
    /// collect `c2s` bytes the engine emits in response to a replayed capture,
    /// for future byte-compare tests once real annotated `c2s` captures exist.
    func startCollectingReceivedBytes() async {
        // Must wait for the peer connection to actually exist before arming
        // the receive loop: `newConnectionHandler`'s `state.peer = conn`
        // (init, above) fires asynchronously once the TCP handshake
        // completes on the SERVER side, which is not guaranteed to have
        // already happened just because the CLIENT's `session.connect()`
        // has returned (that only awaits the CLIENT side's `NWConnection`
        // reaching `.ready`) -- an earlier version of this method started
        // the loop synchronously and silently no-op'd (guard-return on a
        // nil peer, never retried) when called immediately after `connect()`.
        await waitForPeer()
        queue.async { [self] in
            if peerState.isCollectingReceives { return }
            peerState.isCollectingReceives = true
            receiveLoop()
        }
    }

    private func receiveLoop() {
        guard let peer = peerState.peer else { return }
        peer.receive(minimumIncompleteLength: 1, maximumLength: 65536) { [weak self] data, _, isComplete, _ in
            guard let self else { return }
            self.queue.async {
                if let data, !data.isEmpty {
                    self.peerState.receivedBytes.append(data)
                }
                if !isComplete {
                    self.receiveLoop()
                }
            }
        }
    }

    /// Snapshot of everything received so far (see `startCollectingReceivedBytes`).
    var receivedBytes: Data {
        queue.sync { peerState.receivedBytes }
    }

    /// Cancels the accepted peer connection WITHOUT tearing down the listener
    /// (`ReconnectTests`' drop trigger): the `ProtocolSession` under test sees
    /// its receive loop hit EOF/error, surfaces `.disconnectedHint`, and the
    /// model schedules a reconnect. This server STAYS listening, so the model's
    /// reconnect re-dials the same port and `newConnectionHandler` accepts the
    /// fresh `ProtocolSession` as a replacement peer (see that handler). Resets
    /// `peerIsReady` so `waitForPeer` (and every `send`) blocks until the NEW
    /// peer actually connects rather than firing against the just-dropped one.
    func dropPeer() {
        queue.sync { [peerState] in
            peerState.peer?.cancel()
            peerState.peer = nil
            peerState.peerIsReady = false
        }
    }

    /// Zeroes the accumulated `receivedBytes` (`ReconnectTests`): after a drop,
    /// the reconnect test wants to observe ONLY the reconnect's own re-login
    /// bytes, not the pre-drop session's, so it clears the record between the
    /// two peers.
    func resetReceivedBytes() {
        queue.sync { [peerState] in
            peerState.receivedBytes = Data()
        }
    }

    func stop() {
        queue.sync { [peerState] in
            peerState.peer?.cancel()
            peerState.peer = nil
        }
        listener.cancel()
    }

    deinit {
        listener.cancel()
    }
}

/// Small mutable box so the state-update handler (a plain closure, not
/// isolated to any actor) can hand the ready port (or startup failure) back
/// to `init` via a semaphore-guarded handoff — safe because the semaphore
/// wait/signal pair guarantees `init`'s read happens-after the handler's
/// write, with no concurrent access to either field.
private final class PortBox: @unchecked Sendable {
    var port: UInt16 = 0
    var startupError: Error?
}

/// The accepted peer connection plus its readiness bookkeeping. All access is
/// serialized through `LoopbackIRCServer.queue` (every read/write above is
/// wrapped in `queue.async`/`queue.sync`), so `@unchecked Sendable` is safe:
/// there is exactly one queue touching these fields, ever.
private final class PeerState: @unchecked Sendable {
    var peer: NWConnection?
    var peerIsReady = false
    var peerReadyContinuations: [CheckedContinuation<Void, Never>] = []
    /// See `LoopbackIRCServer.startCollectingReceivedBytes`/`receivedBytes`.
    var isCollectingReceives = false
    var receivedBytes = Data()
    /// Re-arms the receive loop for a freshly-accepted REPLACEMENT peer
    /// (`newConnectionHandler`, the auto-reconnect case). Set once `self` is
    /// fully initialized (the handler is installed before that, so it can only
    /// reach this box, not `self.receiveLoop()` directly). Called on `queue`.
    var rearmReceive: (() -> Void)?
}
