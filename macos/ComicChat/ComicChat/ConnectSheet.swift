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
/// host that doesn't match any known entry's host+port falls back to the
/// "Custom Server" selection state.
///
/// Live-fix 7 (server picker was INERT in 34c54a8 — tapping a row never
/// changed the highlight or the fields): the server list is now EXPLICIT
/// `Button` rows, each of which, on tap, writes its server's host/port/room
/// straight through the settings write-through path AND sets a plain `@State`
/// visual-selection id (`selectedServerID`). Obvious-by-construction: the tap's
/// effect is the button's own action, not a `Picker` selection binding SwiftUI
/// has to infer and re-read.
///
/// Why the shipped `Picker(.inline)` failed: its `selection` was a `Binding`
/// DERIVED from `settings.server`/`settings.port` (a getter that re-read the
/// fields). But `SettingsStore` is a plain struct with `nonmutating set`
/// accessors that write straight to `UserDefaults` and fire NO change
/// notification, and it is a stored property of the `@Observable AppState` — so
/// writing `appState.settings.server = …` never invalidates any `@Observable`
/// dependency (the struct itself is not reassigned). SwiftUI therefore had no
/// reason to re-evaluate `body`, the derived getter was never re-invoked, and
/// the inline Picker's highlight — which only moves when `body` re-reads its
/// `selection` — stayed put. (The free-text `TextField`s "worked" only because
/// a `TextField` echoes local keystrokes itself, independent of `@Observable`
/// invalidation.)
///
/// The `@State selectedServerID` is the ONLY thing driving the visual selection
/// now, and SwiftUI tracks it natively: a Button tap sets it (instant
/// re-render); a custom host/port edit re-derives it FROM the fields via
/// `serverFieldBinding`/`portFieldBinding` (so typing a host matching no known
/// entry visibly flips the highlight to "Custom Server", and typing one that
/// matches re-highlights that row). The text fields stay authoritative for
/// custom input; the buttons are a fill-in convenience over them.
struct ConnectSheet: View {
    @Environment(AppState.self) private var appState
    @Environment(\.dismiss) private var dismiss
    @State private var isConnecting = false

    /// The visual selection — real SwiftUI `@State` (see the type doc comment
    /// for why a `Picker` selection binding over `settings` could not work).
    /// Seeded from the current settings on appear; set directly by a row
    /// Button tap; re-derived from the fields on a custom edit.
    @State private var selectedServerID = ConnectSheet.customSelectionID

    /// A sentinel selection id for "Custom Server" in the picker list,
    /// distinct from any `KnownServer.id` (which is always `host:port`).
    private static let customSelectionID = "custom"

