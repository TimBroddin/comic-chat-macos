import SwiftUI
import ComicChatKit

/// The initial connect sheet: server/port/nickname/room + encoding, bound
/// directly to `AppState.settings` (a `SettingsStore`, itself a thin
/// `UserDefaults` wrapper — every field edit writes straight through, no
/// separate "save" step). `Connect` calls `appState.connect()`, which builds
/// a `ChatConfig` from the current settings and starts a `ChatSessionModel`.
struct ConnectSheet: View {
    @Environment(AppState.self) private var appState
    @Environment(\.dismiss) private var dismiss
    @State private var isConnecting = false

    var body: some View {
        @Bindable var appState = appState

        Form {
            Section("Server") {
                TextField("Server", text: settingsBinding(\.server))
                TextField("Port", value: settingsBinding(\.port), format: .number.grouping(.never))
                Picker("Encoding", selection: settingsBinding(\.encoding)) {
                    Text("Windows-1252").tag(WireEncoding.cp1252)
                    Text("UTF-8").tag(WireEncoding.utf8)
                }
            }

            Section("Identity") {
                TextField("Nickname", text: settingsBinding(\.nick))
                TextField("Room", text: settingsBinding(\.room))
            }

            if !appState.statusLine.isEmpty {
                Text(appState.statusLine)
                    .foregroundStyle(.secondary)
            }
        }
        .padding()
        .frame(minWidth: 360, minHeight: 260)
        .toolbar {
            ToolbarItem(placement: .cancellationAction) {
                Button("Cancel") { dismiss() }
            }
            ToolbarItem(placement: .confirmationAction) {
                Button("Connect") {
                    isConnecting = true
                    Task {
                        await appState.connect()
                        isConnecting = false
                    }
                }
                .disabled(isConnecting || appState.settings.server.isEmpty
                          || appState.settings.nick.isEmpty || appState.settings.room.isEmpty
                          || !(1...65535).contains(appState.settings.port))
            }
        }
    }

    /// Write-through computed `Binding` over one `SettingsStore` property —
    /// every edit persists immediately via the store's `nonmutating set`
    /// (no local `@State` copy to keep in sync).
    private func settingsBinding<T>(_ keyPath: WritableKeyPath<SettingsStore, T>) -> Binding<T> {
        Binding(
            get: { appState.settings[keyPath: keyPath] },
            set: { appState.settings[keyPath: keyPath] = $0 }
        )
    }
}
