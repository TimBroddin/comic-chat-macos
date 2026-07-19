import SwiftUI
import ComicChatKit

@main
struct ComicChatApp: App {
    @State private var appState = AppState()
    // Bug fix (Tim's report: "New connection doesn't work when the main
    // window is closed"): `AppCommands`' New Connection action needs to be
    // able to REOPEN the main window before it can present the connect
    // sheet — but with the window closed there's no `ChatWindow` around to
    // host a `@Environment(\.openWindow)`. Giving the `WindowGroup` a fixed
    // `id` ("main") is what makes it targetable via `openWindow(id:)` at
    // all (an unnamed/default-id WindowGroup can still be reopened, but an
    // explicit id is the documented, reliable way to address a SPECIFIC
    // scene when several window-producing scenes coexist in this app, same
    // precedent as "whispers"/"roomList"/"transcriptViewer"/"server-console"
    // below). This changes nothing else about the scene: same title, same
    // content, same command/environment wiring.
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var appDelegate

    var body: some Scene {
        mainWindowGroup
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

        // Plan 4b Task 10: the reopened-transcript viewer (File > Open
        // Transcript…) — a fixed `id` `Window` scene, same "handed off via
        // AppState, opened via openWindow(id:)" precedent as "whispers"/
        // "roomList" above. `AppState.viewerImage` is the hand-off; there is
        // no per-open parameter needed beyond that.
        Window("Transcript", id: "transcriptViewer") {
            TranscriptViewerWindow().environment(appState)
        }

        // Live-fix 4 (Tim's request: "show the MOTD like the original
        // client"): the accumulating server-messages console — a fixed `id`
        // `Window` scene, same precedent as "whispers"/"roomList"/
        // "transcriptViewer" above. `AppState.serverMessages` is the data
        // source; no per-open hand-off parameter needed (it just shows
        // whatever has accumulated so far).
        Window("Server Messages", id: "server-console") {
            ServerConsoleWindow().environment(appState)
        }
    }

    /// The main `WindowGroup`, split out from `body` for readability.
    ///
    /// (Root-cause mitigation attempted and abandoned: AppKit's
    /// persistent-UI window restoration can hold a stale saved-window
    /// identifier from an earlier build/run of this app and then fail to
    /// restore it SILENTLY, launching with zero windows — see
    /// `AppDelegate`'s doc comment, hazard (b), for the full story and the
    /// `applicationDidFinishLaunching` recovery net that actually fixes it.
    /// `Scene.restorationBehavior(.disabled)` (macOS 15+/Xcode 16+ only)
    /// would additionally suppress the hazard at its source on 15+, but
    /// gating it behind `#available` inside ANY `SceneBuilder` context in
    /// this toolchain — top-level `body`, a nested computed property, a
    /// `@SceneBuilder`-marked function — hits "closure containing control
    /// flow statement cannot be used with result builder 'SceneBuilder'"
    /// every time; confirmed not specific to one placement. Not worth
    /// fighting further: the `AppDelegate` net below is deployment-target-
    /// agnostic and already verified working live (Console logs + direct
    /// window enumeration) across macOS 14 and this machine's newer OS, so
    /// it is the sole reopen/recovery mechanism, unconditionally.)
    private var mainWindowGroup: some Scene {
        WindowGroup("Comic Chat", id: "main") {
            ChatWindow().environment(appState)
        }
    }
}

/// Bug fix (Tim's report: "New connection doesn't work when the main window
/// is closed") — Dock-icon-click reopen, AND a launch-time safety net.
///
/// Two separate hazards, both routed through this one delegate:
///
/// (a) Dock-icon-click reopen: plain SwiftUI's `WindowGroup` has no
/// documented/guaranteed automatic Dock-reopen once every one of its windows
/// is closed while other `Window` scenes still exist in the same app (this
/// app has four: "whispers"/"roomList"/"transcriptViewer"/"server-console");
/// `applicationShouldHandleReopen(_:hasVisibleWindows:)` is the standard
/// AppKit-level hook for that specific gesture, with no direct SwiftUI
/// equivalent as of macOS 14/15.
///
/// (b) LAUNCH-time zero-window failure (found live while verifying fix (a),
/// reproduced on this very machine): AppKit's persistent-UI window
/// restoration (`hasPersistentStateToRestore=1` in Console, backed by
/// `com.apple.appkit.restoration_storage` — a system-level cache, NOT the
/// `~/Library/Saved Application State/<bundle-id>.savedState` directory,
/// which need not even exist for this to fire) can hold a STALE saved window
/// identifier from a previous build/run of the app. When that identifier no
/// longer resolves, `_restoreWindowWithRestoration:` fails SILENTLY
/// (`window=0x0 error=(null)`) and — critically — does NOT fall back to
/// creating a fresh default window. Net effect: the app launches with ZERO
/// windows and no error, indistinguishable from the user having closed it
/// (this is plausibly the ROOT CAUSE of the original bug report, if Tim's
/// "closed the window" session was actually this launch-time failure rather
/// than a genuine user-initiated close). `applicationDidFinishLaunching`
/// below is the safety net: shortly after launch, if AppKit still hasn't
/// materialized any window, force one open via the same `openWindow(id:)`
/// mechanism (b) is regardless of restoration's internal bookkeeping.
///
/// Neither hook can carry its own `@Environment(\.openWindow)` (an `NSObject`
/// has no SwiftUI environment) — `reopenMainWindow` is instead captured from
/// `AppCommands.body`, which SwiftUI evaluates once at launch as part of
/// building the menu bar, independent of whether any window exists yet (menu
/// commands must work with zero windows open, same as any standard Mac app's
/// File menu) — see that call site's own doc comment.
final class AppDelegate: NSObject, NSApplicationDelegate {
    /// Set once by `AppCommands.body` at launch (menu-bar construction runs
    /// with or without any window open) and never needs re-setting after
    /// that — `OpenWindowAction` remains valid for the process's lifetime.
    var reopenMainWindow: (() -> Void)?
    /// So `AppCommands.body` can reach this instance without threading its
    /// own reference through `AppState`/environment — `NSApplicationDelegateAdaptor`
    /// creates exactly one `AppDelegate` for the process's lifetime, matching
    /// the singleton assumption here.
    static private(set) weak var shared: AppDelegate?

    override init() {
        super.init()
        AppDelegate.shared = self
    }

    func applicationShouldHandleReopen(_ sender: NSApplication, hasVisibleWindows flag: Bool) -> Bool {
        if !flag {
            reopenMainWindow?()
        }
        return true
    }

    /// Launch-time safety net for hazard (b) above. A short delay (rather
    /// than checking synchronously) because AppKit's own restoration attempt
    /// (successful OR silently-failed) needs a beat to finish running on the
    /// main run loop first — checking `NSApp.windows` synchronously inside
    /// `applicationDidFinishLaunching` itself would race that attempt and
    /// could double-open a window that was ABOUT to restore successfully.
    /// 0.5s is generous relative to the sub-millisecond gap observed between
    /// "Restoring windows" and the failed restore callback in Console.
    func applicationDidFinishLaunching(_ notification: Notification) {
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { [weak self] in
            guard let self, NSApp.windows.filter({ $0.isVisible }).isEmpty else { return }
            self.reopenMainWindow?()
        }
    }
}
