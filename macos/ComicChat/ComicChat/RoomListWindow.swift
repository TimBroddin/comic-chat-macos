import SwiftUI
import ComicChatKit

/// The room list browser (Plan 4b Task 8, D1 §1.6/backlog item 5): the
/// original's `CRoomList` LIST browser, reborn — a `Table` of the server's
/// rooms (Name/Users/Topic) with a client-side filter field and min-users
/// stepper (matching the original's `roomlist.cpp:27-60` client-side
/// filtering — the server sends the full list; narrowing it is a local
/// operation), a Refresh button, and Go To (join + activate, closing this
/// window — `AppState.goToRoom`'s own doc comment covers the join-without-
/// part-first semantics).
struct RoomListWindow: View {
    @Environment(AppState.self) private var appState
    @Environment(\.dismiss) private var dismiss

    @State private var filterText = ""
    @State private var minUsers = 0
    @State private var selection: RoomListItem.ID?

    private var filteredRooms: [RoomListItem] {
        appState.roomList.filter { item in
            guard item.users >= Int32(minUsers) else { return false }
            guard !filterText.isEmpty else { return true }
            return item.name.localizedCaseInsensitiveContains(filterText)
                || item.topic.localizedCaseInsensitiveContains(filterText)
        }
    }

    var body: some View {
        VStack(spacing: 0) {
            HStack {
                TextField("Filter (name or topic)", text: $filterText)
                    .textFieldStyle(.roundedBorder)
                    .frame(minWidth: 200)
                Stepper("Min users: \(minUsers)", value: $minUsers, in: 0...999)
                Spacer()
                Button("Refresh") { appState.requestRoomList() }
            }
            .padding(8)
            Divider()

            Table(filteredRooms, selection: $selection) {
                TableColumn("Name") { item in Text(item.name) }
                TableColumn("Users") { item in Text("\(item.users)") }
                TableColumn("Topic") { item in Text(item.topic) }
            }

            Divider()
            HStack {
                Spacer()
                Button("Go To") { goTo() }
                    .keyboardShortcut(.defaultAction)
                    .disabled(selection == nil)
            }
            .padding(8)
        }
        .frame(minWidth: 480, minHeight: 320)
        .task {
            // Populate immediately on open — mirrors the original's
            // roomlist.cpp behavior of issuing LIST as soon as the dialog
            // shows, rather than requiring an explicit first Refresh click.
            appState.requestRoomList()
        }
    }

    private func goTo() {
        guard let name = selection else { return }
        appState.goToRoom(name)
        dismiss()
    }
}
