import Testing
import Foundation
import CoreGraphics
import cchat_engine
@testable import ComicChatKit

// Plan 4b Task 7 — true multi-room (one connection, N joined rooms, one live
// strip owned by the active room). Two suites:
//   - `MultiRoomScopedStreamTests`: the `ProtocolSession.events` stream now
//     yields `ScopedEvent` (event + channel), so a membership/text event's
//     room is recoverable off the stream (Step 2/3).
//   - `MultiRoomModelTests`: `ChatSessionModel` joins a second room, tracks
//     per-room transcripts + unread, and swaps the ONE live strip on
//     `setActiveRoom` (Step 4/5).
//
// SERIALIZATION: same reasoning as ProtocolSessionTests/ChatSessionModelTests
// (the C engine activates a process-global `g_session`/metrics-canvas per
// call) — nested inside EngineGlobalStateSelfTests (.serialized).
extension EngineGlobalStateSelfTests {
    @Suite(.serialized)
    struct MultiRoomScopedStreamTests {
        /// Step 2: after login + JOIN #a + JOIN #b, a peer JOIN and a PRIVMSG
        /// to EACH channel must arrive on `session.events` carrying the
        /// correct `channel` scope. Before Step 3, `events` yields a bare
        /// `ProtocolEvent` with no `.channel`, so this does not compile.
        @Test(.timeLimit(.minutes(1)))
        func eventsCarryChannelScope() async throws {
            let server = try LoopbackIRCServer()
            let session = ProtocolSession(host: "127.0.0.1", port: server.port, nick: "Mac", encoding: .cp1252)
            try await session.connect()

            var iterator = session.events.makeAsyncIterator()

            // login (probe -> 451 -> NICK/USER -> 001)
            try await session.startCollectingReceivedBytesForTest(server)
            try await server.waitForClientLine(containing: "MODE ISIRCX\r\n")
            try await server.send(":srv 451 * :not registered")
            try await server.waitForClientLine(containing: "USER ")
            try await server.send(":srv 001 Mac :Welcome")
            _ = try await collectUntil(&iterator) { if case .loggedIn = $0.event { return true } else { return false } }

            // join #a, confirm
            try await session.join("#a")
            try await server.waitForClientLine(containing: "JOIN #a")
            try await server.send(
                ":Mac!mac@h JOIN :#a",
                ":srv 353 Mac = #a :Mac",
                ":srv 366 Mac #a :End of NAMES list")
            _ = try await collectUntil(&iterator) {
                if case .selfJoined(let c) = $0.event { return c == "#a" } else { return false }
            }

            // join #b, confirm
            try await session.join("#b")
            try await server.waitForClientLine(containing: "JOIN #b")
            try await server.send(
                ":Mac!mac@h JOIN :#b",
                ":srv 353 Mac = #b :Mac",
                ":srv 366 Mac #b :End of NAMES list")
            _ = try await collectUntil(&iterator) {
                if case .selfJoined(let c) = $0.event { return c == "#b" } else { return false }
            }

            // Bob joins #b; a PRIVMSG lands in each channel.
            try await server.send(
                ":Bob!bob@h JOIN :#b",
                ":Alice!al@h PRIVMSG #a :hi from a",
                ":Bob!bob@h PRIVMSG #b :hi from b")

            var bobJoinScope: String? = nil
            var textAScope: String? = nil
            var textBScope: String? = nil
            let collected = try await collectUntil(&iterator) {
                if case .text(let nick, _, _, let text, _, _) = $0.event, nick == "Bob", text == "hi from b" {
                    return true
                }
                return false
            }
            for scoped in collected {
                switch scoped.event {
                case .userJoined(let nick, _) where nick == "Bob":
                    bobJoinScope = scoped.channel
                case .text(_, _, _, let text, _, _) where text == "hi from a":
                    textAScope = scoped.channel
                case .text(_, _, _, let text, _, _) where text == "hi from b":
                    textBScope = scoped.channel
                default:
                    break
                }
            }

            #expect(bobJoinScope == "#b", "Bob's JOIN must carry channel #b, got \(String(describing: bobJoinScope))")
            #expect(textAScope == "#a", "PRIVMSG to #a must carry channel #a, got \(String(describing: textAScope))")
            #expect(textBScope == "#b", "PRIVMSG to #b must carry channel #b, got \(String(describing: textBScope))")

            session.disconnect()
            server.stop()
        }

