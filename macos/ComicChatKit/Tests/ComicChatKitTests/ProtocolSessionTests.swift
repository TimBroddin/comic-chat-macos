import Testing
import Foundation
import cchat_engine
@testable import ComicChatKit

// Plan 3 Task 7: ProtocolSession end-to-end over a real loopback TCP socket.
//
// SERIALIZATION (load-bearing, same reasoning as StripTests/StripScriptTests):
// the C engine's cc_session layer activates a file-static `g_session` on
// entry to every cc_session_* call (bridge/cc_session.cpp) -- there is
// exactly one "current" session at the C layer, process-wide, for the
// duration of any call. Two ProtocolSession tests running concurrently would
// both try to activate g_session, racing each other's session pointer.
// Nested inside EngineGlobalStateSelfTests (declared .serialized in
// BodyDrawTests.swift) so every engine-global-state test -- the C selftests,
// the Strip tests, and these -- runs on one single timeline.
extension EngineGlobalStateSelfTests {
    @Suite(.serialized)
    struct ProtocolSessionTests {
        @Test(.timeLimit(.minutes(1)))
        func joinAndReceiveAnnotatedMessage() async throws {
            let server = try LoopbackIRCServer()
            let session = ProtocolSession(host: "127.0.0.1", port: server.port, nick: "Anon", encoding: .cp1252)
            try await session.connect()

            var iterator = session.events.makeAsyncIterator()

            try await server.send(":srv 001 Anon :Welcome")
            var seen: [ProtocolEvent] = []
            seen.append(contentsOf: try await collectUntil(&iterator, matching: { if case .loggedIn = $0 { return true } else { return false } }))

            // `join` registers the room_token<->"#comicrig" mapping BEFORE
            // sending the wire JOIN (see ProtocolSession.join's doc comment):
            // the engine's inbound JOIN handler only ever RESOLVES an
            // already-registered token, it never registers one itself. This
            // mirrors a real client, which always knows what channel it's
            // asking to join before the server confirms it. It must run only
            // AFTER `.loggedIn` is observed, since `join` is gated on
            // `connectionStatus == .connected` (the CX_DISCONNECTED guard,
            // carry-forward #2), which the `.loggedIn` event is what flips.
            try await session.join("#comicrig")

            // Server confirms: self-join echo for Anon (which is what enqueues
            // the pending ctNames query the 353 reply below needs -- see
            // ircsock.cpp's cmdidJoin self-join branch), then the 353 NAMES
            // reply, then Bob joining and saying something with an inline
            // plain-IRC annotation block.
            try await server.send(
                ":Anon!anon@h JOIN :#comicrig",
                ":srv 353 Anon = #comicrig :Anon",
                ":Bob!bob@h JOIN :#comicrig",
                ":Bob!bob@h PRIVMSG #comicrig :(#G295E193M1) hello")

            seen.append(contentsOf: try await collectUntil(&iterator, matching: {
                if case .text(let nick, _, _, _, _, _) = $0 { return nick == "Bob" } else { return false }
            }))

            #expect(seen.contains { if case .loggedIn = $0 { return true } else { return false } })
            #expect(seen.contains { if case .selfJoined(let channel) = $0 { return channel == "#comicrig" } else { return false } })
            #expect(seen.contains { if case .userJoined(let nick, _) = $0 { return nick == "Bob" } else { return false } })
            let sawAnnotatedText = seen.contains {
                if case .text(let nick, _, _, let text, _, let annotations) = $0 {
                    return nick == "Bob" && text == "hello" && annotations != nil
                }
                return false
            }
            #expect(sawAnnotatedText, "expected a .text event for Bob's message carrying decoded annotations; saw: \(seen)")

            let members = session.members
            #expect(members["#comicrig"]?.contains("Bob") == true)

            session.disconnect()
            server.stop()
        }

