import Testing
import Foundation
import cchat_engine
@testable import ComicChatKit

// Plan 4a Task 8: outbound avatar announce (ProtocolSession.announceAvatar ->
// cc_session_announce_avatar). D1 R2/D4 §4b: without an outbound
// "# Appears as", Windows peers render us as a random stand-in.
//
// SERIALIZATION (same reasoning as ProtocolSessionTests/LoginSequencingTests):
// nested inside EngineGlobalStateSelfTests (.serialized) so this suite never
// races the C selftests or the other protocol suites over the shared
// process-global engine state (cc_session's file-static g_session).
extension EngineGlobalStateSelfTests {
    @Suite(.serialized)
    struct AnnounceTests {
        /// Waits for `server.receivedBytes` to contain `substring`, polling
        /// briefly -- same shape as LoginSequencingTests' own helper (the c2s
        /// bytes arrive asynchronously over the real loopback socket).
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

        /// Drives the full login sequence (probe -> 451-fallback -> NICK/USER
        /// -> 001, LoginSequencingTests' `plainIrcFallback` scenario) then
        /// joins `channel`, waiting for `.loggedIn` and `.selfJoined` on the
        /// event stream before returning -- `announceAvatar`/`join` are both
        /// gated on `connectionStatus == .connected` (the CX_DISCONNECTED
        /// guard), and `announceAvatar` additionally needs the room_token
        /// registered, which only happens once `join` has run.
        private func loginAndJoin(
            _ session: ProtocolSession,
            _ server: LoopbackIRCServer,
            _ iterator: inout AsyncStream<ScopedEvent>.AsyncIterator,
            channel: String,
            nick: String,
            otherMembers: String = ""
        ) async throws {
            try await session.connect()
            await server.startCollectingReceivedBytes()
            _ = try await waitForReceivedBytes(server, containing: "MODE ISIRCX\r\n")

            try await server.send(":srv 451 * :not registered")
            _ = try await waitForReceivedBytes(server, containing: "USER ")

            try await server.send(":srv 001 \(nick) :Welcome")
            _ = try await collectUntil(&iterator, matching: { if case .loggedIn = $0 { return true } else { return false } })

            try await session.join(channel)
            let names = otherMembers.isEmpty ? nick : "\(nick) \(otherMembers)"
            try await server.send(
                ":\(nick)!\(nick.lowercased())@h JOIN :\(channel)",
                ":srv 353 \(nick) = \(channel) :\(names)",
                ":srv 366 \(nick) \(channel) :End of NAMES list")
            _ = try await collectUntil(&iterator, matching: {
                if case .selfJoined(let ch) = $0 { return ch == channel } else { return false }
            })
        }

        // Channel-wide announce, no URL: "PRIVMSG <chan> :# Appears as <name>\r\n".
        @Test(.timeLimit(.minutes(1)))
        func channelWideAnnounce() async throws {
            let server = try LoopbackIRCServer()
            let session = ProtocolSession(host: "127.0.0.1", port: server.port, nick: "Anon", encoding: .cp1252)
            var iterator = session.events.makeAsyncIterator()

            try await loginAndJoin(session, server, &iterator, channel: "#p4", nick: "Anon")

            try await session.announceAvatar(channel: "#p4", name: "Anna")

            let afterAnnounce = try await waitForReceivedBytes(server, containing: "# Appears as Anna\r\n")
            #expect(afterAnnounce.contains("PRIVMSG #p4 :# Appears as Anna\r\n"),
                    "expected the exact channel-wide announce wire line; c2s so far: \(afterAnnounce)")

            session.disconnect()
            server.stop()
        }

        // Private reply-announce (toNick set): "PRIVMSG <nick> :# Appears as <name>\r\n".
        @Test(.timeLimit(.minutes(1)))
        func privateReplyAnnounce() async throws {
            let server = try LoopbackIRCServer()
            let session = ProtocolSession(host: "127.0.0.1", port: server.port, nick: "Anon", encoding: .cp1252)
            var iterator = session.events.makeAsyncIterator()

            try await loginAndJoin(session, server, &iterator, channel: "#p4", nick: "Anon", otherMembers: "Win")

            try await session.announceAvatar(channel: "#p4", toNick: "Win", name: "Anna")

            let afterAnnounce = try await waitForReceivedBytes(server, containing: "# Appears as Anna\r\n")
            #expect(afterAnnounce.contains("PRIVMSG Win :# Appears as Anna\r\n"),
                    "expected the exact private reply-announce wire line; c2s so far: \(afterAnnounce)")

            session.disconnect()
            server.stop()
        }

        // With a URL: "PRIVMSG <chan> :# Appears as <name>.<url>\r\n".
        @Test(.timeLimit(.minutes(1)))
        func announceWithURL() async throws {
            let server = try LoopbackIRCServer()
            let session = ProtocolSession(host: "127.0.0.1", port: server.port, nick: "Anon", encoding: .cp1252)
            var iterator = session.events.makeAsyncIterator()

            try await loginAndJoin(session, server, &iterator, channel: "#p4", nick: "Anon")

            try await session.announceAvatar(channel: "#p4", name: "Anna", url: "http://x/anna.avb")

            let afterAnnounce = try await waitForReceivedBytes(server, containing: "# Appears as Anna.http://x/anna.avb\r\n")
            #expect(afterAnnounce.contains("PRIVMSG #p4 :# Appears as Anna.http://x/anna.avb\r\n"),
                    "expected the exact URL-suffixed announce wire line; c2s so far: \(afterAnnounce)")

            session.disconnect()
            server.stop()
        }
    }
}
