import SwiftUI
import ComicChatKit

/// The Character Settings tab (Plan 4b Task 5 — D1 §1.9/§5: the original's
/// Options property sheet reborn as a SwiftUI `Settings` scene; this picker
/// was an Options property PAGE, not a standalone dialog, hence living here
/// rather than as its own `Window`/sheet). Grid of every bundled `.avb`'s
/// icon pose (`buildCatalog(artDir:)` filtered to `kind == "avatar"`,
/// `AvatarFile(path:).iconImage()` — the dedicated picker/member-list pose,
/// NOT one of the numbered gesture poses). Selecting a character writes
/// `settings.character` (persists for the NEXT connect) and, if a session is
/// already live (`appState.model != nil`), ALSO calls
/// `ChatSessionModel.changeCharacter` for an immediate live switch.
struct CharacterPickerView: View {
    @Environment(AppState.self) private var appState
    /// Built once per view lifetime (`.task`, not recomputed on every body
    /// re-evaluation) — decoding 32-ish `.avb` icon poses is fast but there is
    /// no reason to repeat it on every SwiftUI diff pass.
    @State private var thumbnails: [String: CGImage] = [:]
    @State private var names: [String] = []
    @State private var loadError: String?

    var body: some View {
        @Bindable var state = appState
        ScrollView {
            if let loadError {
                Text(loadError).foregroundStyle(.secondary).padding()
            } else {
                LazyVGrid(columns: [GridItem(.adaptive(minimum: 72, maximum: 96))], spacing: 12) {
                    ForEach(names, id: \.self) { name in
                        VStack(spacing: 4) {
                            thumbnailView(for: name)
                                .frame(width: 48, height: 48)
                                .background(
                                    RoundedRectangle(cornerRadius: 8)
                                        .fill(name == appState.settings.character
                                              ? Color.accentColor.opacity(0.25) : Color.clear))
                                .overlay(
                                    RoundedRectangle(cornerRadius: 8)
                                        .strokeBorder(name == appState.settings.character
                                                      ? Color.accentColor : Color.clear, lineWidth: 2))
                            Text(name.capitalized)
                                .font(.caption)
                                .lineLimit(1)
                        }
                        .padding(6)
                        .contentShape(Rectangle())
                        .onTapGesture { select(name) }
                    }
                }
                .padding()
            }
        }
        .frame(minWidth: 360, minHeight: 280)
        .task { loadCatalog() }
    }

    @ViewBuilder
    private func thumbnailView(for name: String) -> some View {
        if let image = thumbnails[name] {
            Image(decorative: image, scale: 1)
                .resizable().interpolation(.none)
                .aspectRatio(contentMode: .fit)
        } else {
            Image(systemName: "person.crop.square")
                .resizable().aspectRatio(contentMode: .fit)
                .foregroundStyle(.secondary)
                .padding(8)
        }
    }

    private func select(_ name: String) {
        appState.settings.character = name
        appState.model?.changeCharacter(name)
    }

    /// Loads every `.avb` in the app's comicart directory and its icon-pose
    /// thumbnail, once. Failures are non-fatal per-entry (a corrupt/missing
    /// file just has no thumbnail, falling back to the placeholder glyph
    /// above) — only a total catalog-build failure (bad `artDir`) surfaces
    /// `loadError`.
    private func loadCatalog() {
        guard thumbnails.isEmpty, names.isEmpty else { return }
        do {
            let entries = try buildCatalog(artDir: appState.artDir).filter { $0.kind == "avatar" }
            var built: [String: CGImage] = [:]
            for entry in entries {
                let bareName = (entry.file as NSString).deletingPathExtension
                guard let avatar = try? AvatarFile(path: appState.artDir + "/" + entry.file),
                      let icon = try? avatar.iconImage(),
                      let cg = icon.cgImage() else { continue }
                built[bareName] = cg
            }
            names = entries.map { ($0.file as NSString).deletingPathExtension }
            thumbnails = built
        } catch {
            loadError = "Could not load character art: \(error)"
        }
    }
}
