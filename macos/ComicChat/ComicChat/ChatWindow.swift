import SwiftUI
import ComicChatKit

/// The MVP chat window (Plan 4a Task 12, the plan's exit-milestone task):
/// comic strip + compose bar + status line on the left, member sidebar on
/// the right. Replaces `ContentRoot` as the app's root view.
///
/// Quick-wins batch item 1 (the user's explicit ask: "a macOS sidebar instead
/// of tabs"): the root is now a `NavigationSplitView` — a native rooms
/// SIDEBAR (`RoomSidebar`, replacing the retired `RoomTabBar.swift`) as the
/// split view's own sidebar column, with the ORIGINAL two-pane layout (strip+
/// compose+status | member grid+pose+wheel) as its `detail`. The sidebar is
/// collapsible for free via the standard toolbar toggle `NavigationSplitView`
/// already supplies. PRESERVED from the tab-bar era: active-room semantics
/// (`RoomSidebar`'s selection binding drives `appState.setActiveRoom`, same
/// call the old tab tap made), unread clearing on activation (unchanged —
/// `ChatSessionModel.setActiveRoom` zeroes the newly-active room's unread
/// itself, upstream of any UI), the `handleRoomsChanged` selection-clear
/// chokepoint (untouched — this view only ever calls `setActiveRoom`, the
/// same entry point the tab bar called), and the `--replay-fixture`
/// single-room path (the sidebar always renders, even with exactly one room —
/// matching the retired tab bar's own "always renders the single tab"
/// posture, so the fixture demo still shows a consistent affordance).
///
/// `ComicStripView` construction (review correction to this task's brief):
/// `ComicStripView` takes plain `let image`/`sizePoints`/`model` — it does
/// NOT read `AppState` via `@Environment` (see that type's own doc comment:
/// there is no documented guarantee `updateNSView` re-runs purely because an
/// `@Observable` dependency read INSIDE it changed). So this view's `body`
/// must read `appState.stripImage`/`stripSizePoints`/`model` itself and pass
/// them down as plain values — SwiftUI's struct-diffing of those stored
/// properties is what reliably drives `updateNSView`.
struct ChatWindow: View {
    @Environment(AppState.self) private var appState
    @Environment(\.openWindow) private var openWindow
    @State private var composeText = ""