        // Carry-forward #1 (Task 6 review amendment): the IRCX DATA CCUDI1
        // out-of-band annotation transport arrives as TWO separate engine
        // events -- CC_EV_DATA{nick, annotations} for the "DATA <target>
        // CCUDI1 :#G..." line, then CC_EV_TEXT{nick, has_annotations=false}
        // for the plain PRIVMSG that immediately follows. The engine is
        // stateless across these two lines (Task 6 discovery), so
        // ProtocolSession must re-pair them BY NICK: stash the DATA
        // annotations, and attach them to the very next has_annotations=false
        // text event from the same nick (single-use -- cleared on consume).
        @Test(.timeLimit(.minutes(1)))
        func ircxDataPrivmsgAnnotationRepairing() async throws {
            let server = try LoopbackIRCServer()
            let session = ProtocolSession(host: "127.0.0.1", port: server.port, nick: "Anon", encoding: .cp1252)
            try await session.connect()

            var iterator = session.events.makeAsyncIterator()

            try await server.send(":srv 001 Anon :Welcome")
            _ = try await collectUntil(&iterator, matching: { if case .loggedIn = $0 { return true } else { return false } })

            try await session.join("#comicrig")

            // The DATA line's nick is taken from the MESSAGE PREFIX
            // (`:nick!user@host DATA ...`, cmdidData's `pParse->nick`) --
            // NOT from the DATA command's own target argument -- and
            // re-pairing keys off that same prefix nick, matching the plain
            // PRIVMSG's prefix nick right after. Both empty-prefix and
            // non-channel-target DATA lines are rejected by the parser's
            // `*pParse->nick && *pParse->user` guard, so the line needs a
            // full `:nick!user@host` prefix.
            try await server.send(
                ":Anon!anon@h JOIN :#comicrig",
                ":srv 353 Anon = #comicrig :Anon",
                ":Bob!bob@h JOIN :#comicrig",
                ":Bob!bob@h DATA #comicrig CCUDI1 :#G295E193M1",
                ":Bob!bob@h PRIVMSG #comicrig :plain text, annotations arrive out-of-band")

            let seen = try await collectUntil(&iterator, matching: {
                if case .text(let nick, _, _, _, _, _) = $0 { return nick == "Bob" } else { return false }
            })

            let repaired = seen.contains {
                if case .text(let nick, _, _, let text, _, let annotations) = $0 {
                    return nick == "Bob" && text == "plain text, annotations arrive out-of-band"
                        && annotations != nil && annotations!.gesturePose == 2
                }
                return false
            }
            #expect(repaired, "expected the plain PRIVMSG following a DATA CCUDI1 line to be re-paired with that DATA blob's annotations; saw: \(seen)")

            // The raw CC_EV_DATA event is still surfaced (documented choice,
            // see ProtocolSession.handleEvent) but must NOT itself carry the
            // re-attached pose (it's the source of the pending slot, not a
            // consumer of it).
            let sawRawData = seen.contains { if case .data(let nick, _) = $0 { return nick == "Bob" } else { return false } }
            #expect(sawRawData)

            session.disconnect()
            server.stop()
        }

        // Carry-forward #2: the CX_DISCONNECTED guard. Before login is
        // confirmed (CC_EV_LOGGED_IN / `.connected` status), ProtocolSession's
        // outbound methods must refuse to reach the engine at all, mirroring
        // the original bExecuteQuery's dropped-then-Task-7-re-added guard
        // (state-and-codec.md's connection-status row; Task 4 report's
        // explicit flag to Task 7).
        @Test(.timeLimit(.minutes(1)))
        func outboundGuardedWhileDisconnected() async throws {
            let server = try LoopbackIRCServer()
            let session = ProtocolSession(host: "127.0.0.1", port: server.port, nick: "Anon", encoding: .cp1252)
            try await session.connect()
            // No 001 sent yet -- session is only .socketConnected, not
            // .connected. Every outbound call must throw notConnected rather
            // than touching cc_session_* (which would ASSERT/crash on a
            // session with no confirmed login state to work from).
            await #expect(throws: ProtocolSession.ProtocolSessionError.self) {
                try await session.join("#comicrig")
            }
            await #expect(throws: ProtocolSession.ProtocolSessionError.self) {
                try await session.who(nil)
            }

            try await server.send(":srv 001 Anon :Welcome")
            // give the event loop a moment to process the 001 line and flip
            // connectionStatus to .connected before asserting success.
            var loggedIn = false
            for await scoped in session.events {
                if case .loggedIn = scoped.event { loggedIn = true; break }
            }
            #expect(loggedIn)

            // Now that login is confirmed, join is allowed to reach the
            // engine (it may still fail for other reasons in this bare
            // loopback harness, but it must not throw notConnected).
            do {
                try await session.join("#comicrig")
            } catch let error as ProtocolSession.ProtocolSessionError {
                if case .notConnected = error {
                    Issue.record("join() still guarded as notConnected after CC_EV_LOGGED_IN")
                }
            }

            session.disconnect()
            server.stop()
        }

        /// Drains `iterator` into an array, stopping (inclusive) at the first
        /// event matching `predicate`. No timeout of its own — every `@Test`
        /// in this suite carries `.timeLimit(.minutes(1))` (Swift Testing's
        /// built-in test-timeout trait), so a missing/never-arriving event
        /// fails the whole test via that mechanism rather than hanging the
        /// suite forever; a bespoke per-call race against `Task.sleep` was
        /// tried first and abandoned because `AsyncStream.AsyncIterator` is
        /// not `Sendable` and can't cross into a task-group child task,
        /// which a manual timeout race requires.
        private func collectUntil(
            _ iterator: inout AsyncStream<ScopedEvent>.AsyncIterator,
            matching predicate: @escaping (ProtocolEvent) -> Bool
        ) async throws -> [ProtocolEvent] {
            var collected: [ProtocolEvent] = []
            while true {
                guard let scoped = await iterator.next() else {
                    throw StreamEndedError(collectedSoFar: collected)
                }
                let ev = scoped.event   // Plan 4b Task 7: unwrap the scoped event
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
    }
}
