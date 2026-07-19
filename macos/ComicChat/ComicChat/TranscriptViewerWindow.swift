import SwiftUI
import ComicChatKit

/// The reopened-transcript viewer (Plan 4b Task 10, File > Open Transcript…):
/// a plain, read-only presentation of a `TranscriptRenderer`-composed image —
/// reuses `ComicStripView(image:sizePoints:model:)` with `model: nil` (that
/// view already tolerates a nil model per the brief: no live session backs a
/// reopened transcript, so there's nothing to reflow against and no compose
/// bar target). No compose bar, no member sidebar — just the strip, scrolled,
/// matching the original's "reopen shows the finished comic" posture.
struct TranscriptViewerWindow: View {
    @Environment(AppState.self) private var appState

    var body: some View {
        ComicStripView(image: appState.viewerImage,
                       sizePoints: appState.viewerSizePoints,
                       model: nil)
            .frame(minWidth: 480, minHeight: 360)
            .navigationTitle("Transcript")
    }
}
