import Foundation
import Observation
import CoreGraphics
import ComicChatKit

@Observable @MainActor
public final class AppState {
    public var model: ChatSessionModel?
    public var settings = SettingsStore()
    public var showConnectSheet = true
    public var statusLine = ""
    /// Plan 4b Task 8: widened from `[String]` to `[MemberRow]` (nick/isOp/
    /// avatarName), mirrored from `ChatSessionModel.onMembers` on the main
    /// thread — `ChatWindow`'s member `List` reads this directly.
    public var members: [MemberRow] = []
    public var stripImage: CGImage?
    public var stripSizePoints: CGSize = .zero

    // MARK: Rooms (Plan 4b Task 7: true multi-room)

    /// The joined rooms in tab order, mirrored from
    /// `ChatSessionModel.onRoomsChanged` on the main thread — `RoomTabBar`'s
    /// data source (tab name + unread badge + active flag).
    public var rooms: [RoomInfo] = []
    /// The active room's name (the tab currently owning the one live strip),
    /// or `nil` before connect. Derived from `rooms` (the active one), kept as
    /// a convenience for the tab bar's selection binding.
    public var activeRoom: String? { rooms.first { $0.isActive }?.name }
    /// Bound to the Enter Room sheet's text field (⌘J / RoomTabBar "+").
    public var showEnterRoomSheet = false
    public var enterRoomText = ""
    /// Bound to the Create Room… sheet's text field (Room menu, Plan 4b Task 8).
    public var showCreateRoomSheet = false
    public var createRoomText = ""
    /// The self avatar's live pose preview (Plan 4b Task 3) — updated after
    /// every emotion-wheel drag or typing-preview via `ChatSessionModel.onSelfPose`.
    public var selfPoseImage: CGImage?

    // MARK: Room list + room ops + member selection (Plan 4b Task 8)

    /// The last-fetched room list (`CRoomList`'s LIST browser), mirrored from
    /// `ChatSessionModel.onRoomList` — `RoomListWindow`'s `Table` data source.
    public var roomList: [RoomListItem] = []
    /// Whether we're the OWNER of the active room (derived from the active
    /// room's own membership, mirrored from `ChatSessionModel.onSelfOp`) —
    /// gates the Kick/Ban context-menu items (`.disabled(!selfIsOp)`).
    public var selfIsOp = false
    /// Our own away state (Plan 4b Task 8) — mirrors the Room menu's Away
    /// toggle; no server confirmation event exists for our OWN away state on
    /// this wire (`.awayPeer` is peer-only, `ProtocolEvents.swift`), so this
    /// is optimistically flipped by `toggleAway()` itself.
    public var isAway = false
    /// The member list's selection (Plan 4b Task 8, D1 §1.5): the original's
    /// canonical talk-to state. Bound to `ChatWindow`'s member `List`
    /// selection; `ChatWindow`'s send passes this straight through to
    /// `ChatSessionModel.send(_:mode:room:addressees:)`.
    public var selectedMembers: Set<String> = []
    /// Resolved member-row avatar icon thumbnails, keyed by avatar name
    /// (Plan 4b Task 8) — populated lazily by `ChatWindow` as rows render
    /// (`AvatarFile(path:).iconImage()` via the model's art dir), so the same
    /// icon is decoded once per session rather than once per row-redraw.
    /// Unresolvable names simply never gain an entry (no icon shown).
    public var memberIconCache: [String: CGImage] = [:]
    /// The last `getInfo(_:)` popover result (Plan 4b Task 8) — `(nick,
    /// formatted)`, mirrored from `ChatSessionModel.onUserInfo`. `nil` clears
    /// any shown popover.
    public var userInfoResult: (nick: String, text: String)?

    /// Requests a fresh room list — `RoomListWindow`'s Refresh button / the
    /// window's `.task`. Fire-and-forget on the model; the result arrives via
    /// `onRoomList` (mirrored into `roomList` by `connect()`'s callback wiring).
    public func requestRoomList() {
        guard let model else { return }
        Task { try? await model.requestRoomList() }
    }

