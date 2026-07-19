import SwiftUI
import AppKit
import ComicChatKit

/// The initial connect sheet: server/port/nickname/room + encoding, bound
/// directly to `AppState.settings` (a `SettingsStore`, itself a thin
/// `UserDefaults` wrapper — every field edit writes straight through, no
/// separate "save" step). `Connect` calls `appState.connect()`, which builds
/// a `ChatConfig` from the current settings and starts a `ChatSessionModel`.
///
/// UI-improvement pass ("prettier connect sheet with known Comic Chat
/// servers"): adds a `KnownServers` picker above the free-text fields. The
/// text fields remain the SOURCE OF TRUTH — same write-through
/// `settingsBinding` pattern as before, unchanged. The picker is a
/// fill-in convenience layered on top: selecting a row copies that
/// server's host/port/room into the settings fields; typing a custom
/// host that doesn't match any known entry's host+port simply falls back
/// to the "Custom Server" selection state (computed, never fought).
struct ConnectSheet: View {
    @Environment(AppState.self) private var appState
    @Environment(\.dismiss) private var dismiss
    @State private var isConnecting = false

    /// A sentinel selection id for "Custom Server" in the picker list,
    /// distinct from any `KnownServer.id` (which is always `host:port`).
    private static let customSelectionID = "custom"

    var body: some View {
        @Bindable var appState = appState

        Form {
            header

            Section("Server") {
                // `serverPicker`'s selection is DERIVED from `settings.server`/
                // `settings.port` (see `selectionBinding`'s getter) — typing a
                // custom host here that no longer matches the previously
                // selected known entry's host+port automatically re-derives
                // to "Custom Server" on the next body evaluation, with no
                // separate `.onChange` needed. The fields stay authoritative;
                // the picker just follows along (brief: "don't fight the user").
                serverPicker
                TextField("Server", text: settingsBinding(\.server))
            }

            Section("Identity") {
                TextField("Nickname", text: settingsBinding(\.nick))
                    .textContentType(.nickname)
                TextField("Room", text: settingsBinding(\.room))
            }

            DisclosureGroup("Details") {
                TextField("Port", value: settingsBinding(\.port), format: .number.grouping(.never))
                Picker("Encoding", selection: settingsBinding(\.encoding)) {
                    Text("Windows-1252").tag(WireEncoding.cp1252)
                    Text("UTF-8").tag(WireEncoding.utf8)
                }
            }

            if isConnecting {
                HStack {
                    ProgressView()
                        .controlSize(.small)
                    Text("Connecting…")
                        .foregroundStyle(.secondary)
                }
            } else if !appState.statusLine.isEmpty {
                Text(appState.statusLine)
                    .foregroundStyle(.secondary)
            }
        }
        .formStyle(.grouped)
        .disabled(isConnecting)
        .frame(minWidth: 420, minHeight: 460)
        .toolbar {
            ToolbarItem(placement: .cancellationAction) {
                Button("Cancel") { dismiss() }
                    .disabled(isConnecting)
            }
            ToolbarItem(placement: .confirmationAction) {
                Button("Connect") {
                    isConnecting = true
                    Task {
                        await appState.connect()
                        isConnecting = false
                    }
                }
                .keyboardShortcut(.defaultAction)
                .disabled(isConnecting || appState.settings.server.isEmpty
                          || appState.settings.nick.isEmpty || appState.settings.room.isEmpty
                          || !(1...65535).contains(appState.settings.port))
            }
        }
    }

    /// App icon + title, matching macOS HIG's convention for a sheet that
    /// introduces a single clear action (here: connecting to a server).
    private var header: some View {
        HStack(spacing: 12) {
            Image(nsImage: NSApp.applicationIconImage)
                .resizable()
                .frame(width: 40, height: 40)
            VStack(alignment: .leading, spacing: 2) {
                Text("Connect to a Chat Server")
                    .font(.title3)
                    .fontWeight(.semibold)
                Text("Pick a known server or enter your own.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        }
        .padding(.bottom, 4)
        .listRowInsets(EdgeInsets())
        .listRowBackground(Color.clear)
    }

    /// The known-servers list, plus a trailing "Custom Server" row. Selecting
    /// a known row copies host/port/room straight into `settings` (the same
    /// write-through path the text fields use); selecting "Custom Server"
    /// changes nothing (the fields already hold whatever was last typed).
    private var serverPicker: some View {
        Picker("Known Servers", selection: selectionBinding) {
            ForEach(KnownServers.all) { server in
                serverRow(server).tag(server.id)
            }
            customRow.tag(Self.customSelectionID)
        }
        .pickerStyle(.inline)
        .labelsHidden()
    }

    private func serverRow(_ server: KnownServer) -> some View {
        VStack(alignment: .leading, spacing: 3) {
            HStack {
                Text(server.name)
                    .font(.headline)
                Spacer()
                protocolBadge(server.protocolNote)
            }
            Text("\(server.host):\(server.port)")
                .font(.caption)
                .foregroundStyle(.secondary)
            Text(server.blurb)
                .font(.caption)
                .foregroundStyle(.secondary)
        }
        .padding(.vertical, 2)
    }

    private var customRow: some View {
        HStack {
            Image(systemName: "network")
            VStack(alignment: .leading, spacing: 2) {
                Text("Custom Server")
                    .font(.headline)
                Text("Enter your own host and room below.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        }
        .padding(.vertical, 2)
    }

    private func protocolBadge(_ note: String) -> some View {
        Text(note)
            .font(.caption2.weight(.medium))
            .padding(.horizontal, 6)
            .padding(.vertical, 2)
            .background(Capsule().fill(Color.secondary.opacity(0.15)))
            .foregroundStyle(.secondary)
    }

    /// Derives the picker's current selection from the settings fields
    /// (never the other way around, except when the user actively picks a
    /// row): a known entry if server+port exactly match one, else "Custom
    /// Server". Selecting a row writes server/port/room through to
    /// `settings`; selecting "Custom Server" is a no-op on the fields — it
    /// only exists to be reachable/highlightable when nothing else matches.
    private var selectionBinding: Binding<String> {
        Binding(
            get: {
                KnownServers.match(host: appState.settings.server, port: appState.settings.port)?.id
                    ?? Self.customSelectionID
            },
            set: { newID in
                guard newID != Self.customSelectionID,
                      let server = KnownServers.all.first(where: { $0.id == newID }) else { return }
                appState.settings.server = server.host
                appState.settings.port = server.port
                appState.settings.room = server.room
            }
        )
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
