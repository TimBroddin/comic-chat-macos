import Foundation
import Observation
import CoreGraphics
import AVFoundation
import AppKit
import SwiftUI
import ComicChatKit

@Observable @MainActor
public final class AppState {
    public var model: ChatSessionModel?
    public var settings = SettingsStore()
    public var showConnectSheet = true
    public var statusLine = ""
    /// Live-fix 4 (Tim's request: "show the MOTD like the original client";
    /// also fixes error-notes being clobbered by the next status line) — the
    /// accumulating server-messages console (`ServerConsoleWindow`), mirrored
    /// from `ChatSessionModel.onServerMessage` on the main thread. Capped at
    /// `serverMessagesCap` (drop-oldest) so a long session's console doesn't
    /// grow unbounded.
    public var serverMessages: [String] = []
    private let serverMessagesCap = 500
    /// Plan 4b Task 11: an `@Observable`-TRACKED mirror of
    /// `settings.comicMode`. `SettingsStore` is a thin `UserDefaults`
    /// wrapper with no change notification of its own (unlike this class,
    /// which is `@Observable`) — a raw `settings.comicMode = ...` write from
    /// the View menu's `CommandGroup` would silently fail to re-render
    /// `ChatWindow`'s body, since nothing `@Observable`-tracked actually
    /// changed. This property is the fix: it's what the View menu toggles
    /// and what `ChatWindow` reads, `didSet` writes through to
    /// `settings.comicMode` so the choice still persists across launches
    /// (same UserDefaults-backed persistence, just with a live-UI-reactive
    /// front door). Seeded from `settings.comicMode` at init so a relaunch
    /// picks up the last persisted choice.
    public var comicMode = true {
        didSet { settings.comicMode = comicMode }
    }
    /// Plan 4b Task 8: widened from `[String]` to `[MemberRow]` (nick/isOp/
    /// avatarName), mirrored from `ChatSessionModel.onMembers` on the main
    /// thread — `ChatWindow`'s member `List` reads this directly.
    public var members: [MemberRow] = []
    public var stripImage: CGImage?
    public var stripSizePoints: CGSize = .zero
    /// Plan 4b Task 11: the plain-text transcript view's content — the ACTIVE
    /// room's transcript, run through `TranscriptTextBuilder`. Recomputed
    /// from `model.transcript`/`model.sessionEvents` on every strip update
    /// (`onStripImage`, which fires on every recompose — the same signal
    /// `stripImage` itself updates on) and on every status/error line
    /// (`onStatus`, which the comic view doesn't otherwise need to observe,
    /// but the text view's status/error lines DO come from there), on
    /// tab switch (`handleRoomsChanged`, since the active room — and so which
    /// transcript this shows — changes there), AND on every sound event
    /// (`playSound` — `.sound` renders no strip panel so `onStripImage` never
    /// fires for one, and it sets `statusLine` directly rather than through
    /// `onStatus`, so without its own explicit recompute a `.sound` line
    /// would silently never reach the text view; see that method's own doc
    /// comment). Cheap at chat scale (a full rebuild of a plain-text
    /// `AttributedString` from the whole event log each time); a
    /// `.text`-count-gated cache is the brief's fallback if
    /// that ever stops being true.
    public var transcriptText = AttributedString()

    // MARK: Rooms (Plan 4b Task 7: true multi-room)

