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
    init() throws {
        let params = NWParameters.tcp
        params.allowLocalEndpointReuse = true
        let l = try NWListener(using: params, on: .any)
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
                // Only one peer expected for this loopback test's scope.
                state.peer = conn
                conn.stateUpdateHandler = { connState in
                    if case .ready = connState {
                        sharedQueue.async {
                            state.peerIsReady = true
                            let waiters = state.peerReadyContinuations
                            state.peerReadyContinuations.removeAll()
                            for cont in waiters { cont.resume() }
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
        await waitForPeer()
        let payload = lines.map { $0 + "\r\n" }.joined()
        guard let data = payload.data(using: .isoLatin1) else {
            throw LoopbackError.sendFailed("non-Latin1 test payload")
        }
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
}
