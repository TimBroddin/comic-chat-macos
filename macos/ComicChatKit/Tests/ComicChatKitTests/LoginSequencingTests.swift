import Testing
import Foundation
import cchat_engine
@testable import ComicChatKit

// Plan 4a Task 2: live login sequencing (probe -> 451-fallback / 800-pivot ->
// NICK/USER -> loggedIn). D4 §2's hard prerequisite: no code anywhere sent
// NICK/USER before this task -- the loopback tests all pushed `:srv 001`
// unprompted (ProtocolSessionTests.swift). This suite proves ProtocolSession
// now drives the whole sequence itself: `connect()` sends "MODE ISIRCX"
// immediately, then reacts to whichever of {451, 800-pivot, timeout} the
// server produces to send NICK/USER, ending in `.loggedIn` with no further
// calls from the test (Task 9's dependency).
//
// SERIALIZATION (same reasoning as ProtocolSessionTests/EngineInterleaveTests):
// nested inside EngineGlobalStateSelfTests (.serialized) so this suite never
// races the C selftests or the other protocol suites over the shared
// process-global engine state (cc_session's file-static g_session).
extension EngineGlobalStateSelfTests {
    @Suite(.serialized)
    struct LoginSequencingTests {
        /// Waits for `server.receivedBytes` to contain `substring`, polling
        /// briefly -- the c2s bytes arrive asynchronously (real loopback
        /// socket hop), so a bare synchronous read right after `connect()`
        /// returns is not guaranteed to see them yet. Bounded by the calling
        /// test's own `.timeLimit` trait.
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

        private func collectUntil(
            _ iterator: inout AsyncStream<ProtocolEvent>.AsyncIterator,
            matching predicate: @escaping (ProtocolEvent) -> Bool
        ) async throws -> [ProtocolEvent] {
            var collected: [ProtocolEvent] = []
            while true {
                guard let ev = await iterator.next() else {
                    throw StreamEndedError(collectedSoFar: collected)
                }
                collected.append(ev)
                if predicate(ev) { return collected }
            }
        }

        private struct StreamEndedError: Error, CustomStringConvertible {
            let collectedSoFar: [ProtocolEvent]
            var description: String {
                "event stream ended before a matching event arrived; collected so far: \(collectedSoFar)"
            }
        }

        // Scenario 1: Plain-IRC fallback. connect() -> "MODE ISIRCX" is the
        // FIRST c2s line; server replies 451 (not registered); the engine's
        // ccModeIsIrcXFailure trigger fires (dequeues the probe cell, cancels
        // the timer) and ProtocolSession reacts by sending NICK then USER;
        // server confirms with 001 -> .loggedIn("Mac").
        @Test(.timeLimit(.minutes(1)))
        func plainIrcFallback() async throws {
            let server = try LoopbackIRCServer()
            let session = ProtocolSession(host: "127.0.0.1", port: server.port, nick: "Mac", encoding: .cp1252)
            var iterator = session.events.makeAsyncIterator()

            // `startCollectingReceivedBytes` must be called AFTER `connect()`
            // (it internally waits for the SERVER side's peer connection to
            // exist, which only happens once the client actually dials in --
            // see LoopbackIRCServer.waitForPeer's doc comment). This does NOT
            // race the probe bytes: TCP delivers them regardless of when the
            // receive loop starts (ProtocolCodecTests's own established
            // pattern -- "starting late does not actually exclude earlier
            // bytes").
            try await session.connect()
            await server.startCollectingReceivedBytes()

            let afterProbe = try await waitForReceivedBytes(server, containing: "MODE ISIRCX\r\n")
            #expect(afterProbe.hasPrefix("MODE ISIRCX\r\n"),
                    "expected \"MODE ISIRCX\" to be the FIRST c2s line; got: \(afterProbe)")

            try await server.send(":srv 451 * :not registered")

            let afterLogin = try await waitForReceivedBytes(server, containing: "USER ")
            #expect(afterLogin.contains("NICK Mac\r\n"), "expected a NICK line after the 451 fallback; got: \(afterLogin)")
            #expect(afterLogin.contains("USER "), "expected a USER line after the 451 fallback; got: \(afterLogin)")
            // NICK must precede USER (HrIrcLogin's own sequence).
            let nickRange = afterLogin.range(of: "NICK Mac\r\n")!
            let userRange = afterLogin.range(of: "USER ")!
            #expect(nickRange.lowerBound < userRange.lowerBound)