    /// The joined rooms in tab order, mirrored from
    /// `ChatSessionModel.onRoomsChanged` on the main thread — `RoomTabBar`'s
    /// data source (tab name + unread badge + active flag).
    public var rooms: [RoomInfo] = []
    /// The active room's name (the tab currently owning the one live strip),
    /// or `nil` before connect. Derived from `rooms` (the active one), kept as
    /// a convenience for the tab bar's selection binding.
    public var activeRoom: String? { rooms.first { $0.isActive }?.name }
    /// The active room name as of the LAST `onRoomsChanged` delivery (Fix
    /// round 1, review Important #1) — lets `handleRoomsChanged` detect when
    /// the active room actually CHANGES (vs. an unrelated `rooms` update,
    /// e.g. an unread-count bump) so it clears `selectedMembers` exactly
    /// once per real switch, not on every snapshot.
    private var lastKnownActiveRoom: String?
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
    /// Names `resolveMemberIcon` tried and failed to resolve to any art
    /// (Plan 4b live-fix: gray-placeholder bug) — keeps `memberIconCache`
    /// itself a clean "name -> successfully decoded icon" map while still
    /// letting a failed name be recognized as "already tried" so a SwiftUI
    /// `List` re-firing the same row's `.onAppear` (scroll bounce,
    /// membership-churn redraw) doesn't re-attempt the same doomed file
    /// read + decode forever. `MemberRowView` renders no icon either way
    /// (cache miss or known-unresolvable) — this only stops the retry loop.
    public var unresolvableMemberIcons: Set<String> = []
    /// The last `getInfo(_:)` popover result (Plan 4b Task 8) — `(nick,
    /// formatted)`, mirrored from `ChatSessionModel.onUserInfo`. `nil` clears
    /// any shown popover.
    public var userInfoResult: (nick: String, text: String)?

    // MARK: Sounds (Plan 4b Task 9)

    /// The one live `AVAudioPlayer`, held so playback isn't torn down by ARC
    /// the instant `playSound` returns (an unretained local player stops
    /// mid-clip). A new `.sound` event replaces it outright — matching the
    /// original's one-sound-at-a-time posture, no mixing/queueing.
    private var soundPlayer: AVAudioPlayer?

    /// Creates the user sounds folder on first launch, if it doesn't already
    /// exist (D1 §4.2 / spec §5 amendment: it ships EMPTY — no bundled WAVs
    /// exist anywhere in this repo's trees, so there is nothing to seed it
    /// with; the user drops files in themselves). Uses `settings.soundsFolder`
    /// (Task 5's setting, not a hardcoded string) so a user-relocated folder
    /// is respected on every subsequent launch — `createDirectory` is a no-op
    /// once the target already exists.
    public init() {
        try? FileManager.default.createDirectory(
            atPath: settings.soundsFolder, withIntermediateDirectories: true)
        // Seed the observable mirror from the persisted setting (Plan 4b
        // Task 11) — done here, AFTER `settings` exists, rather than as
        // `comicMode`'s own default literal, so a relaunch picks up whatever
        // was last persisted instead of always starting `true`. Assigning
        // here does fire `comicMode`'s `didSet`, which just writes the same
        // value straight back to `settings.comicMode` — a harmless no-op
        // re-write, not a reset.
        comicMode = settings.comicMode
    }

    /// Handles one inbound `.sound` event (`ChatSessionModel.onSound`, wired
    /// in `connect()`) — Plan 4b Task 9. Playback is gated on
    /// `settings.soundsEnabled`; the status line ALWAYS notes the event
    /// (`"<nick> played <file>"`), regardless of whether playback is enabled
    /// or the file resolves — playback is an overlay on top of the always-visible
    /// status note, never a replacement for it (matches the brief's "visible
    /// either way" wording).
    private func playSound(nick: String, file: String) {
        statusLine = "\(nick) played \(file)"
        // Plan 4b Task 11: `.sound` events render NO strip panel
        // (`ChatSessionModel.handleLocked`'s `.sound` case is audio-only —
        // no `bridge.apply`/`recomposeLocked()`), so `onStripImage` never
        // fires for one, and this method sets `statusLine` directly rather
        // than through `onStatus` — neither of `transcriptText`'s other two
        // recompute triggers would ever see a `.sound` event otherwise, even
        // though it already landed in the transcript
        // (`ChatSessionModel.handleLocked`'s transcript-routing block runs
        // for every event, `.sound` included, before this callback even
        // fires) and IS one of `TranscriptTextBuilder`'s styled kinds
        // (`♪ nick played file`). This is the third, sound-specific
        // recompute trigger.
        recomputeTranscriptText()
        guard settings.soundsEnabled else { return }
        let folder = URL(fileURLWithPath: settings.soundsFolder)
        guard let resolved = SoundLibrary(folder: folder).resolve(file) else { return }
        soundPlayer = try? AVAudioPlayer(contentsOf: resolved)
        soundPlayer?.play()
    }