    /// "Go To" a room from the LIST browser (Plan 4b Task 8): join + activate
    /// (opens a NEW tab — no part-first, `ChatSessionModel.goToRoom`'s own doc
    /// comment). Normalizes a bare name the same way `joinRoom(_:)` does.
    public func goToRoom(_ raw: String) {
        guard let channel = normalizedChannel(raw), let model else { return }
        selectedMembers = []   // per-room addressee state — see setActiveRoom's doc comment
        Task { try? await model.goToRoom(channel) }
    }

    /// Creates a room and goes to it (Plan 4b Task 8, the Create Room… sheet):
    /// `createRoom` (the wire CREATE, which server-side joins us) then
    /// `goToRoom` opens the tab locally the same way any other join does.
    public func createRoom(_ raw: String) {
        guard let channel = normalizedChannel(raw), let model else { return }
        selectedMembers = []   // per-room addressee state — see setActiveRoom's doc comment
        Task {
            try? await model.createRoom(channel)
            try? await model.goToRoom(channel)
        }
    }

    /// Toggles the session-scoped Away state (Plan 4b Task 8: the Room menu's
    /// Away toggle) — optimistic local flip (see `isAway`'s doc comment).
    public func toggleAway() {
        guard let model else { return }
        let newValue = !isAway
        isAway = newValue
        Task { try? await model.setAway(newValue) }
    }

    /// Kicks `nick` from the active room (Plan 4b Task 8's member context
    /// menu). No-op with no active room/model.
    public func kick(_ nick: String) {
        guard let model, let room = activeRoom else { return }
        Task { try? await model.kick(room, nick: nick) }
    }

    /// Bans `pattern` (typically `nick!*@*`) from the active room (Plan 4b
    /// Task 8's member context menu).
    public func ban(_ pattern: String) {
        guard let model, let room = activeRoom else { return }
        Task { try? await model.ban(room, pattern: pattern, banning: true) }
    }

    /// Requests Get Info for `nick` (Plan 4b Task 8) — the result arrives via
    /// `onUserInfo`, mirrored into `userInfoResult`.
    public func getInfo(_ nick: String) {
        guard let model else { return }
        Task { try? await model.getInfo(nick) }
    }

    /// In-flight guard for `resolveMemberIcon` (Plan 4b Task 8 self-review
    /// fix) — mirrors `ChatSessionModel`'s own `inFlightAvatarDownloads`
    /// pattern for the identical redundant-fetch problem: SwiftUI `List` can
    /// re-fire a row's `.onAppear` (scroll bounce, membership-churn redraw)
    /// before an earlier decode for the SAME name has completed and
    /// populated `memberIconCache`; without this, each such re-fire would
    /// pass the cache-miss guard and spawn its own redundant file-read +
    /// decode of the same `.avb` icon pose.
    private var inFlightMemberIconResolves: Set<String> = []

    /// Resolves (and caches) `avatarName`'s member-row icon thumbnail —
    /// `AvatarFile(path:).iconImage()` over the path `ChatSessionModel.
    /// resolveAvatarPath(_:)` returns, which searches the SAME
    /// downloaded-avatars-shadow-bundled order the live strip's own
    /// `AvatarResolver` uses (Plan 4b Task 8 self-review fix: this used to
    /// hardcode `artDir` only, so a member wearing a custom/downloaded
    /// avatar that shadows a bundled name showed the WRONG icon here even
    /// though the strip rendered the right one). Silently no-ops for an
    /// empty name, an already-cached name, an already-in-flight name, or a
    /// name that fails to resolve anywhere (unresolvable names get no icon,
    /// per the brief).
    public func resolveMemberIcon(_ avatarName: String) {
        guard !avatarName.isEmpty, let model,
              memberIconCache[avatarName] == nil,
              !inFlightMemberIconResolves.contains(avatarName) else { return }
        inFlightMemberIconResolves.insert(avatarName)
        Task.detached { [weak self] in
            defer { Task { @MainActor in self?.inFlightMemberIconResolves.remove(avatarName) } }
            guard let path = model.resolveAvatarPath(avatarName),
                  let av = try? AvatarFile(path: path),
                  let art = try? av.iconImage(),
                  let cg = art.cgImage() else { return }
            await MainActor.run { self?.memberIconCache[avatarName] = cg }
        }
    }

