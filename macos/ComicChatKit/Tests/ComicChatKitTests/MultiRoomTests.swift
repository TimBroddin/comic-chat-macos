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

        /// Final-review Important #1 (RED before the fix): a peer's PRIVATE
        /// `.appearsAs` reply-announce -- the 1998 client's standard response
        /// to our own channel-wide avatar announce -- arrives as a bare
        /// `PRIVMSG <ourNick> :# Appears as <name>` (token 0, no channel;
        /// wire form verified against `AnnounceTests.privateReplyAnnounce`'s
        /// own assertion of the OUTBOUND shape of the exact same line, which
        /// this test drives INBOUND). Pre-fix, `handleLocked` routed this to
        /// `sessionTranscript` (never replayed by `rebuildStripLocked`) and
        /// gated `bridge.apply` on `isActiveRoom` (which a token-0 event can
        /// never satisfy, since `isActiveRoom` requires `scopedRoom != nil`)
        /// -- so the peer's avatar update was rendered nowhere, ever, and
        /// lost completely on any later reflow.
        ///
        /// Setup: join #a (peer "Win" present) and #b on one connection, #a
        /// active. Drive the private reply-announce, then:
        ///   1. assert it landed in EVERY joined room's transcript (#a AND
        ///      #b) -- the fan-out this fix adds, proving replay-survival for
        ///      a room that isn't even the one active when the announce
        ///      arrived;
        ///   2. assert the live strip actually re-composed (a fresh
        ///      `onStripImage` fires) -- proving the "regardless of active
        ///      room" apply, not just a transcript write with no visible
        ///      effect;
        ///   3. switch away to #b and back to #a (a real `setActiveRoom`
        ///      rebuild, replaying #a's transcript from scratch) -- assert
        ///      the strip recomposes again without error and #a's transcript
        ///      still carries the `.appearsAs` entry, proving the avatar
        ///      assignment SURVIVES a rebuild rather than being a one-shot
        ///      live-only effect.
        @Test(.timeLimit(.minutes(1)))
        func privateAppearsAsReplyAnnounceFansOutToAllRoomsAndSurvivesRebuild() async throws {
            let server = try LoopbackIRCServer()
            let art = repoRoot5Up().appendingPathComponent("v2.5-beta-1-modern/comicart").path
            let model = ChatSessionModel(config: .init(host: "127.0.0.1", port: server.port,
                                                       nick: "Mac", room: "#a", artDir: art))
            let images = MRImagesBox()
            model.onStripImage = { _, _ in images.bump() }

            try await model.start()
            try await server.replyToProbeWith451ThenWelcomeAndJoin(nick: "Mac", channel: "#a", otherMembers: "Win")

            try await model.joinRoom("#b")
            try await server.waitForClientLine(containing: "JOIN #b")
            try await server.send(
                ":Mac!mac@h JOIN :#b",
                ":srv 353 Mac = #b :Mac",
                ":srv 366 Mac #b :End of NAMES list")
            try await pollUntil { model.roomInfos.count == 2 }
            #expect(model.currentRoom == "#a", "sanity: #a is active going into the announce")

            let imagesBeforeAnnounce = images.count

            // The private reply-announce -- token 0, no channel prefix. Wire
            // form matches AnnounceTests.privateReplyAnnounce's outbound
            // assertion exactly (`PRIVMSG <nick> :# Appears as <name>\r\n`),
            // driven here as an INBOUND line from the peer.
            try await server.send(":Win!u@h PRIVMSG Mac :# Appears as Armando")

            // Polled (rather than an `AsyncStream` await) since a prior
            // recompose's yield could otherwise be mistaken for this one --
            // polling the actual counter sidesteps that ordering hazard.
            try await pollUntil { images.count > imagesBeforeAnnounce }
            #expect(images.count > imagesBeforeAnnounce,
                    "the private reply-announce must trigger a live strip recompose regardless of which room is active")

            func appearsAsLanded(in transcript: [ProtocolEvent]) -> Bool {
                transcript.contains {
                    if case .appearsAs(let nick, let avatarName, _) = $0 {
                        return nick == "Win" && avatarName == "Armando"
                    }
                    return false
                }
            }
            try await pollUntil { appearsAsLanded(in: model.transcript(for: "#a")) }
            #expect(appearsAsLanded(in: model.transcript(for: "#a")),
                    "the private announce must land in the ACTIVE room's transcript")
            #expect(appearsAsLanded(in: model.transcript(for: "#b")),
                    "the private announce must ALSO land in a BACKGROUND room's transcript (the fan-out fix) so its own later rebuild re-avatars Win too")

            // Rebuild #a from scratch (switch away, then back) -- proves the
            // avatar assignment is REPLAY-DURABLE, not merely a one-shot live
            // mutation that a reflow would silently lose (the exact pre-fix
            // failure mode: the event was never IN any transcript, so a
            // rebuild had nothing to replay it from).
            model.setActiveRoom("#b")
            try await pollUntil { model.currentRoom == "#b" }
            model.setActiveRoom("#a")
            try await pollUntil { model.currentRoom == "#a" }
            try await pollUntil { model.panelCount > 0 }
            #expect(appearsAsLanded(in: model.transcript(for: "#a")),
                    "the announce must still be in #a's transcript after a rebuild replayed it from scratch")

            model.shutdown()
            server.stop()
        }

        /// Final-review Important #2 (RED before the fix): `changeCharacter`
        /// only appends its synthetic `.appearsAs` to rooms that already
        /// exist AT SWITCH TIME (`self.roomOrder` in that method, at the
        /// moment it runs) -- a room joined AFTER the switch has no switch
        /// entry anywhere in its transcript. That room's very first rebuild
        /// (`setUpStripLocked`'s `isReflow` seeding) seeds the self
        /// participant from `initialCharacterName` (the character the
        /// SESSION started with) and then replays a transcript with nothing
        /// in it to move the avatar forward -- self renders as the ORIGINAL
        /// character in the new room, even though every other room (and the
        /// wheel/preview) has shown the switched character ever since.
        ///
        /// Setup: join #a (initial, character "anna"), switch to "armando",
        /// THEN join #b, send a line there, activate it. Assert:
        ///   1. #b's transcript's FIRST event is the seeded `.appearsAs` for
        ///      our own nick naming "Armando" -- landing at position 0, i.e.
        ///      before the line sent afterward, so ANY replay of #b renders
        ///      self as the switched character from the very first panel;
        ///   2. panelCount coherence after activating #b (title + the one
        ///      line sent there) -- proves the seed didn't corrupt the
        ///      strip's own panel bookkeeping.
        @Test(.timeLimit(.minutes(1)))
        func roomJoinedAfterCharacterSwitchSeedsSwitchInNewRoomTranscript() async throws {
            let server = try LoopbackIRCServer()
            let art = repoRoot5Up().appendingPathComponent("v2.5-beta-1-modern/comicart").path
            let model = ChatSessionModel(config: .init(host: "127.0.0.1", port: server.port,
                                                       nick: "Mac", room: "#a",
                                                       characterName: "anna", artDir: art))
            try await model.start()
            try await server.replyToProbeWith451ThenWelcomeAndJoin(nick: "Mac", channel: "#a")

            // Switch character BEFORE #b is ever joined.
            model.changeCharacter("armando")
            try await withCheckedContinuation { (cont: CheckedContinuation<Void, Never>) in
                model.settleEngineQueue { cont.resume() }
            }

            // Join #b -- AFTER the switch. Pre-fix, #b's box is created with
            // an empty transcript (`ensureRoomBoxLocked`); this fix seeds it
            // with the switch synthetic at creation time.
            try await model.joinRoom("#b")
            try await server.waitForClientLine(containing: "JOIN #b")
            try await server.send(
                ":Mac!mac@h JOIN :#b",
                ":srv 353 Mac = #b :Mac",
                ":srv 366 Mac #b :End of NAMES list")
            try await pollUntil { model.roomInfos.count == 2 }

            func seededAppearsAs(in transcript: [ProtocolEvent]) -> Bool {
                guard let first = transcript.first, case .appearsAs(let nick, let avatarName, _) = first else {
                    return false
                }
                return nick == "Mac" && avatarName == "Armando"
            }
            try await pollUntil { !model.transcript(for: "#b").isEmpty }
            let bTranscript = model.transcript(for: "#b")
            #expect(seededAppearsAs(in: bTranscript),
                    "room #b (joined AFTER the character switch) must have the switch seeded as its FIRST transcript event, got: \(bTranscript)")

            // Send a line into #b, activate it, and check panel coherence:
            // title + the seeded appearsAs (no panel of its own -- appearsAs
            // doesn't add a panel) + the one line = title + 1 message.
            try await server.send(":Al!a@h PRIVMSG #b :hello b")
            try await pollUntil { model.transcript(for: "#b").filter { isText($0) }.count == 1 }

            model.setActiveRoom("#b")
            try await pollUntil { model.currentRoom == "#b" }
            try await pollUntil { model.panelCount == 2 }   // title + 1 message
            #expect(model.panelCount == 2, "#b strip: title + 1 message (the seed itself adds no panel); got \(model.panelCount)")

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