        /// Drains `ScopedEvent`s until one matches `predicate`, returning every
        /// event seen this call (inclusive). Mirrors ProtocolSessionTests'
        /// own `collectUntil` shape but over the scoped stream.
        private func collectUntil(
            _ iterator: inout AsyncStream<ScopedEvent>.AsyncIterator,
            matching predicate: (ScopedEvent) -> Bool
        ) async throws -> [ScopedEvent] {
            var collected: [ScopedEvent] = []
            while let scoped = await iterator.next() {
                collected.append(scoped)
                if predicate(scoped) { return collected }
            }
            return collected
        }
    }
}

extension EngineGlobalStateSelfTests {
    @Suite(.serialized)
    struct MultiRoomModelTests {
        /// Step 4: one `ChatSessionModel`, two rooms, one live strip.
        ///   - login + join `#a` (initial), then `joinRoom("#b")`
        ///   - 3 messages to `#a`, 2 to `#b`
        ///   - active room `#a` strip shows the title + 3 lines; `#b` unread==2
        ///   - `setActiveRoom("#b")` rebuilds the strip (title + 2), `#b`
        ///     unread==0, a fresh strip image fires
        ///   - a message to now-background `#a` bumps ITS unread without
        ///     touching the live strip's panelCount
        ///   - `leaveRoom("#b")` -> active falls back to `#a` with its full
        ///     3-message strip
        @Test(.timeLimit(.minutes(1)))
        func twoRoomsOneStripSwapsAndCountsUnread() async throws {
            let server = try LoopbackIRCServer()
            let art = repoRoot5Up().appendingPathComponent("v2.5-beta-1-modern/comicart").path
            let model = ChatSessionModel(config: .init(host: "127.0.0.1", port: server.port,
                                                       nick: "Mac", room: "#a", artDir: art))
            let images = MRImagesBox()
            let imagesArrived = AsyncStream<Void>.makeStream()
            model.onStripImage = { _, _ in imagesArrived.continuation.yield() }
            _ = images   // (image identity tracked via panelCount + count below)
            let rooms = MRRoomsBox()
            model.onRoomsChanged = { infos in rooms.set(infos) }

            try await model.start()
            try await server.replyToProbeWith451ThenWelcomeAndJoin(nick: "Mac", channel: "#a")

            // Join the second room on the same connection.
            try await model.joinRoom("#b")
            try await server.waitForClientLine(containing: "JOIN #b")
            try await server.send(
                ":Mac!mac@h JOIN :#b",
                ":srv 353 Mac = #b :Mac",
                ":srv 366 Mac #b :End of NAMES list")
            // Wait until the model has registered both rooms.
            try await pollUntil { model.roomInfos.count == 2 }

            // 3 messages to #a (active), 2 to #b (background).
            try await server.send(
                ":Al!a@h PRIVMSG #a :a-one",
                ":Al!a@h PRIVMSG #a :a-two",
                ":Al!a@h PRIVMSG #a :a-three")
            try await server.send(
                ":Bo!b@h PRIVMSG #b :b-one",
                ":Bo!b@h PRIVMSG #b :b-two")

            // Active room #a: title panel + 3 message panels.
            try await pollUntil { model.transcript(for: "#a").filter { isText($0) }.count == 3 }
            try await pollUntil { model.panelCount == 4 }
            #expect(model.currentRoom == "#a")
            #expect(model.panelCount == 4, "active #a strip: title + 3 messages; got \(model.panelCount)")

            // #b (background): unread == 2, strip untouched.
            try await pollUntil { (rooms.info(for: "#b")?.unread ?? 0) == 2 }
            #expect(rooms.info(for: "#b")?.unread == 2, "background #b unread should be 2, got \(String(describing: rooms.info(for: "#b")?.unread))")
            #expect(model.transcript(for: "#b").filter { isText($0) }.count == 2)

            // Swap to #b: strip rebuilds (title + 2), unread cleared, fresh image.
            var iter = imagesArrived.stream.makeAsyncIterator()
            model.setActiveRoom("#b")
            _ = await iter.next()   // a recompose fired from the rebuild
            try await pollUntil { model.currentRoom == "#b" }
            try await pollUntil { model.panelCount == 3 }
            #expect(model.panelCount == 3, "#b strip: title + 2 messages; got \(model.panelCount)")
            try await pollUntil { (rooms.info(for: "#b")?.unread ?? -1) == 0 }
            #expect(rooms.info(for: "#b")?.unread == 0, "activating #b clears its unread")
            #expect(rooms.info(for: "#b")?.isActive == true)

            // A message to now-background #a bumps its unread, live strip (#b)
            // panelCount unchanged.
            let bPanelsBefore = model.panelCount
            try await server.send(":Al!a@h PRIVMSG #a :a-four")
            try await pollUntil { (rooms.info(for: "#a")?.unread ?? 0) == 1 }
            #expect(rooms.info(for: "#a")?.unread == 1, "background #a unread bumps to 1")
            #expect(model.panelCount == bPanelsBefore, "a background-room message must not touch the live strip")
            #expect(model.transcript(for: "#a").filter { isText($0) }.count == 4, "the message still lands in #a's transcript")

            // Leave #b: active falls back to #a with its full 4-message strip.
            try await model.leaveRoom("#b")
            try await pollUntil { model.currentRoom == "#a" }
            try await pollUntil { model.panelCount == 5 }   // title + 4 messages
            #expect(model.currentRoom == "#a")
            #expect(model.panelCount == 5, "after leaving #b, #a's strip rebuilds with title + 4 messages; got \(model.panelCount)")
            #expect(model.roomInfos.count == 1)

            model.shutdown()
            server.stop()
        }

