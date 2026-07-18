import SwiftUI

/// App-wide menu commands. `File > New Connection` reopens the connect
/// sheet; `Room > Leave/Disconnect` tears down the current session (routing
/// through `AppState.disconnect()`, which calls the mandatory
/// `ChatSessionModel.shutdown()` before dropping the model — see Task 9's
/// review note: there is no `deinit`, so dropping a model without shutting
/// it down first leaves a dangling engine global). Edit/Window menus are
/// left as SwiftUI's standard defaults (no customization needed yet).
struct AppCommands: Commands {
    var appState: AppState

    var body: some Commands {
        CommandGroup(replacing: .newItem) {
            Button("New Connection…") {
                appState.showConnectSheet = true
            }
            .keyboardShortcut("n", modifiers: .command)
        }

        CommandMenu("Room") {
            Button("Leave/Disconnect") {
                appState.disconnect()
                appState.showConnectSheet = true
            }
            .disabled(appState.model == nil)
        }
    }
}
