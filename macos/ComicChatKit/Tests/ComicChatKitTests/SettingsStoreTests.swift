import Testing
import Foundation
@testable import ComicChatKit

// Plan 4a Task 9 Step 1: SettingsStore is a plain typed UserDefaults wrapper
// with no engine/`cc_*` involvement at all — outside the serialized
// EngineGlobalStateSelfTests tree (see StripTests.swift's doc comment for why
// that tree exists: process-global engine state with no internal locking;
// UserDefaults has none of that, so this suite is free to run in parallel
// with everything else).
@Suite
struct SettingsStoreTests {
    /// A fresh, isolated UserDefaults suite — `removePersistentDomain` first
    /// so no prior run's state leaks in. Swift Testing runs `@Test` functions
    /// in parallel by default, so the suite name is uniqued per call (a
    /// UUID suffix on the brief's "p4-test" base name) rather than shared
    /// across tests in this file -- two tests racing on the SAME suite name
    /// would otherwise stomp on each other's values (observed: the
    /// round-trip test's writes leaking into the defaults-table test).
    private func freshDefaults() -> UserDefaults {
        let suiteName = "p4-test-\(UUID().uuidString)"
        UserDefaults().removePersistentDomain(forName: suiteName)
        return UserDefaults(suiteName: suiteName)!
    }

    @Test func defaultsMatchTable() {
        let defaults = freshDefaults()
        let store = SettingsStore(defaults: defaults)
        #expect(store.server == "")
        #expect(store.port == 6667)
        #expect(store.room == "")
        #expect(store.encoding == .cp1252)
        #expect(store.nick == "")
        #expect(store.character == "anna")
        #expect(store.backdrop == "field")
    }

    @Test func roundTripsEachProperty() {
        let defaults = freshDefaults()
        let store = SettingsStore(defaults: defaults)

        store.server = "irc.example.com"
        store.port = 6668
        store.room = "#p4"
        store.encoding = .utf8
        store.nick = "Mac"
        store.character = "armando"
        store.backdrop = "clouds"

        #expect(store.server == "irc.example.com")
        #expect(store.port == 6668)
        #expect(store.room == "#p4")
        #expect(store.encoding == .utf8)
        #expect(store.nick == "Mac")
        #expect(store.character == "armando")
        #expect(store.backdrop == "clouds")

        // A second store instance over the SAME defaults sees the persisted
        // values -- proves round-tripping through UserDefaults, not just an
        // in-memory copy.
        let reloaded = SettingsStore(defaults: defaults)
        #expect(reloaded.server == "irc.example.com")
        #expect(reloaded.port == 6668)
        #expect(reloaded.room == "#p4")
        #expect(reloaded.encoding == .utf8)
        #expect(reloaded.nick == "Mac")
        #expect(reloaded.character == "armando")
        #expect(reloaded.backdrop == "clouds")
    }

    /// Quick-wins batch item 3: `panelsPerRow` defaults to 0 ("automatic")
    /// and round-trips through UserDefaults like every other property here.
    @Test func panelsPerRowDefaultsToAutomaticAndRoundTrips() {
        let defaults = freshDefaults()
        let store = SettingsStore(defaults: defaults)
        #expect(store.panelsPerRow == 0)

        store.panelsPerRow = 4
        #expect(store.panelsPerRow == 4)

        let reloaded = SettingsStore(defaults: defaults)
        #expect(reloaded.panelsPerRow == 4)
    }

    /// Batch B: `notificationsEnabled` defaults to `true` (opt-out, matching
    /// every other Advanced-tab protocol/art toggle) and round-trips.
    @Test func notificationsEnabledDefaultsToTrueAndRoundTrips() {
        let defaults = freshDefaults()
        let store = SettingsStore(defaults: defaults)
        #expect(store.notificationsEnabled == true)

        store.notificationsEnabled = false
        #expect(store.notificationsEnabled == false)

        let reloaded = SettingsStore(defaults: defaults)
        #expect(reloaded.notificationsEnabled == false)
    }

    /// Batch E: `comicFontFace`/`comicFontSize` default to "Comic Sans MS" /
    /// 0 (the "engine default" sentinel `cc_set_comic_font` treats as
    /// "leave unchanged") and round-trip.
    @Test func comicFontDefaultsAndRoundTrips() {
        let defaults = freshDefaults()
        let store = SettingsStore(defaults: defaults)
        #expect(store.comicFontFace == "Comic Sans MS")
        #expect(store.comicFontSize == 0)

        store.comicFontFace = "Chalkboard SE"
        store.comicFontSize = 18
        #expect(store.comicFontFace == "Chalkboard SE")
        #expect(store.comicFontSize == 18)

        let reloaded = SettingsStore(defaults: defaults)
        #expect(reloaded.comicFontFace == "Chalkboard SE")
        #expect(reloaded.comicFontSize == 18)
    }

    /// Batch E: `favorites` defaults to empty and round-trips a list of
    /// `FavoriteConnection` through JSON `Data`.
    @Test func favoritesDefaultsToEmptyAndRoundTrips() {
        let defaults = freshDefaults()
        let store = SettingsStore(defaults: defaults)
        #expect(store.favorites == [])

        let a = FavoriteConnection(name: "The Crypt", host: "www.crypthome.com", port: 6667, room: "#Crypt")
        let b = FavoriteConnection(name: "Home rig", host: "127.0.0.1", port: 6668, room: "#comicrig")
        store.favorites = [a, b]
        #expect(store.favorites == [a, b])

        let reloaded = SettingsStore(defaults: defaults)
        #expect(reloaded.favorites == [a, b])
    }
}
