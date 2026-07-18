import SwiftUI
import ComicChatKit

/// The app's `Settings` scene (⌘,) — Plan 4b Task 5, D1 §1.9/§5: the
/// original's Options property sheet reborn. The original's Options dialog
/// was a tabbed property sheet (Persona/Character/Backdrop/Sounds/Advanced
/// pages); this is the direct SwiftUI equivalent — the character/backdrop
/// PICKERS are tabs of THIS sheet, not standalone windows/dialogs (a
/// correction the brief calls out explicitly: they were Options property
/// pages, never separate dialogs in the original).
///
/// Every field binds straight through to `AppState.settings`
/// (`SettingsStore`, itself a thin `UserDefaults` wrapper) — no separate
/// "Apply"/"OK" step, matching `ConnectSheet`'s existing write-through
/// posture. Persona (nick/real name) changes apply on the NEXT connect only
/// (`ProtocolSession`'s `userName`/`realName` are construction-time-only —
/// there is no live "change realname" wire command in the original protocol
/// either); Character/Backdrop selections, by contrast, DO apply live via
/// `ChatSessionModel.changeCharacter`/`changeBackdrop` when a session is
/// already connected (see `CharacterPickerView`/`BackdropPickerView`).
struct SettingsScene: View {
    @Environment(AppState.self) private var appState

    var body: some View {
        TabView {
            PersonaSettingsView()
                .tabItem { Label("Persona", systemImage: "person.crop.circle") }

            CharacterPickerView()
                .tabItem { Label("Character", systemImage: "theatermasks") }

            BackdropPickerView()
                .tabItem { Label("Backdrop", systemImage: "photo.on.rectangle") }

            SoundsSettingsView()
                .tabItem { Label("Sounds", systemImage: "speaker.wave.2") }

            AdvancedSettingsView()
                .tabItem { Label("Advanced", systemImage: "gearshape.2") }
        }
        .frame(minWidth: 420, minHeight: 340)
    }
}

/// Persona tab: nickname + real name, the USER-command identity fields
/// (Task 5's real gap — see `ProtocolSession.init`'s `userName`/`realName`
/// doc comment). Nickname already existed (`ConnectSheet`'s Identity
/// section) — surfaced again here since the original's Options sheet
/// carried both nick and real name on the same Persona page.
private struct PersonaSettingsView: View {
    @Environment(AppState.self) private var appState

    var body: some View {
        @Bindable var state = appState
        Form {
            Section {
                TextField("Nickname", text: settingsBinding(\.nick))
                TextField("Real Name", text: settingsBinding(\.realName))
            } footer: {
                Text("Applies on next connect.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        }
        .padding()
        .frame(minWidth: 360, minHeight: 280)
    }

    private func settingsBinding<T>(_ keyPath: WritableKeyPath<SettingsStore, T>) -> Binding<T> {
        Binding(
            get: { appState.settings[keyPath: keyPath] },
            set: { appState.settings[keyPath: keyPath] = $0 }
        )
    }
}

/// Sounds tab: enable toggle, folder path display + "Reveal in Finder"
/// (`SettingsStore.soundsFolder`'s default is the app's Application Support
/// directory — Task 9's inbound-sound-playback work is a later task; this
/// tab only exposes the SETTING, not playback itself).
private struct SoundsSettingsView: View {
    @Environment(AppState.self) private var appState

    var body: some View {
        @Bindable var state = appState
        Form {
            Toggle("Play Sounds", isOn: settingsBinding(\.soundsEnabled))
            LabeledContent("Sounds Folder") {
                HStack {
                    Text(appState.settings.soundsFolder)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .lineLimit(1)
                        .truncationMode(.middle)
                    Button("Reveal in Finder") { revealSoundsFolder() }
                }
            }
        }
        .padding()
        .frame(minWidth: 360, minHeight: 280)
    }

    private func settingsBinding<T>(_ keyPath: WritableKeyPath<SettingsStore, T>) -> Binding<T> {
        Binding(
            get: { appState.settings[keyPath: keyPath] },
            set: { appState.settings[keyPath: keyPath] = $0 }
        )
    }

    /// Ensures the folder exists (the default App Support path may not have
    /// been created yet — nothing writes into it before Task 9's sound
    /// playback lands) before asking Finder to show it, so "Reveal in
    /// Finder" doesn't silently no-op on a fresh install.
    private func revealSoundsFolder() {
        let path = appState.settings.soundsFolder
        let url = URL(fileURLWithPath: path, isDirectory: true)
        try? FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
        NSWorkspace.shared.activateFileViewerSelecting([url])
    }
}

/// Advanced tab: protocol/art toggles that don't fit the other tabs — send
/// comics data (the ComicsData annotation opt-out, `ChatConfig.sendComicsData`),
/// accept whispers (`ChatConfig.acceptWhispers`, wiring Task 4's
/// `_acceptWhispers` seam), auto-download avatars (`SettingsStore.autoDownloadAvatars`,
/// consumed by a later task's avatar-fetch work — this tab only exposes the
/// setting), and default encoding (moved here from `ConnectSheet`'s Server
/// section is NOT done — encoding stays on the connect sheet as a per-connect
/// choice; this is a separate DEFAULT so a fresh connect sheet starts from
/// the user's preferred encoding without needing to duplicate the picker's
/// meaning across two disconnected controls — same `SettingsStore.encoding`
/// key either way, so editing it here or on the connect sheet is the same
/// value).
private struct AdvancedSettingsView: View {
    @Environment(AppState.self) private var appState

    var body: some View {
        @Bindable var state = appState
        Form {
            Toggle("Send Comics Data", isOn: settingsBinding(\.sendComicsData))
            Toggle("Accept Whispers", isOn: settingsBinding(\.acceptWhispers))
            Toggle("Auto-Download Avatars", isOn: settingsBinding(\.autoDownloadAvatars))
            Picker("Default Encoding", selection: settingsBinding(\.encoding)) {
                Text("Windows-1252").tag(WireEncoding.cp1252)
                Text("UTF-8").tag(WireEncoding.utf8)
            }
        }
        .padding()
        .frame(minWidth: 360, minHeight: 280)
    }

    private func settingsBinding<T>(_ keyPath: WritableKeyPath<SettingsStore, T>) -> Binding<T> {
        Binding(
            get: { appState.settings[keyPath: keyPath] },
            set: { appState.settings[keyPath: keyPath] = $0 }
        )
    }
}