    var body: some View {
        @Bindable var state = appState
        NavigationSplitView {
            RoomSidebar()
        } detail: {
            HSplitView {
                VStack(spacing: 0) {
                    // Plan 4b Task 11: the View menu ("Comic Strip view ⌘1" /
                    // "Plain Text view ⌘2", `AppCommands`) swaps this for
                    // `TranscriptTextView` — same event log, two renderings, one
                    // shown at a time (never both).
                    if appState.comicMode {
                        // Comic hit-testing (Plan 4b): a click in the strip
                        // toggles the clicked avatar's nick in the talk-to
                        // selection (`AppState.selectedMembers`), the same
                        // canonical state a member-grid cell tap toggles — so
                        // the grid highlight + ComposeBar addressees update for
                        // free. `onAvatarClick` fires on the main thread
                        // (ChatSessionModel.hitTestNick's completion contract).
                        ComicStripView(image: appState.stripImage,
                                        sizePoints: appState.stripSizePoints,
                                        model: appState.model,
                                        onAvatarClick: { nick in appState.toggleTalkTo(nick) },
                                        onCopyPanel: { index in appState.copyPanelAsImage(index) },
                                        onSavePanel: { index in appState.savePanelAsPNG(index) },
                                        onCopyStrip: { appState.copyStripAsImage() },
                                        canCopyStrip: appState.stripImage != nil)
                    } else {
                        TranscriptTextView(attributedText: appState.transcriptText)
                    }
                    Divider()
                    ComposeBar(composeText: $composeText, model: appState.model,
                              selectedMembers: appState.selectedMembers,
                              soundsFolder: appState.settings.soundsFolder)
                    Text(appState.statusLine)
                        .font(.caption).foregroundStyle(.secondary)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .padding(.horizontal, 8).padding(.bottom, 4)
                }
                // Original layout (chatview.cpp:333-378, the user's screenshot):
                // the member ICON GRID at the top, the large full-body self-pose
                // pane in the middle, the COMPACT emotion wheel in a short pane at
                // the bottom. Parameter-passing contract: this parent body reads
                // `appState.*`; each child takes plain values.
                VStack(spacing: 0) {
                    // (1) Member grid — a 2-column icon grid, scrollable. The grid
                    // SELECTION is still the original's canonical talk-to state
                    // (`AppState.selectedMembers` feeds `ComposeBar`'s send as
                    // `addressees`, Plan 4b Task 8 / D1 §1.5): a cell tap toggles
                    // that nick in the set (visible highlight), preserved from the
                    // List's `selection:` semantics.
                    MemberGrid(members: appState.members)
                    Divider()
                    // (2) The large full-body self-pose pane (head + torso
                    // composited by DrawBody, live-fix 7). Plain `CGImage?` value.
                    PosePreviewPane(poseImage: appState.selfPoseImage)
                        .frame(maxHeight: .infinity)
                        .padding(8)
                    Divider()
                    // (3) The compact emotion wheel, in a short fixed-height pane
                    // (~1/4 the column, matching the original's compact bulls-eye
                    // pane). Wheel-only now — the pose moved up to (2).
                    BodyCamView(onEmotion: { angle, intensity in
                        appState.model?.setEmotion(angle: angle, intensity: intensity)
                    })
                    .frame(height: 140)
                    .padding(8)
                }
                .frame(minWidth: 180, maxWidth: 240)
            }
        }
        .frame(minWidth: 640, minHeight: 480)
        .sheet(isPresented: $state.showConnectSheet) { ConnectSheet() }
        .sheet(isPresented: $state.showCreateRoomSheet) { CreateRoomSheet() }
        .sheet(isPresented: $state.showSetTopicSheet) { SetTopicSheet() }
        .popover(item: userInfoBinding) { info in
            VStack(alignment: .leading, spacing: 8) {
                Text(info.nick).font(.headline)
                Text(info.text).font(.callout).textSelection(.enabled)
            }
            .padding(12)
            .frame(minWidth: 220)
        }
        .task {
            // Offline demo hook (Task 12): `--replay-fixture <path>` should
            // demo the app with zero interaction — skip the connect sheet
            // and connect immediately (`AppState.connect()` reads the flag
            // itself and overrides host/port/nick/room to match the fixture).
            if ProcessInfo.processInfo.arguments.contains("--replay-fixture") {
                appState.showConnectSheet = false
                await appState.connect()
            }
            // Live-fix-7 probe hook: `--debug-pose-probe` drives the wheel's
            // self-pose path end-to-end without UI automation (the screen may
            // be locked). After the replay-fixture connect has JOINED, it
            // drives one full-intensity emotion via the SAME
            // `ChatSessionModel.setEmotion` call the wheel drag makes, then
            // logs `appState.selfPoseImage`'s size via `PoseProbe` (a non-nil
            // size proves the whole model->AppState->view pose chain is live).
            // The size is now the DrawBody composite-canvas size (the head+torso
            // preview, not a raw pose record), so a full-body pose is present.
            // Same "demo-only launch flag, no AX available in this sandbox"
            // posture as `--switch-character`/`--replay-fixture`; inert without
            // the flag.
            if ProcessInfo.processInfo.arguments.contains("--debug-pose-probe") {
                for _ in 0..<200 {
                    if appState.model != nil && !appState.members.isEmpty { break }
                    try? await Task.sleep(for: .milliseconds(50))
                }
                try? await Task.sleep(for: .seconds(2))
                appState.model?.setEmotion(angle: 0, intensity: 1.0)
                try? await Task.sleep(for: .seconds(1))
                let sz = appState.selfPoseImage.map { "\($0.width)x\($0.height)" } ?? "nil"
                PoseProbe.log("selfPoseImage=\(sz) membersCount=\(appState.members.count)")
            }
            // Task 4's visual-artifact demo hook: `--open-whisper <peer>`
            // auto-opens the whisper box for `peer` once a whisper from them
            // has actually arrived (polls `whisperPeers`, bounded by this
            // `.task`'s own lifetime) — same "demo-only launch flag" posture
            // as `--replay-fixture` above, for headless/scripted screenshot
            // capture where driving the member-list context menu via UI
            // automation isn't available.
            if let peerIndex = ProcessInfo.processInfo.arguments.firstIndex(of: "--open-whisper"),
               peerIndex + 1 < ProcessInfo.processInfo.arguments.count {
                let peer = ProcessInfo.processInfo.arguments[peerIndex + 1]
                for _ in 0..<200 {
                    if appState.whisperPeers.contains(peer) { break }
                    try? await Task.sleep(for: .milliseconds(50))
                }
                if appState.whisperPeers.contains(peer) {
                    appState.showWhisperBox(peer: peer)
                    openWindow(id: "whispers")
                }
            }
            // Task 5's visual-artifact demo hook: `--switch-character <name>`
            // waits for the replay-fixture connect to actually be JOINED
            // (`appState.members` non-empty — `model != nil` alone is not
            // sufficient: `AppState.connect()` assigns `model` BEFORE
            // `await m.start()` completes, so polling only for `model != nil`
            // could fire before the strip exists or the fixture's login/join
            // handshake has even finished, same "poll actual state, not just
            // object existence" lesson `--open-whisper`'s own wait already
            // follows). Once joined, drives the SAME
            // `ChatSessionModel.changeCharacter` call the Character picker's
            // `select(_:)` calls, sends one more line so a new (post-switch)
            // panel actually renders, and waits for the strip image to
            // update — again because UI automation (System Events/AX) can't
            // drive the Settings scene's picker clicks headlessly in this
            // sandbox (same limitation the Task 3/4 reports document).
            if let nameIndex = ProcessInfo.processInfo.arguments.firstIndex(of: "--switch-character"),
               nameIndex + 1 < ProcessInfo.processInfo.arguments.count {
                let name = ProcessInfo.processInfo.arguments[nameIndex + 1]
                for _ in 0..<200 {
                    if appState.model != nil && !appState.members.isEmpty { break }
                    try? await Task.sleep(for: .milliseconds(50))
                }
                if let model = appState.model, !appState.members.isEmpty {
                    let panelsBefore = model.panelCount
                    model.changeCharacter(name)
                    try? await model.send("look, a new character")
                    for _ in 0..<200 {
                        if model.panelCount > panelsBefore { break }
                        try? await Task.sleep(for: .milliseconds(50))
                    }
                }
            }
        }
    }

