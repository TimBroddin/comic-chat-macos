import Testing
import Foundation
import cchat_engine
@testable import ComicChatKit

// Live-fix 1 (Plan 4b, crypthome.com live-reproduced): IRC channel names are
// case-insensitive (RFC 1459 §2.3.1). `JOIN #crypt` followed by the server's
// JOIN echo/PRIVMSGs naming `#Crypt` (server casing) previously created a
// SECOND room box, keyed on the byte-exact string — one tab showed the live
// strip (keyed "#crypt", never touched again since every server line named
// "#Crypt"), the other ("#Crypt") sat permanently empty. `ChatSessionModel`
// now case-folds every internal room lookup through `roomKey(_:)` while
// keeping a per-room `displayName` (the server's own casing, from the JOIN
// echo) for the tab bar/title.
//
// SERIALIZATION: same reasoning as every other engine-touching suite (the C
// engine activates a process-global `g_session`/metrics-canvas per call) —
// nested inside `EngineGlobalStateSelfTests` (.serialized).
extension EngineGlobalStateSelfTests {
    @Suite(.serialized)
    struct RoomCaseFoldTests {
        /// The exact live-reproduced sequence: connect with room `#crypt`,
        /// server echoes the JOIN as `#Crypt` (server casing) and sends a
        /// PRIVMSG to `#Crypt` — asserts ONE room box/tab (not two), the
        /// message renders on the live strip, and the tab shows the SERVER's
        /// casing ("#Crypt"), not our locally-typed one.
        @Test(.timeLimit(.minutes(1)))
        func serverJoinEchoWithDifferentCaseMergesIntoOneRoom() async throws {
            let server = try LoopbackIRCServer()
            let art = repoRoot5Up().appendingPathComponent("v2.5-beta-1-modern/comicart").path
            let model = ChatSessionModel(config: .init(host: "127.0.0.1", port: server.port,
                                                       nick: "Mac", room: "#crypt", artDir: art))
            let rooms = RCFRoomsBox()
            model.onRoomsChanged = { infos in rooms.set(infos) }

            try await model.start()

            // Drive login manually (rather than the shared
            // replyToProbeWith451ThenWelcomeAndJoin helper) so the JOIN echo
            // can deliberately use DIFFERENT casing than what we joined with.
            try await server.startCollectingReceivedBytes()
            try await server.waitForClientLine(containing: "MODE ISIRCX\r\n")
            try await server.send(":srv 451 * :not registered")
            try await server.waitForClientLine(containing: "USER ")
            try await server.send(":srv 001 Mac :Welcome")
            try await server.waitForClientLine(containing: "JOIN #crypt\r\n")
            try await server.send(
                ":Mac!mac@h JOIN :#Crypt",
                ":srv 353 Mac = #Crypt :Mac",
                ":srv 366 Mac #Crypt :End of NAMES list")

            try await pollUntil { model.roomInfos.first?.name == "#Crypt" }
            #expect(model.roomInfos.count == 1,
                    "server JOIN echo with different case must merge into the SAME room, not create a second tab; got \(model.roomInfos.map(\.name))")
            #expect(model.roomInfos.first?.name == "#Crypt",
                    "the tab must show the SERVER's casing, got \(String(describing: model.roomInfos.first?.name))")
            #expect(model.currentRoom == "#Crypt" || model.currentRoom.caseInsensitiveCompare("#crypt") == .orderedSame,
                    "currentRoom should resolve to the joined room regardless of case, got \(model.currentRoom)")

            // A PRIVMSG to the server-cased channel must land in the SAME
            // room's transcript and render on the live strip (not get
            // silently dropped/misrouted to a phantom second box).
            let panelsBefore = model.panelCount
            try await server.send(":Al!a@h PRIVMSG #Crypt :hello from crypt")
            try await pollUntil { model.panelCount > panelsBefore }
            #expect(model.panelCount > panelsBefore, "a message to the server-cased channel must render on the live strip")

            let transcript = model.transcript
            let hasMessage = transcript.contains {
                if case .text(_, _, _, let text, _, _) = $0 { return text == "hello from crypt" }
                return false
            }
            #expect(hasMessage, "the message must land in the (single) room's transcript")

            model.shutdown()
            server.stop()
        }

        /// `joinRoom("#CRYPT")` (yet another casing) while `#Crypt` already
        /// exists (from the server's own casing) must NOT create a new room —
        /// it activates the existing one instead.
        @Test(.timeLimit(.minutes(1)))
        func joinRoomWithDifferentCaseActivatesExistingRoom() async throws {
            let server = try LoopbackIRCServer()
            let art = repoRoot5Up().appendingPathComponent("v2.5-beta-1-modern/comicart").path
            let model = ChatSessionModel(config: .init(host: "127.0.0.1", port: server.port,
                                                       nick: "Mac", room: "#a", artDir: art))
            let rooms = RCFRoomsBox()
            model.onRoomsChanged = { infos in rooms.set(infos) }

            try await model.start()
            try await server.replyToProbeWith451ThenWelcomeAndJoin(nick: "Mac", channel: "#a")

            // Join #Crypt (server casing established first).
            try await model.joinRoom("#Crypt")
            try await server.waitForClientLine(containing: "JOIN #Crypt")
            try await server.send(
                ":Mac!mac@h JOIN :#Crypt",
                ":srv 353 Mac = #Crypt :Mac",
                ":srv 366 Mac #Crypt :End of NAMES list")
            try await pollUntil { model.roomInfos.count == 2 }
            #expect(model.roomInfos.count == 2)

            // Now joinRoom with yet another casing -- must NOT add a third tab.
            try await model.joinRoom("#CRYPT")
            // No wire JOIN should be needed/expected since we're already in
            // the room canonically; give the model a moment to settle and
            // confirm the room count is unchanged.
            try await withCheckedContinuation { (cont: CheckedContinuation<Void, Never>) in
                model.settleEngineQueue { cont.resume() }
            }
            #expect(model.roomInfos.count == 2,
                    "joinRoom with different case of an existing room must not create a new tab; got \(model.roomInfos.map(\.name))")

            // setActiveRoom with the alternate casing activates the existing room.
            model.setActiveRoom("#CRYPT")
            try await pollUntil { model.currentRoom.caseInsensitiveCompare("#Crypt") == .orderedSame }
            #expect(model.currentRoom.caseInsensitiveCompare("#Crypt") == .orderedSame,
                    "setActiveRoom with different case must activate the existing canonical room, got \(model.currentRoom)")
            #expect(model.roomInfos.count == 2, "still only 2 rooms after activating via a different-cased name")

            model.shutdown()
            server.stop()
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

/// Thread-safe latest-`[RoomInfo]` box, same shape as MultiRoomTests' `MRRoomsBox`.
private final class RCFRoomsBox: @unchecked Sendable {
    private let lock = NSLock()
    private var storage: [RoomInfo] = []
    func set(_ infos: [RoomInfo]) { lock.lock(); defer { lock.unlock() }; storage = infos }
    func all() -> [RoomInfo] { lock.lock(); defer { lock.unlock() }; return storage }
}
