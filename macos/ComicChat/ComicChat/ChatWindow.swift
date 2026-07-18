import SwiftUI
import ComicChatKit

/// The MVP chat window (Plan 4a Task 12, the plan's exit-milestone task):
/// comic strip + compose bar + status line on the left, member sidebar on
/// the right. Replaces `ContentRoot` as the app's root view.
///
/// `ComicStripView` construction (review correction to this task's brief):
/// `ComicStripView` takes plain `let image`/`sizePoints`/`model` — it does
/// NOT read `AppState` via `@Environment` (see that type's own doc comment:
/// there is no documented guarantee `updateNSView` re-runs purely because an
/// `@Observable` dependency read INSIDE it changed). So this view's `body`
/// must read `appState.stripImage`/`stripSizePoints`/`model` itself and pass
/// them down as plain values — SwiftUI's struct-diffing of those stored
/// properties is what reliably drives `updateNSView`.
struct ChatWindow: View {
    @Environment(AppState.self) private var appState
    @State private var composeText = ""

    var body: some View {
        @Bindable var state = appState
        HSplitView {
            VStack(spacing: 0) {
                ComicStripView(image: appState.stripImage,
                                sizePoints: appState.stripSizePoints,
                                model: appState.model)
                Divider()
                ComposeBar(composeText: $composeText, model: appState.model)
                Text(appState.statusLine)
                    .font(.caption).foregroundStyle(.secondary)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(.horizontal, 8).padding(.bottom, 4)
            }
            // Original layout (chatview.cpp:333-378): members over bodycam.
            VStack(spacing: 0) {
                List(appState.members, id: \.self) { nick in Text(nick) }
                Divider()
                BodyCamView(poseImage: appState.selfPoseImage,
                            onEmotion: { angle, intensity in
                    appState.model?.setEmotion(angle: angle, intensity: intensity)
                })
            }
            .frame(minWidth: 140, maxWidth: 220)
        }
        .frame(minWidth: 640, minHeight: 480)
        .sheet(isPresented: $state.showConnectSheet) { ConnectSheet() }
        .task {
            // Offline demo hook (Task 12): `--replay-fixture <path>` should
            // demo the app with zero interaction — skip the connect sheet
            // and connect immediately (`AppState.connect()` reads the flag
            // itself and overrides host/port/nick/room to match the fixture).
            if ProcessInfo.processInfo.arguments.contains("--replay-fixture") {
                appState.showConnectSheet = false
                await appState.connect()
            }
        }
    }
}