    /// `.popover(item:)`'s binding over `AppState.userInfoResult` — the
    /// popover shows while non-`nil` and clears it on dismiss.
    private var userInfoBinding: Binding<UserInfoItem?> {
        Binding(
            get: { appState.userInfoResult.map { UserInfoItem(nick: $0.nick, text: $0.text) } },
            set: { if $0 == nil { appState.userInfoResult = nil } }
        )
    }
}

/// `Identifiable` wrapper so `AppState.userInfoResult`'s plain tuple can back
/// a `.popover(item:)`.
private struct UserInfoItem: Identifiable {
    var id: String { nick }
    let nick: String
    let text: String
}

/// The member ICON GRID (live-fix 7, the original's right-column top region,
/// chatview.cpp:333-378 / the user's screenshot): a 2-column `LazyVGrid` where
/// each cell is an avatar head thumbnail with an "@nick" caption beneath —
/// replacing the old single-column `List`. Native macOS chrome (no Windows-98
/// styling). Scrollable so a large channel doesn't push the pose pane / wheel
/// off-screen.
///
/// Parameter-passing contract: the parent passes `members` as a plain value;
/// this view reads `AppState` for the cross-cutting state each cell needs
/// (selection, resolved icons, op status, actions) — the same reads the old
/// `List`/`MemberRowView` pair made.
///
/// SELECTION = the original's canonical talk-to state (`AppState.selectedMembers`
/// feeds `ComposeBar`'s send as `addressees`): a tap TOGGLES the cell's nick in
/// the set, exactly the multi-select semantics the `List`'s `selection:`
/// binding had, with a visible highlight. Context menus (Whisper/Get Info/Kick/
/// Ban) are preserved from Task 8.
private struct MemberGrid: View {
    @Environment(AppState.self) private var appState
    @Environment(\.openWindow) private var openWindow
    let members: [MemberRow]