    // MARK: Whisper box (Plan 4b Task 4)

    /// Peers we have ANY whisper history with, in first-contact order (a
    /// peer is appended the first time `onWhisper` fires for them — inbound
    /// OR our own outbound start of a new whisper). `WhisperBox`'s sidebar
    /// list.
    public var whisperPeers: [String] = []
    /// Mirror of `ChatSessionModel.whisperHistories`, kept in sync from
    /// `onWhisper` on the MAIN thread (the model's own storage is
    /// engine-queue-owned and requires a `engineQueue.sync` hop per read —
    /// this mirror avoids that on every SwiftUI body re-evaluation).
    public var whisperHistories: [String: [WhisperLine]] = [:]
    /// Unread count per peer — incremented on every INBOUND `onWhisper` for a
    /// peer whose tab is not the currently-selected one in `WhisperBox`;
    /// cleared when that peer's tab is selected (see `WhisperBox.selectedPeer`'s
    /// `onChange`). Modern-Mac deviation from the original (noted in the
    /// Task 4 report): the original auto-pops-up `CWhisperBox`/`CWhisperLeaf`
    /// on first inbound whisper from a peer (whisprbx.cpp); this app instead
    /// shows a status line + badge and leaves opening the window to the user
    /// (Room > Whisper…/the member context menu), never auto-opening it.
    public var whisperUnread: [String: Int] = [:]
    /// Set by `showWhisperBox(peer:)`, read (and cleared) by `WhisperBox`'s
    /// `.task`/`.onChange` on window appearance — the hand-off for "which
    /// peer's tab should be selected" across the `openWindow(id:)` boundary,
    /// since `Window` scenes take no per-open parameter in this SwiftUI
    /// version (only a fixed `id`).
    public var pendingWhisperPeer: String?

    /// Requests the whisper box window be opened (via the CALLER's
    /// `@Environment(\.openWindow)`, since `AppState` itself has no window
    /// scene to open) with `peer`'s tab pre-selected, if given. Called from
    /// `ChatWindow`'s member context menu ("Whisper…") and `AppCommands`'
    /// Member menu.
    public func showWhisperBox(peer: String?) {
        if let peer {
            pendingWhisperPeer = peer
            if !whisperPeers.contains(peer) { whisperPeers.append(peer) }
            whisperUnread[peer] = 0
        }
    }

    /// Records one whisper line from `ChatSessionModel.onWhisper` into the
    /// mirrors above. `peer` first-contact order seeds `whisperPeers`;
    /// inbound lines (`!line.isOwn`) bump `whisperUnread[peer]` unless that
    /// peer's tab is the one currently showing (`pendingWhisperPeer == peer`
    /// is NOT sufficient for that check — `WhisperBox` clears unread directly
    /// via its own `onChange(of: selectedPeer)`, so this always increments;
    /// the box's clear-on-select is what actually keeps the badge honest).
    func recordWhisper(peer: String, line: WhisperLine) {
        if !whisperPeers.contains(peer) { whisperPeers.append(peer) }
        whisperHistories[peer, default: []].append(line)
        if !line.isOwn {
            whisperUnread[peer, default: 0] += 1
        }
    }

