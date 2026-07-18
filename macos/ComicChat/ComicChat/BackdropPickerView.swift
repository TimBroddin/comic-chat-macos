import SwiftUI
import ComicChatKit

/// The Backdrop Settings tab (Plan 4b Task 5), the `BackdropFile` counterpart
/// to `CharacterPickerView` — see that type's doc comment for the shared
/// design rationale (Options-property-page-as-Settings-tab, thumbnail cache
/// built once, selection writes settings + drives a live switch). Backdrops
/// have no icon/pose concept (`BackdropFile.image()` is the whole decoded
/// image, `ArtFile.swift`'s doc comment) — thumbnails here are the full
/// backdrop image scaled down by the grid cell, not a dedicated icon pose.
struct BackdropPickerView: View {
    @Environment(AppState.self) private var appState
    @State private var thumbnails: [String: CGImage] = [:]
    @State private var names: [String] = []
    @State private var loadError: String?

    var body: some View {
        ScrollView {
            if let loadError {
                Text(loadError).foregroundStyle(.secondary).padding()
            } else {
                LazyVGrid(columns: [GridItem(.adaptive(minimum: 96, maximum: 128))], spacing: 12) {
                    ForEach(names, id: \.self) { name in
                        VStack(spacing: 4) {
                            thumbnailView(for: name)
                                .frame(width: 72, height: 54)
                                .clipShape(RoundedRectangle(cornerRadius: 6))
                                .overlay(
                                    RoundedRectangle(cornerRadius: 6)
                                        .strokeBorder(name == appState.settings.backdrop
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
                .resizable().interpolation(.medium)
                .aspectRatio(contentMode: .fill)
        } else {
            Rectangle()
                .fill(Color.secondary.opacity(0.15))
                .overlay(Image(systemName: "photo").foregroundStyle(.secondary))
        }
    }

    private func select(_ name: String) {
        appState.settings.backdrop = name
        appState.model?.changeBackdrop(name)
    }

    /// Loads every `.bgb` in the app's comicart directory and its decoded
    /// image, once. Same non-fatal-per-entry-failure posture as
    /// `CharacterPickerView.loadCatalog`.
    private func loadCatalog() {
        guard thumbnails.isEmpty, names.isEmpty else { return }
        do {
            let entries = try buildCatalog(artDir: appState.artDir).filter { $0.kind == "backdrop" }
            var built: [String: CGImage] = [:]
            for entry in entries {
                let bareName = (entry.file as NSString).deletingPathExtension
                guard let backdrop = try? BackdropFile(path: appState.artDir + "/" + entry.file),
                      let image = try? backdrop.image(),
                      let cg = image.cgImage() else { continue }
                built[bareName] = cg
            }
            names = entries.map { ($0.file as NSString).deletingPathExtension }
            thumbnails = built
        } catch {
            loadError = "Could not load backdrop art: \(error)"
        }
    }
}