    private let columns = [GridItem(.flexible(), spacing: 8),
                           GridItem(.flexible(), spacing: 8)]

    var body: some View {
        ScrollView {
            LazyVGrid(columns: columns, spacing: 8) {
                ForEach(members) { row in
                    MemberGridCell(row: row,
                                   icon: appState.memberIconCache[row.avatarName],
                                   isSelected: appState.selectedMembers.contains(row.nick))
                        .onTapGesture { toggleSelection(row.nick) }
                        .contextMenu {
                            Button("Whisper…") {
                                appState.showWhisperBox(peer: row.nick)
                                openWindow(id: "whispers")
                            }
                            Button("Get Info…") { appState.getInfo(row.nick) }
                            Divider()
                            // Quick-wins batch item 5 (original per-member
                            // ignore, CUserInfo m_bIgnored): toggles the
                            // engine-queue-owned `ignoredNicks` set — the
                            // model itself handles the live reflow (both apply
                            // sites, per that property's doctrine comment);
                            // this menu item only flips the flag.
                            Button(row.isIgnored ? "Unignore" : "Ignore") {
                                appState.model?.setIgnored(row.nick, !row.isIgnored)
                            }
                            Divider()
                            Button("Kick…") { appState.kick(row.nick) }
                                .disabled(!appState.selfIsOp)
                            Button("Ban…") { appState.ban("\(row.nick)!*@*") }
                                .disabled(!appState.selfIsOp)
                        }
                        .onAppear { appState.resolveMemberIcon(row.avatarName) }
                }
            }
            .padding(8)
        }
        .frame(maxWidth: .infinity)
    }

    /// Toggle `nick` in the multi-select set (the `List` selection semantics
    /// this grid replaces): tapping a selected member deselects it.
    private func toggleSelection(_ nick: String) {
        if appState.selectedMembers.contains(nick) {
            appState.selectedMembers.remove(nick)
        } else {
            appState.selectedMembers.insert(nick)
        }
    }
}

/// One member grid cell (live-fix 7): an avatar head thumbnail above an
/// "@nick" caption, with the op star preserved and a selection highlight.
/// Pure plain-value inputs (`row`, resolved `icon`, `isSelected`) — no
/// `@Environment` read, so it re-renders purely on those values changing
/// (parameter-passing contract).
///
/// A name that still doesn't resolve to real art (empty `avatarName`, or one
/// `resolveMemberIcon` failed to decode — Tim's earlier gray-placeholder
/// report) renders NO icon, just a blank thumbnail slot above the caption, so
/// cells stay aligned.
private struct MemberGridCell: View {
    let row: MemberRow
    let icon: CGImage?
    let isSelected: Bool

    var body: some View {
        VStack(spacing: 3) {
            ZStack {
                if let icon {
                    Image(decorative: icon, scale: 1)
                        .resizable()
                        .aspectRatio(contentMode: .fit)
                        .frame(width: 40, height: 40)
                        .clipShape(RoundedRectangle(cornerRadius: 4))
                } else {
                    Color.clear.frame(width: 40, height: 40)
                }
                if row.isOp {
                    Image(systemName: "star.fill")
                        .font(.caption2)
                        .foregroundStyle(.yellow)
                        .help("Operator")
                        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topTrailing)
                        .frame(width: 40, height: 40)
                }
                // Quick-wins batch item 4 (Batch B clear-fix): away badge
                // (mirrors `ProtocolSession.RoomMember.isAway`, set AND
                // cleared from an inbound `.awayPeer` event — see
                // `MemberRow.isAway`'s doc comment for the away-return
                // finding). Display-only.
                if row.isAway {
                    Image(systemName: "moon.zzz.fill")
                        .font(.caption2)
                        .foregroundStyle(.secondary)
                        .help("Away")
                        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .bottomTrailing)
                        .frame(width: 40, height: 40)
                }
                // Quick-wins batch item 5 (original CUserInfo m_bIgnored): a
                // slashed-eye badge on ignored cells — display-only; the
                // actual filtering happens engine-side.
                if row.isIgnored {
                    Image(systemName: "eye.slash.fill")
                        .font(.caption2)
                        .foregroundStyle(.secondary)
                        .help("Ignored")
                        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .bottomLeading)
                        .frame(width: 40, height: 40)
                }
            }
            Text("@\(row.nick)")
                .font(.caption)
                .foregroundStyle(.secondary)
                .lineLimit(1)
                .truncationMode(.tail)
        }
        .frame(maxWidth: .infinity)
        .padding(4)
        .background(
            RoundedRectangle(cornerRadius: 6)
                .fill(isSelected ? Color.accentColor.opacity(0.25) : Color.clear)
        )
        .contentShape(Rectangle())
    }
}