    // MARK: Save/reopen transcript + PNG/PDF export + print (Plan 4b Task 10)

    /// The reopened-transcript viewer's composed image, or `nil` when no
    /// viewer is showing. `ChatWindow`'s "Transcript Viewer" window scene
    /// reads this (mirrors `stripImage`'s own posture, but kept SEPARATE from
    /// the live strip — reopening a saved transcript must never touch the
    /// live session's own `stripImage`, which doesn't exist anyway while
    /// disconnected, the only state `openTranscript()` permits).
    public var viewerImage: CGImage?
    public var viewerSizePoints: CGSize = .zero
    /// Set by `openTranscript()` once a render succeeds. `ComicChatApp`'s
    /// "Transcript" `Window` scene (fixed `id: "transcriptViewer"`, same
    /// precedent as "whispers"/"roomList") is a plain always-open scene, not
    /// gated on this itself — `AppCommands`' "Open Transcript…" button reads
    /// this flag right after calling `openTranscript(...)` to decide whether
    /// to actually call `openWindow(id: "transcriptViewer")` (a failed/
    /// refused open must not pop an empty viewer window).
    public var showTranscriptViewer = false

    /// Save Transcript… (⌘S): snapshots the ACTIVE room's conversation via
    /// `ChatSessionModel.conversationFile()` and writes it to a user-chosen
    /// `.json` path via `NSSavePanel`. No-op with no live session (the menu
    /// item is disabled in that case — see `AppCommands`).
    public func saveTranscript() {
        guard let model else { return }
        let file = model.conversationFile()
        let panel = NSSavePanel()
        panel.allowedContentTypes = [.json]
        panel.nameFieldStringValue = (file.room.isEmpty ? "conversation" : file.room)
            .trimmingCharacters(in: CharacterSet(charactersIn: "#&")) + ".json"
        panel.canCreateDirectories = true
        guard panel.runModal() == .OK, let url = panel.url else { return }
        do {
            try file.write(to: url)
        } catch {
            statusLine = "Save Transcript failed: \(error)"
        }
    }

    /// Open Transcript… : REFUSES while connected (plan-review DECISION,
    /// binding — a second concurrent `Strip` while a live session holds the
    /// one process-global engine slot is engine UB, not a UI choice; see
    /// `TranscriptRenderer`'s own doc comment). Presents an alert instead of
    /// silently no-opping so the user understands why nothing happened.
    /// Otherwise: `NSOpenPanel` for a `.json` file, decode via
    /// `ConversationFile.read(from:)`, render via `TranscriptRenderer` at the
    /// given viewport width (the CURRENT window width, matching a live
    /// session's own `setViewport` geometry derivation), and present the
    /// result in the transcript viewer.
    public func openTranscript(currentWindowWidthPoints: CGFloat) {
        guard model == nil else {
            let alert = NSAlert()
            alert.messageText = "Disconnect First"
            alert.informativeText = "Opening a saved transcript renders it through the same comic engine as a live session, which only supports one strip at a time. Disconnect the current session before opening a saved transcript."
            alert.alertStyle = .warning
            alert.runModal()
            return
        }
        let panel = NSOpenPanel()
        panel.allowedContentTypes = [.json]
        panel.canChooseDirectories = false
        panel.canChooseFiles = true
        panel.allowsMultipleSelection = false
        guard panel.runModal() == .OK, let url = panel.url else { return }

        // Final-review Important #3 (gate race): `NSOpenPanel.runModal()`
        // above pumps the main-queue run loop for as long as the panel is
        // showing — a `connect()` already queued on the main queue (e.g. the
        // user had also just hit Connect) can land and fully establish a
        // live session WHILE the panel is up. The `model == nil` check
        // above only proves there was no session at the moment the panel was
        // OPENED; it says nothing about the moment it CLOSED. Re-checking
        // here, immediately after `runModal()` returns and before any
        // decode/render happens, catches that window and refuses with the
        // same alert rather than building a second concurrent `Strip`
        // against a now-live session (engine UB — the same one-strip-at-a-
        // time hazard the FIRST check exists for).
        guard model == nil else {
            let alert = NSAlert()
            alert.messageText = "Disconnect First"
            alert.informativeText = "A session connected while the Open panel was showing. Opening a saved transcript renders it through the same comic engine as a live session, which only supports one strip at a time. Disconnect the current session before opening a saved transcript."
            alert.alertStyle = .warning
            alert.runModal()
            return
        }

        do {
            let file = try ConversationFile.read(from: url)
            let viewportTwips = Int32((currentWindowWidthPoints * 20).rounded())
            let columns = PanelFit.columns(forViewportWidthTwips: viewportTwips)
            let unit = PanelFit.unitPanelTwips(viewportWidthTwips: viewportTwips, columns: columns)
            let renderer = TranscriptRenderer(file: file, artDir: artDir)
            let (image, _) = try renderer.render(columns: columns, unitTwips: unit, scale: 2.0)
            viewerImage = image
            viewerSizePoints = CGSize(width: CGFloat(image.width) / 2.0, height: CGFloat(image.height) / 2.0)
            showTranscriptViewer = true
        } catch {
            statusLine = "Open Transcript failed: \(error)"
        }
    }