    /// Switches the one live strip to `room` (Plan 4b Task 7) — the tab bar's
    /// tap handler. Fire-and-forget on the model (engine-queue-hopped there).
    ///
    /// Plan 4b Task 8 self-review fix: clears `selectedMembers` — the
    /// member-list selection is per-room addressee state (D1 §1.5); carrying
    /// a stale selection across a tab switch would silently address whoever
    /// happened to share those nicks (or no one) in the NEW room's member
    /// list on the next send.
    public func setActiveRoom(_ room: String) {
        selectedMembers = []
        model?.setActiveRoom(room)
    }

    /// Joins an ADDITIONAL room on the same connection (Plan 4b Task 7) — the
    /// Enter Room sheet / ⌘J / RoomTabBar "+". Normalizes a bare name to a
    /// channel (`#`-prefixed) the way the connect sheet's room field does.
    public func joinRoom(_ raw: String) {
        guard let channel = normalizedChannel(raw), let model else { return }
        Task { try? await model.joinRoom(channel) }
    }

    /// Trims `raw` and prefixes it with `#` unless it already carries a valid
    /// channel prefix (`#`/`&`) — `nil` for an empty/whitespace-only input.
    /// Shared by `joinRoom`/`goToRoom`/`createRoom` (Plan 4b Task 8
    /// self-review fix: these three previously repeated this ternary
    /// verbatim — factored here so the channel-prefix rule lives in ONE
    /// place).
    private func normalizedChannel(_ raw: String) -> String? {
        let trimmed = raw.trimmingCharacters(in: .whitespaces)
        guard !trimmed.isEmpty else { return nil }
        return trimmed.hasPrefix("#") || trimmed.hasPrefix("&") ? trimmed : "#" + trimmed
    }

    /// Leaves `room` (Plan 4b Task 7) — the tab's close button. If it was
    /// active, the model activates a surviving room (or clears the strip).
    public func leaveRoom(_ room: String) {
        guard let model else { return }
        Task { try? await model.leaveRoom(room) }
    }

    /// Kept alive for the process's whole replay session — `FixtureReplayServer`
    /// services exactly one connection, and its `NWListener`/`NWConnection`
    /// are torn down if this reference drops (Task 12's offline demo hook).
    private var replayServer: FixtureReplayServer?

    /// The app bundle's comicart directory — also read by the Settings
    /// scene's character/backdrop pickers (Task 5), so exposed here rather
    /// than each call site re-deriving `Bundle.main.resourceURL`.
    public var artDir: String {
        Bundle.main.resourceURL!.appendingPathComponent("comicart").path
    }

