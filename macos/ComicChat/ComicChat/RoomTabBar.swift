import SwiftUI
import ComicChatKit

/// The MDI room tab bar (Plan 4b Task 7: true multi-room — the original's
/// `tabbar.cpp`, reborn). A horizontal strip of tabs, one per joined room:
/// room name + an unread badge + a close button; plus a trailing "+" that
/// opens the Enter Room sheet (⌘J's UI twin). Tapping a tab switches the ONE
/// live strip to that room (`AppState.setActiveRoom` -> the model's
/// destroy-then-rebuild-from-transcript swap); the member sidebar, compose
/// bar, and strip all follow the active tab.
///
/// Shows nothing when only one room is joined? No — it always renders the
/// single tab (the replay-fixture path shows exactly one), so the bar is a
/// consistent affordance and the "+" is always reachable.
struct RoomTabBar: View {
    @Environment(AppState.self) private var appState

    var body: some View {
        @Bindable var state = appState
        ScrollView(.horizontal, showsIndicators: false) {
            HStack(spacing: 4) {
                ForEach(appState.rooms) { room in
                    RoomTab(room: room,
                            canClose: appState.rooms.count > 1,
                            onSelect: { appState.setActiveRoom(room.name) },
                            onClose: { appState.leaveRoom(room.name) })
                }
                Button {
                    appState.enterRoomText = ""
                    appState.showEnterRoomSheet = true
                } label: {
                    Image(systemName: "plus")
                        .frame(width: 22, height: 22)
                }
                .buttonStyle(.borderless)
                .help("Enter Room… (⌘J)")
            }
            .padding(.horizontal, 6)
            .padding(.vertical, 4)
        }
        .frame(height: 30)
        .sheet(isPresented: $state.showEnterRoomSheet) {
            EnterRoomSheet()
        }
    }
}

/// One tab: room name, unread badge, and (when more than one room is joined) a
/// close button. The active tab is visually distinguished.
private struct RoomTab: View {
    let room: RoomInfo
    let canClose: Bool
    let onSelect: () -> Void
    let onClose: () -> Void

    var body: some View {
        HStack(spacing: 4) {
            Button(action: onSelect) {
                HStack(spacing: 4) {
                    Text(room.name)
                        .fontWeight(room.isActive ? .semibold : .regular)
                        .lineLimit(1)
                    if room.unread > 0 {
                        Text("\(room.unread)")
                            .font(.caption2)
                            .padding(.horizontal, 5)
                            .padding(.vertical, 1)
                            .background(Capsule().fill(Color.accentColor))
                            .foregroundStyle(.white)
                    }
                }
            }
            .buttonStyle(.plain)

            if canClose {
                Button(action: onClose) {
                    Image(systemName: "xmark")
                        .font(.system(size: 8, weight: .bold))
                        .foregroundStyle(.secondary)
                }
                .buttonStyle(.borderless)
                .help("Leave \(room.name)")
            }
        }
        .padding(.horizontal, 8)
        .padding(.vertical, 3)
        .background(
            RoundedRectangle(cornerRadius: 6)
                .fill(room.isActive ? Color.accentColor.opacity(0.18) : Color.secondary.opacity(0.08))
        )
        .overlay(
            RoundedRectangle(cornerRadius: 6)
                .strokeBorder(room.isActive ? Color.accentColor.opacity(0.5) : Color.clear, lineWidth: 1)
        )
    }
}

/// The Enter Room sheet (Plan 4b Task 7): a single channel-name field that
/// joins an ADDITIONAL room on the current connection. Shared by the tab
/// bar's "+" and the Room > Enter Room… ⌘J command (both flip
/// `AppState.showEnterRoomSheet`).
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