    var body: some View {
        @Bindable var appState = appState

        Form {
            header

            Section("Server") {
                // Explicit Button rows: each writes its server's host/port/room
                // through to `settings` AND sets `selectedServerID` (the
                // highlight) in its own action — no Picker selection inference.
                // Typing a custom host in the field re-derives the selection to
                // "Custom Server" via `serverFieldBinding`.
                serverPicker
                TextField("Server", text: serverFieldBinding)
            }

            Section("Identity") {
                TextField("Nickname", text: settingsBinding(\.nick))
                    .textContentType(.nickname)
                TextField("Room", text: settingsBinding(\.room))
            }

            DisclosureGroup("Details") {
                TextField("Port", value: portFieldBinding, format: .number.grouping(.never))
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
        // Seed the visual selection from whatever the persisted settings
        // already hold (a relaunch restores the last host/port), so the
        // matching known row — or "Custom Server" — is highlighted from the
        // first render. (The row Buttons write settings on tap; the write-
        // through no longer lives in an `.onChange`.)
        .onAppear { selectedServerID = derivedSelectionID() }
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

    /// The known-servers list (explicit Button rows), plus a trailing "Custom
    /// Server" row. Tapping a known row writes its host/port/room straight into
    /// `settings` (the same write-through path the text fields use) and marks
    /// it selected; tapping "Custom Server" changes no settings (the fields
    /// already hold whatever was last typed), it just moves the highlight.
    private var serverPicker: some View {
        VStack(spacing: 0) {
            ForEach(KnownServers.all) { server in
                serverRow(server)
                if server.id != KnownServers.all.last?.id {
                    Divider()
                }
            }
            Divider()
            customRow
        }
    }

    /// A known-server row as a Button: on tap it writes host/port/room through
    /// to `settings` and sets `selectedServerID` (both effects in this one
    /// action — obvious-by-construction).
    private func serverRow(_ server: KnownServer) -> some View {
        Button {
            appState.settings.server = server.host
            appState.settings.port = server.port
            appState.settings.room = server.room
            selectedServerID = server.id
        } label: {
            HStack(spacing: 8) {
                selectionMark(isSelected: selectedServerID == server.id)
                VStack(alignment: .leading, spacing: 3) {
                    HStack {
                        Text(server.name)
                            .font(.headline)
                        Spacer()
                        protocolBadge(server.protocolNote)
                    }
                    // verbatim: Text's LocalizedStringKey interpolation
                    // locale-groups Ints ("6,667") -- the same defect class as
                    // the 4a T10 port-field fix (.grouping(.never), line 86).
                    Text(verbatim: "\(server.host):\(server.port)")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                    Text(server.blurb)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
                .frame(maxWidth: .infinity, alignment: .leading)
            }
            .contentShape(Rectangle())
            .padding(.vertical, 4)
        }
        .buttonStyle(.plain)
    }

    /// The "Custom Server" row as a Button: selecting it changes no settings
    /// (the fields already hold custom input), it only moves the highlight —
    /// so a user who typed a custom host, then wants the picker to reflect
    /// "Custom", can tap it explicitly (it is also selected automatically
    /// whenever the fields match no known entry, via `serverFieldBinding`).
    private var customRow: some View {
        Button {
            selectedServerID = Self.customSelectionID
        } label: {
            HStack(spacing: 8) {
                selectionMark(isSelected: selectedServerID == Self.customSelectionID)
                Image(systemName: "network")
                VStack(alignment: .leading, spacing: 2) {
                    Text("Custom Server")
                        .font(.headline)
                    Text("Enter your own host and room below.")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
                .frame(maxWidth: .infinity, alignment: .leading)
            }
            .contentShape(Rectangle())
            .padding(.vertical, 4)
        }
        .buttonStyle(.plain)
    }

    /// The leading selection checkmark for a picker row — filled when selected,
    /// a reserved blank slot otherwise so every row's text stays left-aligned.
    private func selectionMark(isSelected: Bool) -> some View {
        Image(systemName: isSelected ? "checkmark.circle.fill" : "circle")
            .foregroundStyle(isSelected ? Color.accentColor : Color.secondary.opacity(0.4))
            .imageScale(.large)
    }

    private func protocolBadge(_ note: String) -> some View {
        Text(note)
            .font(.caption2.weight(.medium))
            .padding(.horizontal, 6)
            .padding(.vertical, 2)
            .background(Capsule().fill(Color.secondary.opacity(0.15)))
            .foregroundStyle(.secondary)
    }

    /// The current known-entry id matching the persisted `settings.server`/
    /// `settings.port` exactly, else the "Custom Server" sentinel. Used to
    /// SEED `selectedServerID` on appear and to RE-DERIVE it whenever the
    /// server/port fields are edited (so custom input flips the highlight to
    /// "Custom Server", and typing a host that happens to match a known
    /// entry re-selects that entry). This is a plain read of `settings`, not
    /// a `Binding` — the picker's live selection lives in `@State`
    /// (`selectedServerID`), which SwiftUI actually tracks; `settings` (a
    /// notification-free `UserDefaults` wrapper) cannot drive the highlight
    /// on its own (see the type doc comment).
    private func derivedSelectionID() -> String {
        KnownServers.match(host: appState.settings.server, port: appState.settings.port)?.id
            ?? Self.customSelectionID
    }

    /// Write-through binding for the free-text Server field that ALSO
    /// re-derives `selectedServerID` on every edit — so typing a custom host
    /// visibly flips the picker to "Custom Server" (and typing one that
    /// matches a known entry re-highlights that row). Setting `selectedServerID`
    /// here only moves the HIGHLIGHT — it no longer triggers any settings
    /// write-back (the row Buttons own that), so a field edit can never
    /// clobber what the user just typed.
    private var serverFieldBinding: Binding<String> {
        Binding(
            get: { appState.settings.server },
            set: { newValue in
                appState.settings.server = newValue
                selectedServerID = derivedSelectionID()
            }
        )
    }

    /// Write-through binding for the Port field that likewise re-derives the
    /// picker selection (a custom port on an otherwise-known host flips the
    /// row to "Custom Server", since a known entry matches host AND port).
    private var portFieldBinding: Binding<Int> {
        Binding(
            get: { appState.settings.port },
            set: { newValue in
                appState.settings.port = newValue
                selectedServerID = derivedSelectionID()
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