    public func connect() async {
        // Final review (Plan 4a): tear down any existing session FIRST. Without
        // this, ⌘N -> Connect while already connected builds a SECOND
        // ChatSessionModel (with its own engine queue driving the same
        // process-global engine state -- a two-serial-queues hazard) and
        // drops the old `model` reference without ever calling its mandatory
        // `shutdown()`. `disconnect()` is idempotent (safe to call with no
        // active session).
        disconnect()

        var cfg = ChatConfig(host: settings.server, port: UInt16(exactly: settings.port) ?? 6667,
                             nick: settings.nick, room: settings.room,
                             encoding: settings.encoding,
                             characterName: settings.character,
                             backdropName: settings.backdrop, artDir: artDir,
                             // Plan 4b Task 5: persona plumbing + protocol
                             // toggles, read straight from settings. `realName`
                             // empty -> `nil` (preserves ProtocolSession's own
                             // nick-fallback default rather than sending an
                             // explicit empty string); `userName` is not yet a
                             // distinct settings field (no separate "USER
                             // <user>" UI control exists — the persona tab
                             // exposes nick + real name only, matching the
                             // brief's Persona-tab field list), so it stays
                             // `nil` (own_user falls back to nick, same as
                             // before this task).
                             realName: settings.realName.isEmpty ? nil : settings.realName,
                             sendComicsData: settings.sendComicsData,
                             acceptWhispers: settings.acceptWhispers,
                             // Plan 4b Task 6: gates the .appearsAs-triggered
                             // avatar auto-download.
                             autoDownloadAvatars: settings.autoDownloadAvatars)

        // Offline demo hook (Task 12): `--replay-fixture <path>` starts a
        // FixtureReplayServer over the given capture-shaped .jsonl and
        // overrides host/port/nick/room to match it, so the MVP is
        // demonstrable with zero network and real 1998-client bytes. The
        // fixture's recorded nick/room MUST match what we connect with —
        // FixtureReplayServer does not rewrite them (see that type's doc
        // comment) — so this is hardcoded to the fixture actually used here
        // (`hand-authored-annotation.jsonl`: nick "Anon", room "#comicrig" —
        // chosen over `smoke-2-replay.jsonl` because it is the one fixture
        // that carries a peer's annotated/cooked-pose PRIVMSG, verified
        // empirically to render a second strip panel; the login-only fixture
        // renders just the title panel).
        if let path = replayFixturePath() {
            do {
                let server = try FixtureReplayServer(fixtureURL: URL(fileURLWithPath: path))
                try server.start()
                replayServer = server
                cfg.host = "127.0.0.1"
                cfg.port = server.port
                cfg.nick = "Anon"
                cfg.room = "#comicrig"
            } catch {
                statusLine = "Replay fixture failed to start: \(error)"
                showConnectSheet = true
                return
            }
        }

        let m = ChatSessionModel(config: cfg)
        m.onStripImage = { [weak self] img, size in
            Task { @MainActor in self?.stripImage = img; self?.stripSizePoints = size } }
        m.onMembers = { [weak self] rows in Task { @MainActor in self?.members = rows } }
        m.onStatus = { [weak self] s in Task { @MainActor in self?.statusLine = s } }
        m.onSelfPose = { [weak self] img in Task { @MainActor in self?.selfPoseImage = img } }
        m.onWhisper = { [weak self] peer, line in
            Task { @MainActor in self?.recordWhisper(peer: peer, line: line) } }
        m.onRoomsChanged = { [weak self] infos in
            Task { @MainActor in self?.rooms = infos } }
        m.onRoomList = { [weak self] items in Task { @MainActor in self?.roomList = items } }
        m.onSelfOp = { [weak self] isOp in Task { @MainActor in self?.selfIsOp = isOp } }
        m.onUserInfo = { [weak self] nick, text in
            Task { @MainActor in self?.userInfoResult = (nick: nick, text: text) } }
        model = m
        do { try await m.start(); showConnectSheet = false }
        catch { statusLine = "Connect failed: \(error)" }
    }

    /// Reads `--replay-fixture <path>` out of the process arguments, if
    /// present (a simple positional lookup — no need for a full argument
    /// parser for one demo-only flag).
    private func replayFixturePath() -> String? {
        let args = ProcessInfo.processInfo.arguments
        guard let flagIndex = args.firstIndex(of: "--replay-fixture"), flagIndex + 1 < args.count else {
            return nil
        }
        return args[flagIndex + 1]
    }

    /// Tears down the current session (Task 9 review: `shutdown()` is
    /// mandatory before dropping a `ChatSessionModel` — no `deinit`, so
    /// dropping one without calling this leaves a dangling engine global).
    /// Used by both the Room > Leave/Disconnect command and as the safety
    /// net before starting a fresh connection.
    public func disconnect() {
        model?.shutdown()
        model = nil
        members = []
        stripImage = nil
        stripSizePoints = .zero
        selfPoseImage = nil
        rooms = []
        showEnterRoomSheet = false
        enterRoomText = ""
        showCreateRoomSheet = false
        createRoomText = ""
        roomList = []
        selfIsOp = false
        isAway = false
        selectedMembers = []
        memberIconCache = [:]
        inFlightMemberIconResolves = []
        userInfoResult = nil
        replayServer?.stop()
        replayServer = nil
        whisperPeers = []
        whisperHistories = [:]
        whisperUnread = [:]
        pendingWhisperPeer = nil
    }
}
