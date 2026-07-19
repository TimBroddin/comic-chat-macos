import SwiftUI
import ComicChatKit

/// The MVP chat window (Plan 4a Task 12, the plan's exit-milestone task):
/// comic strip + compose bar + status line on the left, member sidebar on
/// the right. Replaces `ContentRoot` as the app's root view.
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
        HSplitView {
            VStack(spacing: 0) {
                // Plan 4b Task 7: the room tab bar sits above the one live
                // strip — tabs with unread badges + close, a "+" to enter a
                // room. Only shown once connected (there's at least one room).
                if !appState.rooms.isEmpty {
                    RoomTabBar()
                    Divider()
                }
                // Plan 4b Task 11: the View menu ("Comic Strip view ⌘1" /
                // "Plain Text view ⌘2", `AppCommands`) swaps this for
                // `TranscriptTextView` — same event log, two renderings, one
                // shown at a time (never both).
                if appState.comicMode {
                    ComicStripView(image: appState.stripImage,
                                    sizePoints: appState.stripSizePoints,
                                    model: appState.model)
                } else {
                    TranscriptTextView(attributedText: appState.transcriptText)
                }
                Divider()
                ComposeBar(composeText: $composeText, model: appState.model,
                          selectedMembers: appState.selectedMembers)
                Text(appState.statusLine)
                    .font(.caption).foregroundStyle(.secondary)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(.horizontal, 8).padding(.bottom, 4)
            }
            // Original layout (chatview.cpp:333-378): members over bodycam.
            VStack(spacing: 0) {
                // Plan 4b Task 8 (D1 §1.5): the List's SELECTION is the
                // original's canonical talk-to state — `AppState.selectedMembers`
                // feeds `ComposeBar`'s send as `addressees`.
                List(appState.members, selection: $state.selectedMembers) { row in
                    MemberRowView(row: row)
                        .tag(row.nick)
                        .contextMenu {
                            Button("Whisper…") {
                                appState.showWhisperBox(peer: row.nick)
                                openWindow(id: "whispers")
                            }
                            Button("Get Info…") { appState.getInfo(row.nick) }
                            Divider()
                            Button("Kick…") { appState.kick(row.nick) }
                                .disabled(!appState.selfIsOp)
                            Button("Ban…") { appState.ban("\(row.nick)!*@*") }
                                .disabled(!appState.selfIsOp)
                        }
                        .onAppear { appState.resolveMemberIcon(row.avatarName) }
                }
                Divider()
                BodyCamView(poseImage: appState.selfPoseImage,
                            onEmotion: { angle, intensity in
                    appState.model?.setEmotion(angle: angle, intensity: intensity)
                })
            }
            .frame(minWidth: 140, maxWidth: 220)
        }
        .frame(minWidth: 640, minHeight: 480)
        .sheet(isPresented: $state.showConnectSheet) { ConnectSheet() }
        .sheet(isPresented: $state.showCreateRoomSheet) { CreateRoomSheet() }
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

/// One member-list row (Plan 4b Task 8): an icon thumbnail (resolved via
/// `AppState.resolveMemberIcon`/`memberIconCache`; unresolved names show no
/// icon), the nick, and an op badge for room owners/ops.
private struct MemberRowView: View {
    @Environment(AppState.self) private var appState
    let row: MemberRow

    var body: some View {
        HStack(spacing: 6) {
            if let icon = appState.memberIconCache[row.avatarName] {
                Image(decorative: icon, scale: 1)
                    .resizable()
                    .frame(width: 20, height: 20)
                    .clipShape(RoundedRectangle(cornerRadius: 3))
            } else {
                RoundedRectangle(cornerRadius: 3)
                    .fill(Color.secondary.opacity(0.15))
                    .frame(width: 20, height: 20)
            }
            Text(row.nick)
            if row.isOp {
                Image(systemName: "star.fill")
                    .font(.caption2)
                    .foregroundStyle(.yellow)
                    .help("Operator")
            }
        }
    }
}

/// The Create Room… sheet (Plan 4b Task 8, Room menu): a single channel-name
/// field that creates (and goes to) a new room. Mirrors `EnterRoomSheet`'s
/// shape (`RoomTabBar.swift`).
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
