import SwiftUI
import ComicChatKit

/// The server messages console (live-fix 4, Tim's request: "show the MOTD
/// like the original client"; also fixes error-notes being clobbered by the
/// next status line — `AppState.statusLine` is a single `String`, overwritten
/// on every `onStatus` call, so an error note is only ever visible until the
/// very next status line arrives). The original client showed MOTD/server
/// text in its status window; this reproduces that as a simple, always-
/// available, ACCUMULATING log of every `.motd`/`.statusLine`/`.error`/
/// `.disconnectedHint` line for the whole app session (`AppState
/// .serverMessages`, capped at 500 lines, drop-oldest).
///
/// A fixed `id` `Window` scene, same "handed off via AppState, opened via
/// openWindow(id:)" precedent as "whispers"/"roomList"/"transcriptViewer"
/// (`ComicChatApp`'s own doc comments on those scenes). Kept deliberately
/// simple per the brief: a monospaced, stick-to-bottom scrolling text list —
/// no per-line styling/filtering, since this is a diagnostic/nostalgia
/// console, not a primary chat surface.
struct ServerConsoleWindow: View {
    @Environment(AppState.self) private var appState

    var body: some View {
        ScrollViewReader { proxy in
            ScrollView {
                LazyVStack(alignment: .leading, spacing: 2) {
                    ForEach(Array(appState.serverMessages.enumerated()), id: \.offset) { idx, line in
                        Text(line)
                            .font(.system(.body, design: .monospaced))
                            .textSelection(.enabled)
                            .frame(maxWidth: .infinity, alignment: .leading)
                            .id(idx)
                    }
                }
                .padding(8)
            }
            .onChange(of: appState.serverMessages.count) { _, _ in
                // Stick-to-bottom: scroll to the newest line every time one
                // arrives, mirroring `WhisperBox`'s own scroll-to-latest
                // posture for its per-peer transcript.
                if let lastIndex = appState.serverMessages.indices.last {
                    proxy.scrollTo(lastIndex, anchor: .bottom)
                }
            }
        }
        .frame(minWidth: 420, minHeight: 280)
        .navigationTitle("Server Messages")
        .overlay {
            if appState.serverMessages.isEmpty {
                ContentUnavailableView("No Server Messages Yet", systemImage: "server.rack")
            }
        }
    }
}
