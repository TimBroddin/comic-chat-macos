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
        @Bindable var state = appState
        // Quick-wins batch item 2 (the original's double-click-opens-Options-
        // at-the-character-page gesture, bodycam.cpp:351): `selection:` bound
        // to `AppState.settingsTab` so `PosePreviewPane`'s double-click gesture
        // (and the Member/View menu's "Choose Character…" item) can preselect
        // the Character tab BEFORE calling `openSettings()`.
        TabView(selection: $state.settingsTab) {
            PersonaSettingsView()
                .tabItem { Label("Persona", systemImage: "person.crop.circle") }
                .tag(SettingsTab.persona)

            CharacterPickerView()
                .tabItem { Label("Character", systemImage: "theatermasks") }
                .tag(SettingsTab.characters)

            BackdropPickerView()
                .tabItem { Label("Backdrop", systemImage: "photo.on.rectangle") }
                .tag(SettingsTab.backdrop)

            ComicSettingsView()
                .tabItem { Label("Comic", systemImage: "square.grid.2x2") }
                .tag(SettingsTab.comic)

            SoundsSettingsView()
                .tabItem { Label("Sounds", systemImage: "speaker.wave.2") }
                .tag(SettingsTab.sounds)

            AdvancedSettingsView()
                .tabItem { Label("Advanced", systemImage: "gearshape.2") }
                .tag(SettingsTab.advanced)
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
            // Plan 4b Batch C: the free-text reply a peer's "# GetInfo" probe
            // receives (`SettingsStore.profileText` -> `ChatConfig.profileText`
            // -> `cc_session_send_info_reply`'s "# HeresInfo: <profile>" wire
            // reply). Unlike nick/real name above, this DOES apply live — it's
            // read fresh from `config.profileText` at reply time (`handleLocked`'s
            // `.infoRequest` case), not baked into the USER command at connect.
            Section {
                TextEditor(text: profileTextBinding)
                    .frame(minHeight: 80)
            } header: {
                Text("Profile")
            } footer: {
                Text("Sent to peers who probe your profile (\"# GetInfo\"). Applies immediately.")
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

    /// Plan 4b Batch C: like `ComicSettingsView.panelsPerRowBinding`, this
    /// writes through to BOTH the persisted setting and (if a session is
    /// already connected) the live model via `setProfileText` — an already-
    /// connected peer's NEXT "# GetInfo" probe should see the freshly typed
    /// profile without requiring a reconnect.
    private var profileTextBinding: Binding<String> {
        Binding(
            get: { appState.settings.profileText },
            set: { newValue in
                appState.settings.profileText = newValue
                appState.model?.setProfileText(newValue)
            }
        )
    }
}

/// Comic tab (quick-wins batch item 3, original `UnitsWide`): a Picker for
/// `SettingsStore.panelsPerRow` — Automatic (0) or a forced 1...5 column count
/// (`PanelFit`'s own `FitPanelsWide` cap, `PanelFit.columns`'s `1...5` scan
/// range). Live change: writes through `appState.settings.panelsPerRow`
/// (persisted) AND, when a session is already live, calls
/// `ChatSessionModel.setPanelsPerRow` for an immediate reflow — the same
/// "persist + live-apply if connected" shape `CharacterPickerView.select`
/// already uses for `changeCharacter`.
///
/// Batch E (the ticketed comic font surface): a face `TextField` + size
/// `Stepper`, the same "persist + live-apply if connected" shape as the
/// panels-per-row picker above — `ChatSessionModel.setComicFont` handles the
/// live case (full reflow; fonts are per-strip, see that method's doc
/// comment). A `NSFontManager`-backed system font picker would be more
/// discoverable, but the brief calls it overkill for a CP-1252 LOGFONT face
/// name the engine doesn't validate against installed fonts anyway — a plain
/// text field (matching the original's own Options-dialog font-name edit
/// box) is the simpler, honest control here.
private struct ComicSettingsView: View {
    @Environment(AppState.self) private var appState

    var body: some View {
        Form {
            Picker("Panels Per Row", selection: panelsPerRowBinding) {
                Text("Automatic").tag(0)
                Text("1").tag(1)
                Text("2").tag(2)
                Text("3").tag(3)
                Text("4").tag(4)
                Text("5").tag(5)
            }
            Section("Balloon Font") {
                TextField("Face", text: comicFontFaceBinding)
                Stepper("Size: \(appState.settings.comicFontSize == 0 ? "Default" : "\(appState.settings.comicFontSize) pt")",
                        value: comicFontSizeBinding, in: 0...72)
            }
        }
        .padding()
        .frame(minWidth: 360, minHeight: 280)
    }

    private var panelsPerRowBinding: Binding<Int> {
        Binding(
            get: { appState.settings.panelsPerRow },
            set: { newValue in
                appState.settings.panelsPerRow = newValue
                appState.model?.setPanelsPerRow(newValue)
            }
        )
    }

    private var comicFontFaceBinding: Binding<String> {
        Binding(
            get: { appState.settings.comicFontFace },
            set: { newValue in
                appState.settings.comicFontFace = newValue
                appState.model?.setComicFont(face: newValue, sizePoints: appState.settings.comicFontSize)
            }
        )
    }

    private var comicFontSizeBinding: Binding<Int> {
        Binding(
            get: { appState.settings.comicFontSize },
            set: { newValue in
                appState.settings.comicFontSize = newValue
                appState.model?.setComicFont(face: appState.settings.comicFontFace, sizePoints: newValue)
            }
        )
    }
}

/// Sounds tab: enable toggle, folder path display + "Reveal in Finder"
/// (`SettingsStore.soundsFolder`'s default is the app's Application Support
/// directory), plus a caption documenting the three conventionally-named
/// event-sound files (Batch E) — `SoundLibrary.resolve` looks these up by
/// bare name against whatever the user has actually dropped into the
/// folder; an absent file is silence, never a beep/error (that type's own
/// doc comment), so this caption is the only place that convention is
/// written down for the user.
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
            Section {
                Text("Drop .wav files into the sounds folder using these names to enable event sounds:")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                Text("mention.wav — someone mentions your name\nwhisper.wav — you receive a whisper\njoin.wav — someone joins the active room")
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
            // Batch B: gates AppState's mention/whisper UNUserNotificationCenter
            // posts (`ChatSessionModel.onNotificationEvent`'s app-layer consumer).
            Toggle("Notify on Mentions & Whispers", isOn: settingsBinding(\.notificationsEnabled))
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
