import SwiftUI

/// Temporary placeholder content — replaced in Task 12 by the real MVP chat
/// window (member list + `ComicStripView` + input line). For now it exists
/// only to host the connect sheet and prove the app boots.
struct ContentRoot: View {
    @Environment(AppState.self) private var appState

    var body: some View {
        @Bindable var appState = appState

        Text(appState.statusLine.isEmpty ? "Not connected" : appState.statusLine)
            .frame(minWidth: 480, minHeight: 360)
            .sheet(isPresented: $appState.showConnectSheet) {
                ConnectSheet()
            }
    }
}