        private func isText(_ ev: ProtocolEvent) -> Bool {
            if case .text = ev { return true }
            return false
        }

        /// Polls `condition` (5ms cadence) until true; the test's own
        /// `.timeLimit` trait fails a never-true condition rather than hanging.
        private func pollUntil(_ condition: @escaping () -> Bool) async throws {
            while !condition() {
                try await Task.sleep(nanoseconds: 5_000_000)
            }
        }
    }
}

/// Thread-safe `onStripImage` counter (same lock-box shape as
/// ChatSessionModelTests' `ImagesBox`).
private final class MRImagesBox: @unchecked Sendable {
    private let lock = NSLock()
    private var storage = 0
    func bump() { lock.lock(); defer { lock.unlock() }; storage += 1 }
    var count: Int { lock.lock(); defer { lock.unlock() }; return storage }
}

/// Thread-safe latest-`[RoomInfo]` box (`onRoomsChanged` fires on the main
/// thread; the test body reads from its own task).
private final class MRRoomsBox: @unchecked Sendable {
    private let lock = NSLock()
    private var storage: [RoomInfo] = []
    func set(_ infos: [RoomInfo]) { lock.lock(); defer { lock.unlock() }; storage = infos }
    func all() -> [RoomInfo] { lock.lock(); defer { lock.unlock() }; return storage }
    func info(for name: String) -> RoomInfo? {
        lock.lock(); defer { lock.unlock() }
        return storage.first { $0.name == name }
    }
}

// Test-only helpers on the loopback rig + session used across the multi-room
// suites — thin conveniences over the existing `receivedBytes`/`send` API.
extension LoopbackIRCServer {
    /// Polls `receivedBytes` until the decoded client stream contains
    /// `substring` (bounded by the caller's own `.timeLimit`). Same shape as
    /// `replyToProbeWith451ThenWelcomeAndJoin`'s inline waits, factored out.
    func waitForClientLine(containing substring: String) async throws {
        while true {
            let text = String(data: receivedBytes, encoding: .isoLatin1) ?? ""
            if text.contains(substring) { return }
            try await Task.sleep(nanoseconds: 5_000_000)
        }
    }
}

extension ProtocolSession {
    /// Arms the loopback rig's receive loop (needed before `waitForClientLine`
    /// can observe anything). A tiny shim so the scoped-stream test reads like
    /// the model tests without duplicating `startCollectingReceivedBytes`.
    func startCollectingReceivedBytesForTest(_ server: LoopbackIRCServer) async throws {
        await server.startCollectingReceivedBytes()
    }
}
