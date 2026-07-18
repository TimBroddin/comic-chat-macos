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

    @State private var mode: SendMode = .say

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
            Button("Send", action: send).keyboardShortcut(.defaultAction)
        }
        .padding(8)
    }

    private func send() {
        let text = composeText.trimmingCharacters(in: .whitespaces)
        guard !text.isEmpty, let model else { return }
        composeText = ""
        let sendMode = mode.stripMode
        Task { try? await model.send(text, mode: sendMode) }
    }
}
