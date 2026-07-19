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
    public var members: [String] = []
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
    /// The self avatar's live pose preview (Plan 4b Task 3) — updated after
    /// every emotion-wheel drag or typing-preview via `ChatSessionModel.onSelfPose`.
    public var selfPoseImage: CGImage?

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
    public func setActiveRoom(_ room: String) {
        model?.setActiveRoom(room)
    }

    /// Joins an ADDITIONAL room on the same connection (Plan 4b Task 7) — the
    /// Enter Room sheet / ⌘J / RoomTabBar "+". Normalizes a bare name to a
    /// channel (`#`-prefixed) the way the connect sheet's room field does.
    public func joinRoom(_ raw: String) {
        let trimmed = raw.trimmingCharacters(in: .whitespaces)
        guard !trimmed.isEmpty, let model else { return }
        let channel = trimmed.hasPrefix("#") || trimmed.hasPrefix("&") ? trimmed : "#" + trimmed
        Task { try? await model.joinRoom(channel) }
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
        m.onMembers = { [weak self] nicks in Task { @MainActor in self?.members = nicks } }
        m.onStatus = { [weak self] s in Task { @MainActor in self?.statusLine = s } }
        m.onSelfPose = { [weak self] img in Task { @MainActor in self?.selfPoseImage = img } }
        m.onWhisper = { [weak self] peer, line in
            Task { @MainActor in self?.recordWhisper(peer: peer, line: line) } }
        m.onRoomsChanged = { [weak self] infos in
            Task { @MainActor in self?.rooms = infos } }
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
        replayServer?.stop()
        replayServer = nil
        whisperPeers = []
        whisperHistories = [:]
        whisperUnread = [:]
        pendingWhisperPeer = nil
    }
}