            try await server.send(":srv 001 Mac :Welcome")
            let seen = try await collectUntil(&iterator, matching: { if case .loggedIn = $0 { return true } else { return false } })
            #expect(seen.contains { if case .loggedIn(let nick) = $0 { return nick == "Mac" } else { return false } })

            session.disconnect()
            server.stop()
        }

        // Scenario 2: IRCX pivot. Server answers the probe with the FIRST 800
        // (state 0) advertising ANON -> ProtocolSession must NOT log in yet;
        // it sends "IRCX" (the engine's own bExecuteQuery(qpIrcX,...) call,
        // unconditional on the parse side). The server then sends the SECOND
        // 800 (state 1) -- per Step 1's finding, the engine emits nothing
        // observable here when anon is allowed EXCEPT a second CC_EV_SERVER_CAPS
        // re-emission (this task's documented deviation from the original
        // silent branch, see ircsock.cpp's RPL_IRCX handler comment) -- which
        // is exactly the trigger ProtocolSession uses to send NICK/USER.
        @Test(.timeLimit(.minutes(1)))
        func ircxPivot() async throws {
            let server = try LoopbackIRCServer()
            let session = ProtocolSession(host: "127.0.0.1", port: server.port, nick: "Mac", encoding: .cp1252)
            var iterator = session.events.makeAsyncIterator()

            try await session.connect()
            await server.startCollectingReceivedBytes()
            _ = try await waitForReceivedBytes(server, containing: "MODE ISIRCX\r\n")

            try await server.send(":srv 800 * 0 0 ANON 512 *")
            let afterFirst800 = try await waitForReceivedBytes(server, containing: "IRCX\r\n")
            // NICK/USER must NOT have been sent yet -- only after the SECOND 800.
            #expect(!afterFirst800.contains("NICK "), "login must wait for the second 800; c2s so far: \(afterFirst800)")

            try await server.send(":srv 800 * 1 0 ANON 512 *")
            let afterSecond800 = try await waitForReceivedBytes(server, containing: "USER ")
            #expect(afterSecond800.contains("NICK Mac\r\n"))
            #expect(afterSecond800.contains("USER "))

            try await server.send(":srv 001 Mac :Welcome")
            let seen = try await collectUntil(&iterator, matching: { if case .loggedIn = $0 { return true } else { return false } })
            #expect(seen.contains { if case .loggedIn(let nick) = $0 { return nick == "Mac" } else { return false } })

            session.disconnect()
            server.stop()
        }

        // Scenario 3: probe timeout. The server never answers "MODE ISIRCX"
        // at all -- ProtocolSession's own probeTimeoutMs (capped well below
        // the engine's requested 50s) fires the timer-driven fallback, which
        // must send NICK/USER anyway (matching real clients that don't hang
        // forever on a non-responding/dead-air server).
        @Test(.timeLimit(.minutes(1)))
        func probeTimeout() async throws {
            let server = try LoopbackIRCServer()
            let session = ProtocolSession(host: "127.0.0.1", port: server.port, nick: "Mac", encoding: .cp1252)
            session.probeTimeoutMs = 200
            var iterator = session.events.makeAsyncIterator()

            try await session.connect()
            await server.startCollectingReceivedBytes()
            _ = try await waitForReceivedBytes(server, containing: "MODE ISIRCX\r\n")

            // Server stays silent -- NICK/USER must still arrive within ~1s,
            // driven by the capped probe timeout.
            let afterTimeout = try await waitForReceivedBytes(server, containing: "USER ")
            #expect(afterTimeout.contains("NICK Mac\r\n"))
            #expect(afterTimeout.contains("USER "))

            try await server.send(":srv 001 Mac :Welcome")
            let seen = try await collectUntil(&iterator, matching: { if case .loggedIn = $0 { return true } else { return false } })
            #expect(seen.contains { if case .loggedIn(let nick) = $0 { return nick == "Mac" } else { return false } })

            session.disconnect()
            server.stop()
        }
    }
}