    /// Export as PNG… : the CURRENT live strip's last composed image
    /// (`stripImage`) written to a user-chosen `.png` path. No-op with
    /// nothing composed yet.
    public func exportPNG() {
        guard let image = stripImage else { return }
        guard let png = Self.pngData(for: image) else { return }
        let panel = NSSavePanel()
        panel.allowedContentTypes = [.png]
        panel.nameFieldStringValue = "comic.png"
        panel.canCreateDirectories = true
        guard panel.runModal() == .OK, let url = panel.url else { return }
        do {
            try png.write(to: url, options: .atomic)
        } catch {
            statusLine = "Export as PNG failed: \(error)"
        }
    }

    /// Export as PDF… : the current live strip's composed image, scaled to
    /// fit an `NSPrintInfo`-derived page width with vertical pagination, via
    /// `ComicPrintView.dataWithPDF(inside:)` (no print panel — a direct
    /// save, unlike `printTranscript()`'s interactive `NSPrintOperation`).
    public func exportPDF() {
        guard let image = stripImage else { return }
        let panel = NSSavePanel()
        panel.allowedContentTypes = [.pdf]
        panel.nameFieldStringValue = "comic.pdf"
        panel.canCreateDirectories = true
        guard panel.runModal() == .OK, let url = panel.url else { return }
        let printInfo = NSPrintInfo.shared
        let pageWidth = printInfo.paperSize.width - printInfo.leftMargin - printInfo.rightMargin
        let printView = ComicPrintView(image: image, pageWidthPoints: max(1, pageWidth))
        let data = printView.dataWithPDF(inside: printView.bounds)
        do {
            try data.write(to: url, options: .atomic)
        } catch {
            statusLine = "Export as PDF failed: \(error)"
        }
    }

    /// Print… (⌘P): the current live strip's composed image through an
    /// interactive `NSPrintOperation` (which also offers its own "Save as
    /// PDF…" button — the brief's "PDF export = the print panel's PDF
    /// button, plus a direct save for Export as PDF").
    public func printTranscript() {
        guard let image = stripImage else { return }
        let printInfo = NSPrintInfo.shared
        let pageWidth = printInfo.paperSize.width - printInfo.leftMargin - printInfo.rightMargin
        let printView = ComicPrintView(image: image, pageWidthPoints: max(1, pageWidth))
        let operation = NSPrintOperation(view: printView, printInfo: printInfo)
        operation.run()
    }

    /// PNG-encodes a `CGImage` via ImageIO — the app-side twin of
    /// `CGCanvas.pngData()` (that method encodes from its OWN live
    /// `CGContext`; this one encodes an already-composed `CGImage` handed
    /// back through `onStripImage`, which is all `AppState` retains).
    private static func pngData(for image: CGImage) -> Data? {
        let mutableData = CFDataCreateMutable(nil, 0)
        guard let mutableData,
              let dest = CGImageDestinationCreateWithData(mutableData, "public.png" as CFString, 1, nil)
        else { return nil }
        CGImageDestinationAddImage(dest, image, nil)
        guard CGImageDestinationFinalize(dest) else { return nil }
        return mutableData as Data
    }

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
    ///
    /// Fix round 1 (review Important #1): no longer clears `selectedMembers`
    /// here directly — see `onRoomsChanged`'s doc comment for why the
    /// chokepoint moved there.
    public func goToRoom(_ raw: String) {
        guard let channel = normalizedChannel(raw), let model else { return }
        Task { try? await model.goToRoom(channel) }
    }

