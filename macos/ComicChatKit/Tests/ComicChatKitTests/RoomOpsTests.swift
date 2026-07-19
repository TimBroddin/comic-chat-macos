import Testing
import Foundation
import cchat_engine
@testable import ComicChatKit

// Plan 4b Task 8 — room list + room ops surface + member-list upgrades.
//
// SERIALIZATION: same reasoning as ProtocolSessionTests/ChatSessionModelTests/
// MultiRoomTests (the C engine activates a process-global `g_session`/
// metrics-canvas per call) — nested inside EngineGlobalStateSelfTests
// (.serialized).
extension EngineGlobalStateSelfTests {
    @Suite(.serialized)
    struct RoomOpsWrapperTests {
        /// Step 1(1): each of the 6 `ProtocolSession` wrappers reaches the
        /// right engine builder — asserted on the wire VERB (KICK/INVITE/MODE
        /// +b or room modes/AWAY, plus create-room's own CREATE verb), not
        /// exact grammar (the engine builders own the grammar).
        @Test(.timeLimit(.minutes(1)))
        func wrappersEmitExpectedWireVerbs() async throws {
            let server = try LoopbackIRCServer()
            let session = ProtocolSession(host: "127.0.0.1", port: server.port, nick: "Anon", encoding: .cp1252)
            try await session.connect()
            await server.startCollectingReceivedBytes()

            try await server.send(":srv 001 Anon :Welcome")
            var iterator = session.events.makeAsyncIterator()
            _ = try await collectUntil(&iterator) { if case .loggedIn = $0.event { return true } else { return false } }

            try await session.join("#comicrig")
            try await server.waitForClientLine(containing: "JOIN #comicrig")
            try await server.send(
                ":Anon!anon@h JOIN :#comicrig",
                ":srv 353 Anon = #comicrig :Anon Bob",
                ":srv 366 Anon #comicrig :End of NAMES list")
            _ = try await collectUntil(&iterator) {
                if case .selfJoined(let c) = $0.event { return c == "#comicrig" } else { return false }
            }

            try await session.kick("#comicrig", nick: "Bob", reason: "bye")
            try await server.waitForClientLine(containing: "KICK")

            try await session.invite("#comicrig", nick: "Carol")
            try await server.waitForClientLine(containing: "INVITE")

            try await session.ban("#comicrig", pattern: "*!*@evil.example", banning: true)
            try await server.waitForClientLine(containing: "MODE")

            try await session.setRoomMode("#comicrig", mode: 0, maxUsers: 10)
            try await server.waitForClientLine(containing: "MODE")

            try await session.setAway(true, message: "brb")
            try await server.waitForClientLine(containing: "AWAY")

            // create-room's wire shape (ChatCreateAux, ircproto.cpp:803-828) is
            // "CREATE <channel>[ <modes>][ <maxUsers>][ <password>]\r\n" — a
            // distinct CREATE verb, NOT a JOIN (the server implicitly joins
            // the creator to a room it creates; the client never sends a
            // separate JOIN for it).
            try await session.createRoom("#newroom")
            try await server.waitForClientLine(containing: "CREATE")
            let text = String(data: server.receivedBytes, encoding: .isoLatin1) ?? ""
            #expect(text.contains("#newroom"), "createRoom must reach the wire naming the new channel; got: \(text)")

            session.disconnect()
            server.stop()
        }

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
    struct RoomListAccumulationTests {
        /// Step 1(2): server replies 321/322×3/323 -> `onRoomList` fires ONCE
        /// with 3 items (accumulate begin->items->end, fire once on end).
        @Test(.timeLimit(.minutes(1)))
        func roomListFiresOnceWithAllItems() async throws {
            let server = try LoopbackIRCServer()
            let art = repoRoot5Up().appendingPathComponent("v2.5-beta-1-modern/comicart").path
            let model = ChatSessionModel(config: .init(host: "127.0.0.1", port: server.port,
                                                       nick: "Mac", room: "#p4", artDir: art))
            let box = RoomListBox()
            model.onRoomList = { items in box.append(items) }
            try await model.start()
            try await server.replyToProbeWith451ThenWelcomeAndJoin(nick: "Mac", channel: "#p4")

            try await model.requestRoomList()
            try await server.waitForClientLine(containing: "LIST")
            try await server.send(
                ":srv 321 Mac Channel :Users Name",
                ":srv 322 Mac #alpha 3 :Alpha topic",
                ":srv 322 Mac #beta 5 :Beta topic",
                ":srv 322 Mac #gamma 1 :Gamma topic",
                ":srv 323 Mac :End of LIST")

            try await pollUntil { box.count >= 1 }
            // Give any (incorrect) extra firing a chance to land before asserting.
            try await Task.sleep(nanoseconds: 100_000_000)
            #expect(box.count == 1, "onRoomList must fire exactly once per LIST round-trip, got \(box.count)")
            let items: [RoomListItem] = box.all().first ?? []
            let itemCount: Int = items.count
            #expect(itemCount == 3, "expected 3 room-list items, got \(itemCount)")
            let names: Set<String> = Set(items.map { $0.name })
            #expect(names == ["#alpha", "#beta", "#gamma"])
            let alpha = items.first { $0.name == "#alpha" }
            #expect(alpha?.users == 3)
            #expect(alpha?.topic == "Alpha topic")
            let beta = items.first { $0.name == "#beta" }
            #expect(beta?.users == 5)
            #expect(beta?.topic == "Beta topic")
            let gamma = items.first { $0.name == "#gamma" }
            #expect(gamma?.users == 1)
            #expect(gamma?.topic == "Gamma topic")

            model.shutdown()
            server.stop()
        }

