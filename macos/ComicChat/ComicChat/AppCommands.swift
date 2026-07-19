import SwiftUI
import AppKit

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
    @Environment(\.openSettings) private var openSettings

    var body: some Commands {
        // Hands this scene's `openWindow(id:)` action to `AppDelegate` for its
        // own Dock-reopen (`applicationShouldHandleReopen`) and launch-time
        // zero-window safety net (`applicationDidFinishLaunching`) — see that
        // type's own doc comment for both hazards. `body` runs once at launch
        // (and again on later environment/state changes SwiftUI decides
        // warrant a rebuild) as part of building the menu bar, regardless of
        // whether any window is open yet (menu commands must work even with
        // zero windows) — a reliable, always-set-early capture point, unlike
        // hooking a window's own `.onAppear`, which by definition never fires
        // if that window never appears (exactly the launch-time failure mode
        // this fix exists for). Re-assigning the same closure on every `body`
        // evaluation is harmless (idempotent, cheap). Plain `let _ =` is legal
        // inside a result-builder body (a local declaration, not part of the
        // built `Commands` result) — verified by a successful build below.
        let _ = { AppDelegate.shared?.reopenMainWindow = { openWindow(id: "main") } }()

        CommandGroup(replacing: .newItem) {
            // Bug fix (Tim's report: "New connection doesn't work when the
            // main window is closed"): the connect sheet is presented BY
            // `ChatWindow` (`.sheet(isPresented: $state.showConnectSheet)`,
            // that file) — with the main window closed there is no
            // `ChatWindow` instance around to host the sheet, so setting
            // `showConnectSheet = true` alone was a silent no-op. `openWindow`
            // is available here (`@Environment(\.openWindow)` is reachable
            // from `Commands` content on macOS 14+, confirmed working —
            // WWDC22 "Bring Multiple Windows to Your SwiftUI App"): reopen the
            // main WindowGroup by its fixed id FIRST, then flip the flag —
            // by the time the (possibly-fresh) `ChatWindow` instance's body
            // runs, `showConnectSheet` is already `true`, so its `.sheet`
            // presents immediately. If a "main" window is already open, this
            // is a harmless no-op that just brings it forward.
            Button("New Connection…") {
                openWindow(id: "main")
                appState.showConnectSheet = true
            }
            .keyboardShortcut("n", modifiers: .command)
        }

        // Plan 4b Task 10: save/reopen JSON transcript + PNG/PDF export.
        // Grouped `.saveItem` (right after New/Open, before the standard
        // Close/Save/Save As… block SwiftUI already supplies for a
        // WindowGroup-backed document-less app — this app has no
        // NSDocument, so `.saveItem` is otherwise empty and safe to replace).
        CommandGroup(replacing: .saveItem) {
            Button("Save Transcript…") {
                appState.saveTranscript()
            }
            .keyboardShortcut("s", modifiers: .command)
            .disabled(appState.model == nil)

            Button("Open Transcript…") {
                appState.openTranscript(currentWindowWidthPoints: NSApp.mainWindow?.contentView?.bounds.width ?? 640)
                if appState.showTranscriptViewer {
                    openWindow(id: "transcriptViewer")
                }
            }

            Divider()

            Button("Export as PNG…") {
                appState.exportPNG()
            }
            .disabled(appState.model == nil || appState.stripImage == nil)

            Button("Export as PDF…") {
                appState.exportPDF()
            }
            .disabled(appState.model == nil || appState.stripImage == nil)
        }

        CommandGroup(replacing: .printItem) {
            Button("Print…") {
                appState.printTranscript()
            }
            .keyboardShortcut("p", modifiers: .command)
            .disabled(appState.model == nil || appState.stripImage == nil)
        }

        // Plan 4b Task 11 (D1 §1.2/spec §5): toggles `ChatWindow` between the
        // comic strip and the plain-text transcript view. Two checkable-by-
        // convention buttons rather than a single `Toggle` menu item (SwiftUI
        // `Commands` has no native checkmark-Toggle-in-menu API) — matches
        // how the rest of this app's `CommandMenu`s are built (plain
        // `Button`s with keyboard shortcuts).
        CommandMenu("View") {
            Button("Comic Strip view") {
                appState.comicMode = true
            }
            .keyboardShortcut("1", modifiers: .command)

            Button("Plain Text view") {
                appState.comicMode = false
            }
            .keyboardShortcut("2", modifiers: .command)
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

            // Plan 4b Task 8: the LIST browser window.
            Button("Room List…") {
                openWindow(id: "roomList")
            }
            .disabled(appState.model == nil)

            // Plan 4b Task 8: create (and go to) a new room.
            Button("Create Room…") {
                appState.createRoomText = ""
                appState.showCreateRoomSheet = true
            }
            .disabled(appState.model == nil)

            // Quick-wins batch item 6: seeds the sheet's text field from the
            // ACTIVE room's CURRENT topic (`RoomInfo.topic`, already mirrored
            // model-side from `.topicChanged`) rather than starting blank.
            Button("Set Topic…") {
                appState.setTopicText = appState.rooms.first(where: \.isActive)?.topic ?? ""
                appState.showSetTopicSheet = true
            }
            .disabled(appState.model == nil || appState.activeRoom == nil)

            Divider()

            // Plan 4b Task 8: session-scoped Away toggle.
            Button(appState.isAway ? "Away (On)" : "Away") {
                appState.toggleAway()
            }
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

            Divider()

            // Quick-wins batch item 2 (the original's own gesture,
            // bodycam.cpp:351: double-click the pose pane opens Options at
            // the character page): a menu-driven twin of that gesture, for
            // discoverability — same `settingsTab` preselect + `openSettings()`
            // call `PosePreviewPane`'s double-click makes.
            Button("Choose Character…") {
                appState.settingsTab = .characters
                openSettings()
            }
        }

        // Live-fix 4 (Tim's request: "show the MOTD like the original
        // client"): appended to the standard Window menu (not gated on a
        // live session — the console is a running log across the whole app
        // session, so it's openable even while disconnected to review
        // earlier messages).
        CommandGroup(after: .windowArrangement) {
            Button("Server Messages") {
                openWindow(id: "server-console")
            }
        }
    }
}