    /// Creates a room and goes to it (Plan 4b Task 8, the Create Room… sheet):
    /// `createRoom` (the wire CREATE, which server-side joins us) then
    /// `goToRoom` opens the tab locally the same way any other join does.
    ///
    /// Fix round 1 (review Important #1): no longer clears `selectedMembers`
    /// here directly — see `onRoomsChanged`'s doc comment for why the
    /// chokepoint moved there.
    public func createRoom(_ raw: String) {
        guard let channel = normalizedChannel(raw), let model else { return }
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
    /// name already known unresolvable (per `unresolvableMemberIcons`).
    /// Unresolvable names get no icon, per the brief — and get RECORDED as
    /// unresolvable (live-fix) so a re-fired `.onAppear` for the same name
    /// doesn't re-attempt the same doomed file read + decode forever.
    public func resolveMemberIcon(_ avatarName: String) {
        guard !avatarName.isEmpty, let model,
              memberIconCache[avatarName] == nil,
              !unresolvableMemberIcons.contains(avatarName),
              !inFlightMemberIconResolves.contains(avatarName) else { return }
        inFlightMemberIconResolves.insert(avatarName)
        Task.detached { [weak self] in
            defer { Task { @MainActor in self?.inFlightMemberIconResolves.remove(avatarName) } }
            guard let path = model.resolveAvatarPath(avatarName),
                  let av = try? AvatarFile(path: path),
                  let art = try? av.iconImage(),
                  let cg = art.cgImage() else {
                await MainActor.run { self?.unresolvableMemberIcons.insert(avatarName) }
                return
            }
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
    /// Fix round 1 (review Important #1): no longer clears `selectedMembers`
    /// here directly — see `onRoomsChanged`'s doc comment for why the
    /// chokepoint moved there.
    public func setActiveRoom(_ room: String) {
        model?.setActiveRoom(room)
    }

    /// THE single chokepoint (Fix round 1, review Important #1) for clearing
    /// `selectedMembers` on an active-room change. Wired to
    /// `ChatSessionModel.onRoomsChanged` in `connect()` — every path that
    /// changes the active room ends up here, because `onRoomsChanged`/
    /// `emitRooms()` fires on every one of them (`setActiveRoom`, `goToRoom`,
    /// `createRoom`, and — the bug this fixes — `ChatSessionModel.leaveRoom`'s
    /// OWN internal fallback-activation of a surviving room when the CLOSED
    /// tab was the active one, which reassigns the active room without ever
    /// routing back through `AppState.setActiveRoom`).
    ///
    /// Previously each call site (`setActiveRoom`/`goToRoom`/`createRoom`)
    /// cleared `selectedMembers` itself. That missed `leaveRoom`'s fallback
    /// activation entirely: closing the ACTIVE room's tab carries the stale
    /// nick selection into whichever room the model falls back to, silently
    /// addressing whoever happens to share those nicks (or no one) on the
    /// next send. Moving the clear here, keyed on the active room NAME
    /// actually changing between deliveries, covers every path uniformly
    /// (including `leaveRoom`) and removes the need to remember to clear it
    /// at each new call site.
    ///
    /// `disconnect()` keeps its OWN clear (not covered here): the rooms list
    /// empties there without necessarily routing through this handler for a
    /// final "no active room" transition, and it also resets
    /// `lastKnownActiveRoom` so a fresh connect starts clean.
    private func handleRoomsChanged(_ infos: [RoomInfo]) {
        rooms = infos
        let newActive = infos.first { $0.isActive }?.name
        if newActive != lastKnownActiveRoom {
            selectedMembers = []
        }
        lastKnownActiveRoom = newActive
        // Plan 4b Task 11 (post-Task-7 note): the active room can change here
        // without any strip recompose in between (e.g. switching to a room
        // whose strip is already up to date) — the text view must still
        // re-point at the new active room's transcript.
        recomputeTranscriptText()
    }

    /// Plan 4b Task 11: rebuilds `transcriptText` from the active room's
    /// transcript (`model.transcript`) plus the session-scoped events
    /// (`model.sessionEvents` — login/status/error/whisper-to-self lines that
    /// belong to no single room). `nil` model (not connected, or between
    /// `disconnect()` and a fresh `connect()`) clears it. Room-scoped
    /// whispers are already mixed into `model.transcript` (they're room
    /// events when room-scoped — `ChatSessionModel.handleLocked`'s `.whisper`
    /// case appends to the active room's transcript unconditionally), so no
    /// separate whisper feed is merged in here beyond that.
    ///
    /// ORDERING CAVEAT (documented, not silently swept under the rug):
    /// `model.transcript`/`model.sessionEvents` are two SEPARATE arrays with
    /// no shared sequence number or timestamp (`RoomBox`/`ChatSessionModel`'s
    /// own storage — see those types' doc comments), so this is a
    /// concatenation, NOT a true chronological merge: every session-scoped
    /// line (status/error/login) renders AFTER the entire room transcript,
    /// regardless of when it actually arrived relative to the room's chat.
    /// Session-scoped events are rare relative to room chat (one login, the
    /// occasional error/status note) and still fully visible in-order
    /// RELATIVE TO EACH OTHER, so this is judged an acceptable simplification
    /// for this task rather than plumbing a global sequence counter through
    /// `ChatSessionModel` — flagged here for a future task if strict
    /// interleaving is ever required.
    private func recomputeTranscriptText() {
        guard let model else { transcriptText = AttributedString(); return }
        // Final-review Important #4a: `model.sessionEvents` carries every
        // SESSION-scoped event, including inbound plain-IRC whispers
        // (`.text` shaped as a PRIVMSG-to-self — `ChatSessionModel
        // .handleLocked`'s own `.text` case routes these to the whisper box
        // via the SAME `isWhisperShapedText` predicate used here) and IRCX
        // `.whisper` events with no room token. Against the "whisper box,
        // NOT text view" coordinator ruling (Important #4b), those must NOT
        // also leak into this plain-text transcript — `TranscriptTextBuilder`
        // has no room/session distinction of its own, so the filter has to
        // happen at this call site, on the raw event feed, before it ever
        // reaches the builder. Room-scoped whispers are UNAFFECTED (they're
        // already mixed into `model.transcript`, not `sessionEvents`, and are
        // real room chat, not a private aside) — this is systematic on §8
        // Topology A (every plain-IRC whisper takes this shape), not a rare
        // edge case.
        let ownNick = model.ownNick
        let sessionOnly = model.sessionEvents.filter { event in
            if case .whisper = event { return false }
            if ProtocolEvent.isWhisperShapedText(event, ownNick: ownNick) { return false }
            return true
        }
        let events = model.transcript + sessionOnly
        transcriptText = TranscriptTextBuilder.attributedString(for: events)
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
            Task { @MainActor in
                self?.stripImage = img; self?.stripSizePoints = size
                // Plan 4b Task 11: every strip recompose means the active
                // room's transcript grew (or the room itself changed) — the
                // text view mirrors the same signal the comic view uses.
                self?.recomputeTranscriptText()
            } }
        m.onMembers = { [weak self] rows in Task { @MainActor in self?.members = rows } }
        m.onStatus = { [weak self] s in
            Task { @MainActor in
                self?.statusLine = s
                // Plan 4b Task 11: status/error lines are session-scoped
                // (`.statusLine`/`.error`/`.disconnectedHint`) and don't
                // necessarily trigger a strip recompose — the text view needs
                // its own recompute here to pick them up.
                self?.recomputeTranscriptText()
            } }
        m.onSelfPose = { [weak self] img in Task { @MainActor in self?.selfPoseImage = img } }
        m.onWhisper = { [weak self] peer, line in
            Task { @MainActor in self?.recordWhisper(peer: peer, line: line) } }
        m.onRoomsChanged = { [weak self] infos in
            Task { @MainActor in self?.handleRoomsChanged(infos) } }
        m.onRoomList = { [weak self] items in Task { @MainActor in self?.roomList = items } }
        m.onSelfOp = { [weak self] isOp in Task { @MainActor in self?.selfIsOp = isOp } }
        m.onUserInfo = { [weak self] nick, text in
            Task { @MainActor in self?.userInfoResult = (nick: nick, text: text) } }
        m.onSound = { [weak self] nick, file in
            Task { @MainActor in self?.playSound(nick: nick, file: file) } }
        m.onNickRejected = { [weak self] badNick, text in
            Task { @MainActor in self?.handleNickRejected(badNick: badNick, text: text) } }
        m.onServerMessage = { [weak self] text in
            Task { @MainActor in self?.appendServerMessage(text) } }
        model = m
        do { try await m.start(); showConnectSheet = false }
        catch {
            statusLine = "Connect failed: \(error)"
            // Final-review minor: `m.start()` can throw AFTER `model = m`
            // above already published the half-started session (e.g. the TCP
            // connect itself fails) — without this, `model` stays non-nil
            // pointing at a `ChatSessionModel` that was never `shutdown()`'d
            // (the mandatory-teardown contract `disconnect()`'s own doc
            // comment states: "no `deinit`, so dropping one without calling
            // this leaves a dangling engine global") and whose UI surface
            // (member list, rooms, strip) never got a chance to populate
            // either, leaving the app in a half-connected-looking limbo.
            // Mirrors `disconnect()`'s own model-teardown + replay-server
            // stop subset (not the full reset — this path hasn't shown any
            // session UI yet, so there's nothing else to clear).
            m.shutdown()
            model = nil
            replayServer?.stop()
            replayServer = nil
        }
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
        transcriptText = AttributedString()
        selfPoseImage = nil
        rooms = []
        showEnterRoomSheet = false
        enterRoomText = ""
        showCreateRoomSheet = false
        createRoomText = ""
        roomList = []
        selfIsOp = false
        isAway = false
        // Kept here even though `handleRoomsChanged` is the chokepoint for
        // every OTHER active-room transition (Fix round 1): `rooms = []`
        // above may not route back through `onRoomsChanged`/
        // `handleRoomsChanged` at all (no model to fire it once `shutdown()`
        // has run), so this is the one path that still needs its own
        // explicit clear. Also resets `lastKnownActiveRoom` so a fresh
        // `connect()` starts from a clean "no room seen yet" state rather
        // than comparing against a stale name from the torn-down session.
        selectedMembers = []
        lastKnownActiveRoom = nil
        memberIconCache = [:]
        unresolvableMemberIcons = []
        inFlightMemberIconResolves = []
        userInfoResult = nil
        replayServer?.stop()
        replayServer = nil
        whisperPeers = []
        whisperHistories = [:]
        whisperUnread = [:]
        pendingWhisperPeer = nil
        soundPlayer?.stop()
        soundPlayer = nil
    }

    /// Live-fix 3 (crypthome.com, live-reproduced: `432 Anonymous :Reserved
    /// name` dead-ended the session with only a transient status line).
    /// `ChatSessionModel.onNickRejected` routes here: shows an alert
    /// explaining the rejection, then tears down the now-dead session and
    /// reopens the connect sheet (mirrors the failed-connect teardown path
    /// in `connect()`'s own `catch` block — same "no half-connected limbo"
    /// posture, just triggered by a LATER server reply instead of the
    /// initial `start()` throwing).
    private func handleNickRejected(badNick: String, text: String) {
        let alert = NSAlert()
        alert.messageText = "Nickname Rejected"
        alert.informativeText = text
        alert.alertStyle = .warning
        alert.runModal()
        disconnect()
        showConnectSheet = true
    }

    /// Live-fix 4: `ChatSessionModel.onServerMessage` routes here —
    /// appends one line to the accumulating server-messages console,
    /// dropping the oldest once `serverMessagesCap` is exceeded.
    private func appendServerMessage(_ text: String) {
        serverMessages.append(text)
        if serverMessages.count > serverMessagesCap {
            serverMessages.removeFirst(serverMessages.count - serverMessagesCap)
        }
    }
}
