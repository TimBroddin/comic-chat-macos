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

        /// Final-review minor: if the server ERRORS a LIST request instead of
        /// ever completing the usual 321/322×N/323 sequence,
        /// `roomListRequestInFlight` must still clear -- otherwise every
        /// SUBSEQUENT `requestRoomList()` call for the rest of the session
        /// becomes a permanent silent no-op (the in-flight guard, set the
        /// moment the wire LIST goes out, is never reset by anything other
        /// than `.roomListEnd` pre-fix). Drives a genuine `CC_EV_ERROR`
        /// (`ERR_NOSUCHNICK`/401 -- `ircsock.cpp`'s numeric-reply handler,
        /// verified as the simplest reliable trigger for the event this fix
        /// actually watches) in place of `.roomListEnd`, then proves a SECOND
        /// `requestRoomList()` call still reaches the wire.
        @Test(.timeLimit(.minutes(1)))
        func listErrorClearsInFlightGuardSoASubsequentRequestStillReachesTheWire() async throws {
            let server = try LoopbackIRCServer()
            let art = repoRoot5Up().appendingPathComponent("v2.5-beta-1-modern/comicart").path
            let model = ChatSessionModel(config: .init(host: "127.0.0.1", port: server.port,
                                                       nick: "Mac", room: "#p4", artDir: art))
            let statusBox = RoomOpsStatusBox()
            model.onStatus = { text in statusBox.append(text) }
            try await model.start()
            try await server.replyToProbeWith451ThenWelcomeAndJoin(nick: "Mac", channel: "#p4")

            try await model.requestRoomList()
            try await server.waitForClientLine(containing: "LIST")

            // The server errors instead of ever sending .roomListBegin/.../.roomListEnd.
            try await server.send(":srv 401 Mac ghost :No such nick/channel")
            try await pollUntil { !statusBox.entries.isEmpty }

            // Pre-fix, this second call is a silent no-op forever (the
            // in-flight guard never cleared) -- the wire never sees a second
            // LIST. Bounded poll (rather than `waitForClientLine`, which
            // loops forever relying on the test's own `.timeLimit` trait to
            // eventually fail it with a generic timeout, not this test's own
            // diagnostic) so a wrongly-suppressed second LIST fails fast with
            // a clear message instead of burning the full time limit.
            try await model.requestRoomList()
            var sawSecondList = false
            for _ in 0..<200 {   // ~1s at 5ms/poll
                let text = String(data: server.receivedBytes, encoding: .isoLatin1) ?? ""
                let listCount = text.components(separatedBy: "\r\n").filter { $0 == "LIST" || $0.hasPrefix("LIST ") }.count
                if listCount >= 2 { sawSecondList = true; break }
                try await Task.sleep(nanoseconds: 5_000_000)
            }
            #expect(sawSecondList, "expected a SECOND wire LIST after the first round-trip errored out, but none arrived (roomListRequestInFlight never cleared)")

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

/// Thread-safe accumulator for `onStatus`'s `String` callback (same
/// lock-guarded-box shape as `WhisperRoutingTests`' `StatusBox`, duplicated
/// here rather than shared across files per this test target's existing
/// per-file-private-box convention).
private final class RoomOpsStatusBox: @unchecked Sendable {
    private let lock = NSLock()
    private var storage: [String] = []

    func append(_ text: String) {
        lock.lock(); defer { lock.unlock() }
        storage.append(text)
    }

    var entries: [String] {
        lock.lock(); defer { lock.unlock() }
        return storage
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

extension EngineGlobalStateSelfTests {
    @Suite(.serialized)
    struct GetInfoWhoLoopbackTests {
        /// Fix round 1 (review Important #2): `getInfo`/`.whoResult` had zero
        /// loopback coverage. `getInfo("Bob")` is `ProtocolSession.who(_:)`
        /// (ChatSessionModel.swift:1493-1494) -- there is no `whois` builder
        /// on the outbound surface, so Get Info rides the WHO query. Confirms:
        /// (1) the wire carries "WHO Bob", (2) a real RPL_WHOREPLY (352) line
        /// shaped exactly as the engine parser expects --
        /// ":srv 352 <me> <channel> <user> <host> <server> <nick> <flags>
        /// :<hops> <realname>" -- drives `onUserInfo` with (nick, a formatted
        /// string containing the user/host), and (3) the end-of-WHO (315)
        /// closes the query without a second `onUserInfo` firing.
        ///
        /// Arg-layout citation (ircsock.cpp RPL_WHOREPLY handler,
        /// cchat-engine/engine/ircsock.cpp:1640-1667): `pParse->args[0]` is
        /// the numeric itself (`NGetCmd(pParse->args[0])` at line 553 proves
        /// args are 0-indexed from the command, not the prefix), so for this
        /// line args[2]="#comicrig" (channel), args[3]="bob" (user),
        /// args[4]="bob.host" (host), args[6]="Bob" (nick) -- matching the
        /// handler's `pParse->args[2]`/`args[3]`/`args[4]`/`args[6]` reads and
        /// requiring `nArgs >= 8`, satisfied by the 8 space-separated tokens
        /// before the trailing `:<hops> <realname>`.
        @Test(.timeLimit(.minutes(1)))
        func getInfoRoundTripsThroughWhoReply() async throws {
            let server = try LoopbackIRCServer()
            let art = repoRoot5Up().appendingPathComponent("v2.5-beta-1-modern/comicart").path
            let model = ChatSessionModel(config: .init(host: "127.0.0.1", port: server.port,
                                                       nick: "Mac", room: "#comicrig", artDir: art))
            let box = UserInfoBox()
            model.onUserInfo = { nick, text in box.append((nick, text)) }
            try await model.start()
            try await server.replyToProbeWith451ThenWelcomeAndJoin(nick: "Mac", channel: "#comicrig")

            try await model.getInfo("Bob")
            try await server.waitForClientLine(containing: "WHO Bob")

            try await server.send(
                ":srv 352 Mac #comicrig bob bob.host irc.example Bob H :0 Bob Realname",
                ":srv 315 Mac Bob :End of WHO list")

            try await pollUntil { box.count >= 1 }
            // Give a wrongly-doubled firing (e.g. 315 also emitting) a chance
            // to land before asserting the count.
            try await Task.sleep(nanoseconds: 100_000_000)
            #expect(box.count == 1, "onUserInfo must fire exactly once per WHO round-trip, got \(box.count)")
            let (nick, text) = box.all().first ?? ("", "")
            #expect(nick == "Bob")
            #expect(text.contains("bob"), "formatted onUserInfo text must contain the WHO reply's user; got: \(text)")
            #expect(text.contains("bob.host"), "formatted onUserInfo text must contain the WHO reply's host; got: \(text)")

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

/// Thread-safe accumulator for `onUserInfo`'s delivered `(nick, formatted)`
/// results, in delivery order (same lock-guarded-box shape as `RoomListBox`
/// below / `ChatSessionModelTests`'s `MembersBox`).
private final class UserInfoBox: @unchecked Sendable {
    private let lock = NSLock()
    private var storage: [(String, String)] = []
    func append(_ item: (String, String)) {
        lock.lock(); defer { lock.unlock() }
        storage.append(item)
    }
    func all() -> [(String, String)] {
        lock.lock(); defer { lock.unlock() }
        return storage
    }
    var count: Int {
        lock.lock(); defer { lock.unlock() }
        return storage.count
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
