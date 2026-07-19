import SwiftUI
import ComicChatKit

@main
struct ComicChatApp: App {
    @State private var appState = AppState()
    var body: some Scene {
        WindowGroup("Comic Chat") {
            ChatWindow().environment(appState)
        }
        .commands { AppCommands(appState: appState) }

        // Plan 4b Task 4: ONE tabbed whisper box, not a window per peer (D1
        // §0 correction to spec §5 — see WhisperBox's own doc comment). A
        // single fixed `id` scene; which peer's tab is pre-selected on open
        // is handed off via `AppState.pendingWhisperPeer` (a `Window` scene
        // takes no per-open parameter here), read by `WhisperBox.task`.
        Window("Whispers", id: "whispers") {
            WhisperBox().environment(appState)
        }

        // Plan 4b Task 5: the original's Options property sheet, reborn as
        // the standard macOS Settings scene (⌘,) — see SettingsScene's own
        // doc comment for why the character/backdrop pickers are TABS here
        // rather than separate windows.
        Settings {
            SettingsScene().environment(appState)
        }

        // Plan 4b Task 8: the room list browser (`CRoomList`'s LIST browser,
        // reborn) — a fixed `id` `Window` scene, same precedent as
        // "Whispers" above (opened via Room > Room List…).
        Window("Room List", id: "roomList") {
            RoomListWindow().environment(appState)
        }
    }
}