/// The rooms SIDEBAR (quick-wins batch item 1, replacing the retired
/// `RoomTabBar`): a native `List` of joined rooms — SF Symbol `number` +
/// the room's display name + the topic (if any) as a secondary, truncated
/// subtitle line, with an unread-count `.badge()`. Selection is bound to the
/// active room (`AppState.activeRoom`/`setActiveRoom`, the exact same
/// chokepoint the old tab tap drove — `handleRoomsChanged`'s
/// selection-clear posture is entirely unaffected by this view swap). A
/// context menu per row offers "Leave Room"; a toolbar "+" button opens the
/// existing Enter Room… sheet (the tab bar's "+" twin, unchanged sheet type).
///
/// Always renders (even for exactly one room, e.g. the `--replay-fixture`
/// path) — matching the retired tab bar's own "always renders the single
/// tab" posture, so the sidebar stays a consistent affordance rather than
/// popping in/out as rooms are joined/left.
struct RoomSidebar: View {
    @Environment(AppState.self) private var appState

    var body: some View {
        @Bindable var state = appState
        List(appState.rooms, selection: activeRoomBinding) { room in
            VStack(alignment: .leading, spacing: 2) {
                Label(room.name, systemImage: "number")
                if !room.topic.isEmpty {
                    Text(room.topic)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .lineLimit(1)
                        .truncationMode(.tail)
                }
            }
            .badge(room.unread)
            .tag(room.name)
            .contextMenu {
                Button("Leave Room") { appState.leaveRoom(room.name) }
            }
        }
        .navigationSplitViewColumnWidth(min: 160, ideal: 200)
        // Auto-reconnect (spec §7): a non-modal indicator pinned above the room
        // list while a reconnect backoff loop is running or after it gave up.
        // The per-attempt human-readable countdown line lives in the status
        // line (`onStatus`); this is just the persistent at-a-glance badge.
        .safeAreaInset(edge: .top) {
            if appState.isReconnecting || appState.reconnectGaveUp {
                ReconnectBanner()
            }
        }
        .toolbar {
            ToolbarItem {
                Button {
                    appState.enterRoomText = ""
                    appState.showEnterRoomSheet = true
                } label: {
                    Image(systemName: "plus")
                }
                .help("Enter Room… (⌘J)")
            }
        }
        .sheet(isPresented: $state.showEnterRoomSheet) {
            EnterRoomSheet()
        }
    }

    /// A `Binding<String?>` over the active room's NAME, driving
    /// `AppState.setActiveRoom` on selection — the sidebar's twin of the
    /// retired tab bar's `onSelect: { appState.setActiveRoom(room.name) }`.
    /// Setting `nil` (a `List` selection can clear, e.g. via ⌘-click-to-
    /// deselect) is a no-op: there is always exactly one active room while
    /// any room is joined, and `setActiveRoom` has no "deactivate" mode of
    /// its own to route a `nil` to.
    private var activeRoomBinding: Binding<String?> {
        Binding(
            get: { appState.activeRoom },
            set: { newValue in
                guard let newValue else { return }
                appState.setActiveRoom(newValue)
            }
        )
    }
}

/// Auto-reconnect (spec §7): the non-modal sidebar indicator. A spinner +
/// "Reconnecting…" while a backoff loop runs; a warning glyph + "Couldn't
/// reconnect" (with a Reconnect button that reopens the connect sheet) once it
/// gives up. Shown only when `isReconnecting`/`reconnectGaveUp` (the caller
/// gates it), so the `.connected` steady state renders nothing.
struct ReconnectBanner: View {
    @Environment(AppState.self) private var appState

