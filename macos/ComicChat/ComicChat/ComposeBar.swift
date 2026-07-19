import SwiftUI
import ComicChatKit

/// The compose row: text field (drives the wheel's live typing-preview via
/// `previewTyping`) + a say/think/action mode picker + Send. Whisper goes
/// through the separate whisper box (Plan 4b Task 4) rather than this
/// picker — `Strip.Mode` has no `Hashable`/`CaseIterable` conformance (it's
/// a plain `OptionSet`), so the picker binds to this file's own
/// `SendMode` enum and converts to `Strip.Mode` only at the `send` call site.
enum SendMode: String, CaseIterable, Identifiable {
    case say = "Say"
    case think = "Think"
    case action = "Action"
    var id: String { rawValue }

    var stripMode: Strip.Mode {
        switch self {
        case .say: return .say
        case .think: return .think
        case .action: return .action
        }
    }
}

struct ComposeBar: View {
    @Binding var composeText: String
    var model: ChatSessionModel?
    /// Plan 4b Task 8 (D1 §1.5): the member-list selection, threaded straight
    /// through to `ChatSessionModel.send`'s `addressees:` — the original's
    /// canonical talk-to state. Empty by default so call sites that don't
    /// pass one (none currently exist, but kept optional-shaped) behave
    /// exactly as before this task.
    var selectedMembers: Set<String> = []
    /// The user's sounds folder (outbound-sound task, `settings.soundsFolder`)
    /// — plain `String`, not `AppState`, so this view stays testable/preview-
    /// able without an `@Environment` dependency (matches this file's existing
    /// posture of taking plain values, per `ChatWindow`'s own doc comment
    /// about `ComicStripView`'s parameter-passing contract).
    var soundsFolder: String = ""

    @State private var mode: SendMode = .say
    @State private var showSoundPicker = false

    var body: some View {
        HStack {
            TextField("Say something…", text: $composeText)
                .textFieldStyle(.roundedBorder)
                .onChange(of: composeText) { _, newValue in model?.previewTyping(newValue) }
                .onSubmit { send() }
            Picker("", selection: $mode) {
                ForEach(SendMode.allCases) { m in Text(m.rawValue).tag(m) }
            }
            .pickerStyle(.segmented)
            .frame(width: 180)
            .labelsHidden()
            // Send Sound (outbound-sound task): a compact popover matching
            // the original's CSoundDlg spirit (browse the sounds folder,
            // send one) without a whole separate dialog window — the folder
            // is small/local (no rating/search UI the original dialog had,
            // R20: those are a full-file-browser concern, not core to
            // "pick a sound and send it").
            Button {
                showSoundPicker = true
            } label: {
                Image(systemName: "speaker.wave.2")
            }
            .help("Send Sound…")
            .popover(isPresented: $showSoundPicker) {
                SoundPickerPopover(soundsFolder: soundsFolder, model: model)
            }
            Button("Send", action: send).keyboardShortcut(.defaultAction)
        }
        .padding(8)
    }

    private func send() {
        let text = composeText.trimmingCharacters(in: .whitespaces)
        guard !text.isEmpty, let model else { return }
        composeText = ""
        let sendMode = mode.stripMode
        // Plan 4b Task 8 self-review fix: `selectedMembers` is a `Set`
        // (SwiftUI `List`'s multi-selection binding type — no ordering to
        // preserve), so `Array(selectedMembers)` alone has NON-DETERMINISTIC
        // order (Set enumeration order can vary run-to-run/build-to-build).
        // `Annotations.toCAnnotations`'s wire encoder clips to the first 5
        // (D1 §2.1) — an unsorted array would clip an arbitrary, unstable
        // subset when more than 5 members are selected, silently addressing
        // different recipients on rebuilds/relaunches for the identical UI
        // selection. Sorting alphabetically makes the clip deterministic and
        // matches the member list's own display order (`emitMembers` sorts
        // by nick) — not true "selection order" (Set can't represent that),
        // but stable and predictable.
        let addressees = selectedMembers.sorted()
        Task { try? await model.send(text, mode: sendMode, addressees: addressees) }
    }
}

/// The Send Sound popover's contents: lists every `.wav` in `soundsFolder`
/// (`SoundLibrary.list()`, outbound-sound task) with a Send button per row,
/// and an empty state that reveals the folder in Finder (mirrors
/// `SoundsSettingsView.revealSoundsFolder`'s own "ensure it exists, then
/// show it" posture) when there's nothing to send yet — the common first-run
/// case, since the folder ships empty (D1 §4.2, `SoundLibrary`'s own doc
/// comment: no bundled WAVs exist anywhere in this repo's trees).
private struct SoundPickerPopover: View {
    @Environment(\.dismiss) private var dismiss
    let soundsFolder: String
    let model: ChatSessionModel?

    @State private var names: [String] = []

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            if names.isEmpty {
                VStack(spacing: 8) {
                    Text("No Sounds")
                        .font(.headline)
                    Text("Drop .wav files into your sounds folder, then reopen this menu.")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .multilineTextAlignment(.center)
                    Button("Open Sounds Folder") { revealSoundsFolder() }
                }
                .padding()
                .frame(width: 260)
            } else {
                List(names, id: \.self) { name in
                    HStack {
                        Text(name)
                        Spacer()
                        Button("Send") { sendSound(name) }
                    }
                }
                .frame(width: 260, height: min(CGFloat(names.count) * 28 + 16, 320))
            }
        }
        .task { reload() }
    }

    private func reload() {
        guard !soundsFolder.isEmpty else { return }
        let folder = URL(fileURLWithPath: soundsFolder, isDirectory: true)
        names = SoundLibrary(folder: folder).list()
    }

    private func sendSound(_ name: String) {
        guard let model else { return }
        Task { try? await model.sendSound(file: name) }
        dismiss()
    }

    private func revealSoundsFolder() {
        guard !soundsFolder.isEmpty else { return }
        let url = URL(fileURLWithPath: soundsFolder, isDirectory: true)
        try? FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
        NSWorkspace.shared.activateFileViewerSelecting([url])
    }
}
