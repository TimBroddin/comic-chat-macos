import SwiftUI
import ComicChatKit

/// The whisper box (Plan 4b Task 4, D1 §0 correction to spec §5): ONE
/// floating window with a tab per correspondent, mirroring the original's
/// `CWhisperBox` + `CWhisperLeaf` (whisprbx.cpp:35-72) rather than spec §5's
/// "separate small windows". Rendering decision (constrained by
/// ONE-STRIP-AT-A-TIME): whisper leaves render as a TEXT transcript here — a
/// second live comic strip per whisper peer would need a second concurrent
/// `cc_strip`, which comicchat.h's single-strip contract forbids
/// (use-after-free, not merely a race). Room-scoped whispers ALSO keep
/// rendering as whisper balloons in the MAIN strip (existing 4a behavior,
/// `ChatSessionModel.handleLocked`'s `.whisper` case keeps its
/// `bridge.apply + recomposeLocked` unconditionally) — this box is an
/// ADDITIONAL, not a replacement, view onto whisper traffic.
///
/// MODERN-MAC DEVIATION (noted per the Task 4 report): the original
/// auto-pops-up a `CWhisperLeaf` the moment an inbound whisper arrives from a
/// peer with no open leaf. This window does NOT auto-open — an inbound
/// whisper for a peer with no open box instead surfaces via `onStatus` (a
/// status-line message) plus `AppState.whisperUnread`'s badge; the user
/// opens the box explicitly (member context menu "Whisper…", or the Member
/// menu command), matching how modern Mac apps treat incidental
/// notification windows.
struct WhisperBox: View {
    @Environment(AppState.self) private var appState
    @State private var selectedPeer: String?
    @State private var composeText = ""

    var body: some View {
        @Bindable var state = appState
        NavigationSplitView {
            List(appState.whisperPeers, id: \.self, selection: $selectedPeer) { peer in
                HStack {
                    Text(peer)
                    Spacer()
                    if let unread = appState.whisperUnread[peer], unread > 0 {
                        Text("\(unread)")
                            .font(.caption2).foregroundStyle(.white)
                            .padding(.horizontal, 6).padding(.vertical, 2)
                            .background(.red, in: Capsule())
                    }
                }
                .tag(peer)
            }
            .navigationSplitViewColumnWidth(min: 120, ideal: 160)
        } detail: {
            if let peer = selectedPeer {
                whisperDetail(for: peer)
            } else {
                ContentUnavailableView("No Whisper Selected", systemImage: "bubble.left.and.bubble.right")
            }
        }
        .frame(minWidth: 480, minHeight: 320)
        .onChange(of: selectedPeer) { _, newValue in
            // Clear the badge for whichever tab just became active (existing
            // history read, not a network round-trip — same instant clear
            // the original's leaf-focus behavior implies).
            if let newValue { appState.whisperUnread[newValue] = 0 }
        }
        .task {
            // Window-open hand-off: `showWhisperBox(peer:)` stashes the
            // requested peer on `AppState` (there is no per-open parameter on
            // a `Window` scene, only its fixed `id`) — select it once this
            // view appears, then clear the pending slot so a later
            // window-close/reopen with no explicit peer doesn't re-select it.
            if let pending = appState.pendingWhisperPeer {
                selectedPeer = pending
                appState.pendingWhisperPeer = nil
                appState.whisperUnread[pending] = 0
            } else if selectedPeer == nil {
                selectedPeer = appState.whisperPeers.first
            }
        }
    }

    @ViewBuilder
    private func whisperDetail(for peer: String) -> some View {
        VStack(spacing: 0) {
            ScrollViewReader { proxy in
                ScrollView {
                    LazyVStack(alignment: .leading, spacing: 6) {
                        ForEach(Array((appState.whisperHistories[peer] ?? []).enumerated()), id: \.offset) { idx, line in
                            whisperLineView(line)
                                .id(idx)
                        }
                    }
                    .padding(8)
                }
                .onChange(of: appState.whisperHistories[peer]?.count) { _, _ in
                    if let lastIndex = appState.whisperHistories[peer]?.indices.last {
                        proxy.scrollTo(lastIndex, anchor: .bottom)
                    }
                }
            }
            Divider()
            HStack {
                TextField("Whisper to \(peer)…", text: $composeText)
                    .textFieldStyle(.roundedBorder)
                    .onSubmit { send(to: peer) }
                Button("Send") { send(to: peer) }
                    .keyboardShortcut(.defaultAction)
            }
            .padding(8)
        }
        .navigationTitle(peer)
    }

    @ViewBuilder
    private func whisperLineView(_ line: WhisperLine) -> some View {
        HStack {
            if line.isOwn { Spacer(minLength: 24) }
            VStack(alignment: line.isOwn ? .trailing : .leading, spacing: 2) {
                Text(line.nick).font(.caption2).foregroundStyle(.secondary)
                Text(line.text)
                    .padding(.horizontal, 10).padding(.vertical, 6)
                    .background(line.isOwn ? Color.accentColor.opacity(0.2) : Color.secondary.opacity(0.15),
                                in: RoundedRectangle(cornerRadius: 10))
            }
            if !line.isOwn { Spacer(minLength: 24) }
        }
        .frame(maxWidth: .infinity, alignment: line.isOwn ? .trailing : .leading)
    }

    private func send(to peer: String) {
        let text = composeText.trimmingCharacters(in: .whitespaces)
        guard !text.isEmpty, let model = appState.model else { return }
        composeText = ""
        Task { try? await model.sendWhisper(to: peer, text: text) }
    }
}
