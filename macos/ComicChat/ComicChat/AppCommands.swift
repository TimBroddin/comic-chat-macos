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
    @Environment(\.openWindow) private var openWindow

    var body: some Commands {
        CommandGroup(replacing: .newItem) {
            Button("New Connection…") {
                appState.showConnectSheet = true
            }
            .keyboardShortcut("n", modifiers: .command)
        }

        CommandMenu("Room") {
            // Plan 4b Task 7: joins an ADDITIONAL room on the current
            // connection (opens the Enter Room sheet — the tab bar's "+"
            // twin). Requires a live session.
            Button("Enter Room…") {
                appState.enterRoomText = ""
                appState.showEnterRoomSheet = true
            }
            .keyboardShortcut("j", modifiers: .command)
            .disabled(appState.model == nil)

            Divider()

            Button("Leave/Disconnect") {
                appState.disconnect()
                appState.showConnectSheet = true
            }
            .disabled(appState.model == nil)
        }

        // Plan 4b Task 4: menu-driven entry point to the whisper box, for
        // when there's no member row to right-click (or no member selected
        // yet) — opens with no peer pre-selected (`showWhisperBox(peer: nil)`
        // is a no-op on the pending-selection state, `AppState`'s own doc
        // comment), landing on whichever peer (if any) the box last had.
        CommandMenu("Member") {
            Button("Whisper…") {
                appState.showWhisperBox(peer: nil)
                openWindow(id: "whispers")
            }
            .disabled(appState.model == nil)
        }
    }
}