    var body: some View {
        HStack(spacing: 6) {
            if appState.reconnectGaveUp {
                Image(systemName: "exclamationmark.triangle.fill")
                    .foregroundStyle(.orange)
                Text("Couldn't reconnect")
                    .font(.caption)
                Spacer(minLength: 0)
                Button("Reconnect") { appState.showConnectSheet = true }
                    .controlSize(.small)
            } else {
                ProgressView()
                    .controlSize(.small)
                Text("Reconnecting…")
                    .font(.caption)
                Spacer(minLength: 0)
            }
        }
        .padding(.horizontal, 10)
        .padding(.vertical, 6)
        .background(.thinMaterial)
    }
}

/// The Enter Room sheet (Plan 4b Task 7, moved here from the retired
/// `RoomTabBar.swift` — quick-wins batch item 1): a single channel-name field
/// that joins an ADDITIONAL room on the current connection. Shared by
/// `RoomSidebar`'s "+" toolbar button and the Room > Enter Room… ⌘J command
/// (both flip `AppState.showEnterRoomSheet`).
struct EnterRoomSheet: View {
    @Environment(AppState.self) private var appState
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        @Bindable var state = appState
        VStack(alignment: .leading, spacing: 12) {
            Text("Enter Room")
                .font(.headline)
            TextField("Channel (e.g. #comics)", text: $state.enterRoomText)
                .textFieldStyle(.roundedBorder)
                .frame(minWidth: 240)
                .onSubmit(join)
            HStack {
                Spacer()
                Button("Cancel") { dismiss() }
                    .keyboardShortcut(.cancelAction)
                Button("Join") { join() }
                    .keyboardShortcut(.defaultAction)
                    .disabled(appState.enterRoomText.trimmingCharacters(in: .whitespaces).isEmpty)
            }
        }
        .padding(20)
    }

    private func join() {
        let text = appState.enterRoomText
        guard !text.trimmingCharacters(in: .whitespaces).isEmpty else { return }
        appState.joinRoom(text)
        dismiss()
    }
}

/// The Create Room… sheet (Plan 4b Task 8, Room menu): a single channel-name
/// field that creates (and goes to) a new room. Mirrors `EnterRoomSheet`'s
/// shape (moved here from the retired `RoomTabBar.swift`).
struct CreateRoomSheet: View {
    @Environment(AppState.self) private var appState
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        @Bindable var state = appState
        VStack(alignment: .leading, spacing: 12) {
            Text("Create Room")
                .font(.headline)
            TextField("Channel (e.g. #comics)", text: $state.createRoomText)
                .textFieldStyle(.roundedBorder)
                .frame(minWidth: 240)
                .onSubmit(create)
            HStack {
                Spacer()
                Button("Cancel") { dismiss() }
                    .keyboardShortcut(.cancelAction)
                Button("Create") { create() }
                    .keyboardShortcut(.defaultAction)
                    .disabled(appState.createRoomText.trimmingCharacters(in: .whitespaces).isEmpty)
            }
        }
        .padding(20)
    }

    private func create() {
        let text = appState.createRoomText
        guard !text.trimmingCharacters(in: .whitespaces).isEmpty else { return }
        appState.createRoom(text)
        dismiss()
    }
}

/// The Set Topic… sheet (quick-wins batch item 6, Room menu): an
/// alert-with-textfield over the ACTIVE room's topic — the createRoom sheet's
/// same shape (a single text field + Cancel/action buttons), seeded from the
/// room's CURRENT topic (`AppCommands`' call site sets `setTopicText` before
/// presenting) rather than starting blank, since editing an existing topic is
/// the common case.
struct SetTopicSheet: View {
    @Environment(AppState.self) private var appState
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        @Bindable var state = appState
        VStack(alignment: .leading, spacing: 12) {
            Text("Set Topic")
                .font(.headline)
            TextField("Topic", text: $state.setTopicText)
                .textFieldStyle(.roundedBorder)
                .frame(minWidth: 280)
                .onSubmit(setTopic)
            HStack {
                Spacer()
                Button("Cancel") { dismiss() }
                    .keyboardShortcut(.cancelAction)
                Button("Set") { setTopic() }
                    .keyboardShortcut(.defaultAction)
            }
        }
        .padding(20)
    }

    private func setTopic() {
        appState.setTopic(appState.setTopicText)
        dismiss()
    }
}
