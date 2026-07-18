import Foundation
import Network

/// A minimal in-process loopback TCP server used to feed captured `s2c` bytes
/// into a real `ProtocolSession` (Plan 3 Task 9's `cc-dumpart --replay`).
///
/// `ProtocolSession` has NO direct bytes-injection API by design (Task 7: the
/// C engine only ever receives bytes via the real `NWConnection` receive
/// loop) — the only way to feed it captured bytes is over an actual socket.
/// This is the production-code counterpart of the test target's
/// `LoopbackIRCServer` (`Tests/ComicChatKitTests/LoopbackIRCServer.swift`,
/// Plan 3 Task 7/8): the same "listen on an ephemeral loopback port, accept
/// one connection, write bytes on demand" shape, trimmed to exactly what a
/// replay driver needs (no received-bytes collection, no scripted-line
/// convenience — `cc-dumpart --replay` only ever plays back a capture's
/// authoritative `s2c` hex bytes verbatim). Kept in the library (not the test
/// target) because `cc-dumpart` is a separate SwiftPM module that cannot
/// import test-target sources.
///
/// Threading: `NWListener`/`NWConnection` callbacks run on `queue` (a private
/// serial queue owned by this instance); `sendRaw` hops onto that queue and
/// returns only after the write completes (or fails).
public final class LoopbackReplayServer: @unchecked Sendable {
    public let port: UInt16

    private let listener: NWListener
    private let queue = DispatchQueue(label: "com.comicchat.LoopbackReplayServer")
    private let peerState: PeerState

    public enum ReplayServerError: Error, CustomStringConvertible {
        case startupTimedOut
        case noPeerConnected
        case sendFailed(String)
        public var description: String {
            switch self {
            case .startupTimedOut: return "LoopbackReplayServer: listener did not become ready"
            case .noPeerConnected: return "LoopbackReplayServer: no peer connection accepted yet"
            case .sendFailed(let m): return "LoopbackReplayServer: send failed: \(m)"
            }
        }
    }

    /// Starts listening immediately; throws if the listener can't be created
    /// or fails to come up within a short timeout.
    public init() throws {
        let params = NWParameters.tcp
        params.allowLocalEndpointReuse = true
        let l = try NWListener(using: params, on: .any)
        self.listener = l

        let portBox = PortBox()
        let readySem = DispatchSemaphore(value: 0)
        let state = PeerState()
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
            throw ReplayServerError.startupTimedOut
        }
        if let startupError = portBox.startupError {
            throw startupError
        }
        self.port = portBox.port
        self.peerState = state
    }

    /// Wait for the accepted connection (the `ProtocolSession` under replay)
    /// to reach `.ready`. Called automatically by `sendRaw`.
    public func waitForPeer() async {
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

    /// Sends `data` verbatim, with no line-framing or re-encoding applied —
    /// exactly the captured `s2c` chunk's bytes, preserving whatever
    /// multi-line/partial-line TCP chunking the capture recorded.
    public func sendRaw(_ data: Data) async throws {
        await waitForPeer()
        try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
            queue.async { [peerState] in
                guard let peer = peerState.peer else {
                    continuation.resume(throwing: ReplayServerError.noPeerConnected)
                    return
                }
                peer.send(content: data, completion: .contentProcessed { error in
                    if let error {
                        continuation.resume(throwing: ReplayServerError.sendFailed("\(error)"))
                    } else {
                        continuation.resume()
                    }
                })
            }
        }
    }

    public func stop() {
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

private final class PortBox: @unchecked Sendable {
    var port: UInt16 = 0
    var startupError: Error?
}

/// All access is serialized through `LoopbackReplayServer.queue`, so
/// `@unchecked Sendable` is safe: exactly one queue touches these fields.
private final class PeerState: @unchecked Sendable {
    var peer: NWConnection?
    var peerIsReady = false
    var peerReadyContinuations: [CheckedContinuation<Void, Never>] = []
}