        /// Self-review fix regression: a second `requestRoomList()` call
        /// while a LIST is already in flight (no `.roomListEnd` seen yet)
        /// must NOT send a second wire LIST — two overlapping round-trips
        /// sharing the same accumulator could otherwise interleave and
        /// splice/corrupt the result (`.roomListBegin` resetting mid-way
        /// through the first round-trip). Only ONE "LIST" line should reach
        /// the wire even though `requestRoomList()` is called twice.
        @Test(.timeLimit(.minutes(1)))
        func secondRequestWhileInFlightDoesNotDoubleSendWireList() async throws {
            let server = try LoopbackIRCServer()
            let art = repoRoot5Up().appendingPathComponent("v2.5-beta-1-modern/comicart").path
            let model = ChatSessionModel(config: .init(host: "127.0.0.1", port: server.port,
                                                       nick: "Mac", room: "#p4", artDir: art))
            let box = RoomListBox()
            model.onRoomList = { items in box.append(items) }
            try await model.start()
            try await server.replyToProbeWith451ThenWelcomeAndJoin(nick: "Mac", channel: "#p4")

            try await model.requestRoomList()
            try await server.waitForClientLine(containing: "LIST")
            // A second call BEFORE the first round-trip's .roomListEnd — must
            // be a silent no-op (no second wire LIST).
            try await model.requestRoomList()
            // Give a wrongly-sent second LIST a window to reach the wire
            // before asserting it didn't.
            try await Task.sleep(nanoseconds: 100_000_000)
            let sentText = String(data: server.receivedBytes, encoding: .isoLatin1) ?? ""
            let listCount = sentText.components(separatedBy: "\r\n").filter { $0 == "LIST" || $0.hasPrefix("LIST ") }.count
            #expect(listCount == 1, "expected exactly one wire LIST despite two requestRoomList() calls, got \(listCount) in: \(sentText)")

            try await server.send(
                ":srv 321 Mac Channel :Users Name",
                ":srv 322 Mac #alpha 3 :Alpha topic",
                ":srv 323 Mac :End of LIST")
            try await pollUntil { box.count >= 1 }
            try await Task.sleep(nanoseconds: 100_000_000)
            #expect(box.count == 1, "onRoomList must still fire exactly once, got \(box.count)")

            // A THIRD call, now that the first round-trip has completed
            // (.roomListEnd cleared the in-flight guard), must reach the wire.
            try await model.requestRoomList()
            try await server.waitForClientLine(containing: "LIST")

            model.shutdown()
            server.stop()
        }

        private func pollUntil(_ condition: @escaping () -> Bool) async throws {
            while !condition() {
                try await Task.sleep(nanoseconds: 5_000_000)
            }
        }
    }
}

extension EngineGlobalStateSelfTests {
    @Suite(.serialized)
    struct GoToSemanticsTests {
        /// Step 1(3): `goToRoom("#other")` (coordinator ruling, post-multi-room)
        /// = `joinRoom` + `setActiveRoom` -- opens a NEW tab, NO part-first. The
        /// server must see `JOIN #other` and must NOT see a `PART` of the
        /// original room. `rooms` gains a tab; the active room becomes `#other`
        /// with a fresh title-only strip.
        @Test(.timeLimit(.minutes(1)))
        func goToRoomJoinsWithoutPartingOriginal() async throws {
            let server = try LoopbackIRCServer()
            let art = repoRoot5Up().appendingPathComponent("v2.5-beta-1-modern/comicart").path
            let model = ChatSessionModel(config: .init(host: "127.0.0.1", port: server.port,
                                                       nick: "Mac", room: "#p4", artDir: art))
            try await model.start()
            try await server.replyToProbeWith451ThenWelcomeAndJoin(nick: "Mac", channel: "#p4")

            try await model.goToRoom("#other")
            try await server.waitForClientLine(containing: "JOIN #other")
            try await server.send(
                ":Mac!mac@h JOIN :#other",
                ":srv 353 Mac = #other :Mac",
                ":srv 366 Mac #other :End of NAMES list")

            try await pollUntil { model.currentRoom == "#other" }
            #expect(model.currentRoom == "#other")
            #expect(model.roomInfos.count == 2, "goToRoom must ADD a tab, not replace the original room")
            #expect(model.roomInfos.contains { $0.name == "#p4" }, "the original room must remain joined (no part-first)")
            #expect(model.panelCount == 1, "a fresh room's strip is title-only until a message arrives")

            let text = String(data: server.receivedBytes, encoding: .isoLatin1) ?? ""
            #expect(!text.contains("PART"), "goToRoom must NOT part the original room; wire bytes: \(text)")

            model.shutdown()
            server.stop()
        }

        private func pollUntil(_ condition: @escaping () -> Bool) async throws {
            while !condition() {
                try await Task.sleep(nanoseconds: 5_000_000)
            }
        }
    }
}

/// Thread-safe accumulator for `onRoomList`'s delivered snapshots, in
/// delivery order (same lock-guarded-box shape as `ChatSessionModelTests`'
/// `MembersBox`).
private final class RoomListBox: @unchecked Sendable {
    private let lock = NSLock()
    private var storage: [[RoomListItem]] = []
    func append(_ items: [RoomListItem]) {
        lock.lock(); defer { lock.unlock() }
        storage.append(items)
    }
    func all() -> [[RoomListItem]] {
        lock.lock(); defer { lock.unlock() }
        return storage
    }
    var count: Int {
        lock.lock(); defer { lock.unlock() }
        return storage.count
    }
}
