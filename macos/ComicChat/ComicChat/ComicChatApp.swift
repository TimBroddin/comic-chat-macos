import SwiftUI
import ComicChatKit

@main
struct ComicChatApp: App {
    @State private var appState = AppState()
    var body: some Scene {
        WindowGroup("Comic Chat") {
            ContentRoot().environment(appState)
        }
        .commands { AppCommands(appState: appState) }
    }
}
