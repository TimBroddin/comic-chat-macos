import Testing
import Foundation
import CoreGraphics
import cchat_engine
@testable import ComicChatKit

// Plan 4b Task 5 — Settings scene + persona plumbing + character/backdrop
// pickers. Three things this suite proves:
//   1. SettingsStore round-trips every NEW key (realName/sendComicsData/
//      acceptWhispers/soundsEnabled/soundsFolder/comicMode/autoDownloadAvatars)
//      against an isolated UserDefaults suite, including "never set" defaults
//      (same `object(forKey:)` discrimination `port` already uses).
//   2. `ProtocolSession.init(..., userName:, realName:)` actually reaches the
//      wire: a loopback login test asserts the exact
//      "USER timb <host> . :Tim B" line (cc_session_login's grammar,
//      comicchat.h:688-708) — the real gap this task closes (ProtocolSession
//      never set cc_session_config.own_user/.own_realname before this task,
//      so USER always fell back to the nick).
//   3. `ChatSessionModel.changeCharacter` mid-session: after login/join + one
//      line, switch to a second bundled character, send another line, and
//      assert no crash, panelCount grew, and the announce ("# Appears as")
//      landed in the server-received bytes.
//
// CONFIG-STRING LIFETIME (the brief's read-first mandate): `cc_session_create`
// (bridge/cc_session.cpp:41-46) does `s->cfg = *cfg` — a STRUCT copy that
// copies the `const char*` POINTER VALUES, not the pointee bytes. `own_user`/
// `own_realname`/`local_host` are read later still, inside `cc_session_login`
// (cc_session.cpp:399-410) — which can fire an unbounded amount of async time
// after `cc_session_create` returns (the probe -> 451/800-pivot/timeout
// handshake), and can even be RE-entered (see that function's "REENTRANCY"
// doc comment). So a plain Swift local's `withCString` pointer (valid only
// for that closure's duration) does NOT suffice — this mirrors
// `ProtocolSession`'s existing `ownNickStorage`/`refillOwnNickBuffer()`
// pattern: a manually-`allocate`d, NUL-terminated buffer with a fixed address
// for the session's whole lifetime, freed only in `deinit`.
//
// SERIALIZATION: nested inside `EngineGlobalStateSelfTests` (.serialized),
// matching every other suite touching process-global engine state via
// ProtocolSession/ChatSessionModel.
extension EngineGlobalStateSelfTests {
    @Suite(.serialized)
    struct PersonaSettingsTests {
        // MARK: - Step 1a: SettingsStore round-trip (new keys)

        private func freshDefaults() -> UserDefaults {
            let suiteName = "p4b-t5-test-\(UUID().uuidString)"
            UserDefaults().removePersistentDomain(forName: suiteName)
            return UserDefaults(suiteName: suiteName)!
        }

        @Test func newKeysNeverSetDefaults() {
            let defaults = freshDefaults()
            let store = SettingsStore(defaults: defaults)
            #expect(store.realName == "")
            #expect(store.sendComicsData == true)
            #expect(store.acceptWhispers == true)
            #expect(store.soundsEnabled == true)
            #expect(!store.soundsFolder.isEmpty, "soundsFolder must default to the App Support path, not empty")
            #expect(store.comicMode == true)
            #expect(store.autoDownloadAvatars == true)
        }

        @Test func newKeysRoundTrip() {
            let defaults = freshDefaults()
            let store = SettingsStore(defaults: defaults)

            store.realName = "Tim B"
            store.sendComicsData = false
            store.acceptWhispers = false
            store.soundsEnabled = false
            store.soundsFolder = "/tmp/sounds"
            store.comicMode = false
            store.autoDownloadAvatars = false

            #expect(store.realName == "Tim B")
            #expect(store.sendComicsData == false)
            #expect(store.acceptWhispers == false)
            #expect(store.soundsEnabled == false)
            #expect(store.soundsFolder == "/tmp/sounds")
            #expect(store.comicMode == false)
            #expect(store.autoDownloadAvatars == false)

            // A second store instance over the SAME defaults sees the
            // persisted values -- proves round-tripping through UserDefaults,
            // not just an in-memory copy (mirrors SettingsStoreTests'
            // existing `reloaded` check).
            let reloaded = SettingsStore(defaults: defaults)
            #expect(reloaded.realName == "Tim B")
            #expect(reloaded.sendComicsData == false)
            #expect(reloaded.acceptWhispers == false)
            #expect(reloaded.soundsEnabled == false)
            #expect(reloaded.soundsFolder == "/tmp/sounds")
            #expect(reloaded.comicMode == false)
            #expect(reloaded.autoDownloadAvatars == false)
        }

        // MARK: - Step 1b: persona wire test (USER line)

        /// Polls `server.receivedBytes` until it contains `substring`.
        private func waitForReceivedBytes(
            _ server: LoopbackIRCServer,
            containing substring: String
        ) async throws -> String {
            while true {
                let text = String(data: server.receivedBytes, encoding: .isoLatin1) ?? ""
                if text.contains(substring) { return text }
                try await Task.sleep(nanoseconds: 5_000_000)
            }
        }

        /// Real gap this task closes: `ProtocolSession` never plumbed
        /// `userName`/`realName` into `cc_session_config.own_user`/
        /// `.own_realname` before this task -- USER always fell back to the
        /// nick. This drives a real login handshake (plain-IRC 451 fallback,
        /// same scenario as LoginSequencingTests.plainIrcFallback) with
        /// `userName: "timb", realName: "Tim B"` and asserts the exact wire
        /// grammar `cc_session_login` builds: "USER <user> <host> . :<realname>"
        /// (comicchat.h:688-708, verified byte-for-byte against the real 1998
        /// capture's c2s line in that function's doc comment). `<host>`
        /// itself is whatever `local_host` resolves to (today `nil` ->
        /// "localhost", cc_session.cpp:407) -- not asserted here since this
        /// task does not plumb a host override, only user/realname.
        @Test(.timeLimit(.minutes(1)))
        func personaUserAndRealNameReachTheWire() async throws {
            let server = try LoopbackIRCServer()
            let session = ProtocolSession(host: "127.0.0.1", port: server.port, nick: "timb",
                                          encoding: .cp1252, userName: "timb", realName: "Tim B")
            var iterator = session.events.makeAsyncIterator()

            try await session.connect()
            await server.startCollectingReceivedBytes()
            _ = try await waitForReceivedBytes(server, containing: "MODE ISIRCX\r\n")
            try await server.send(":srv 451 * :not registered")

            let afterLogin = try await waitForReceivedBytes(server, containing: "USER ")
            #expect(afterLogin.contains("USER timb localhost . :Tim B\r\n"),
                    "expected the exact USER grammar with plumbed user/realname; got: \(afterLogin)")

            try await server.send(":srv 001 timb :Welcome")
            while true {
                guard let scoped = await iterator.next() else { break }
                if case .loggedIn = scoped.event { break }   // Plan 4b Task 7: unwrap
            }

            session.disconnect()
            server.stop()
        }

        // MARK: - Step 1c: changeCharacter mid-session

        /// Polls `server.receivedBytes` until it contains `substring`,
        /// returning the accumulated c2s bytes split into lines -- same shape
        /// as ChatSessionModelTests.waitForReceivedLine.
        private func waitForReceivedLine(
            _ server: LoopbackIRCServer,
            containing substring: String
        ) async throws -> [String] {
            while true {
                let text = String(data: server.receivedBytes, encoding: .isoLatin1) ?? ""
                if text.contains(substring) {
                    return text.components(separatedBy: "\r\n").filter { !$0.isEmpty }
                }
                try await Task.sleep(nanoseconds: 5_000_000)
            }
        }

        /// After login/join + one line, switch the SELF character to a
        /// second bundled avatar (armando), send another line, and assert:
        /// no crash, `panelCount` grew (a new panel was actually rendered
        /// after the switch), and the announce-on-change
        /// (`session.announceAvatar`, mirroring SetMyAvatar's
        /// announce-on-change, avatar.cpp:585-599) reached the server as a
        /// "# Appears as Armando" PRIVMSG.
        @Test(.timeLimit(.minutes(1)))
        func changeCharacterMidSessionSwitchesAvatarAndAnnounces() async throws {
            let server = try LoopbackIRCServer()
            let art = repoRoot5Up().appendingPathComponent("v2.5-beta-1-modern/comicart").path
            let model = ChatSessionModel(config: .init(host: "127.0.0.1", port: server.port,
                                                       nick: "Mac", room: "#p4", artDir: art))
            try await model.start()
            try await server.replyToProbeWith451ThenWelcomeAndJoin(nick: "Mac", channel: "#p4")

            try await model.send("first line")
            _ = try await waitForReceivedLine(server, containing: "first line")

            let panelsBeforeSwitch = model.panelCount

            model.changeCharacter("armando")

            // settle the engine-queue hop `changeCharacter` fires (mirrors
            // `settleEngineQueue`'s existing sentinel-hop technique).
            await withCheckedContinuation { (cont: CheckedContinuation<Void, Never>) in
                model.settleEngineQueue { cont.resume() }
            }

            try await model.send("second line")
            let afterSwitch = try await waitForReceivedLine(server, containing: "second line")

            #expect(afterSwitch.contains { $0.contains("# Appears as Armando") },
                    "expected the announce-on-change to reach the wire; got: \(afterSwitch)")
            #expect(model.panelCount > panelsBeforeSwitch,
                    "expected a new panel to have been rendered after the character switch + second send")

            model.shutdown()
            server.stop()
        }

        // MARK: - Live-fix 3: a character switch must change what renders

        /// Plan 4b live-fix 3 (Tim's live report: "selecting a character from
        /// Preferences still keeps rendering Anna"). The EXISTING
        /// `changeCharacterMidSessionSwitchesAvatarAndAnnounces` proves only
        /// that a new panel RENDERED and the announce reached the wire — NOT
        /// that the new panel wears the NEW avatar. And
        /// `characterSwitchSurvivesReflowAtOriginalGeometry` compares a strip
        /// to ITSELF across a reflow, so it stays green even when BOTH the live
        /// and reflow renders share the same wrong avatar. Neither catches what
        /// Tim hit: `changeCharacter` had ZERO effect on the composed strip —
        /// post-switch panels (and the starring icon) kept rendering the
        /// STARTING character, because the strip's line ingestion
        /// (`cc_strip_add_line{,_cooked}`) keyed the avatar off the raw
        /// participant id instead of the participant's CURRENT avatar id
        /// (`pui->GetAvatarID()`, the original's `histent.cpp:108` contract) —
        /// so after `cc_strip_set_participant_avatar` moved the CUserInfo to a
        /// new avatar id, every subsequent line still cloned the OLD avatar.
        ///
        /// OBSERVABLE (differential, no cross-run/panel-merge confound): switch
        /// BEFORE any chat line, then send one line, and compare the composed
        /// strip to a baseline that NEVER switched (same start character, same
        /// one line). A switch to a DIFFERENT character MUST change the pixels
        /// (the panel now wears the new avatar); a switch to the SAME character
        /// MUST be a byte-identical no-op. Pre-fix, the different-character
        /// switch produced a BYTE-IDENTICAL strip to the never-switched
        /// baseline (the switch was ignored) — the exact RED this closes.
        /// Switching before any line keeps the whole strip attributable to the
        /// switch (no legitimately-differing pre-switch panel to muddy the
        /// compare, and no post-switch same-participant panel-merge nuance).
        @Test(.timeLimit(.minutes(1)))
        func characterSwitchChangesTheComposedStrip() async throws {
            let art = repoRoot5Up().appendingPathComponent("v2.5-beta-1-modern/comicart").path

            // Runs one session: login/join at geometry G (600pt), optionally
            // `changeCharacter(switchTo)` BEFORE any line, send one line, return
            // the composed strip bytes.
            func run(start: String, switchTo: String?) async throws -> Data {
                let box = LatestImageBoxLocal()
                let server = try LoopbackIRCServer()
                let model = ChatSessionModel(config: .init(host: "127.0.0.1", port: server.port,
                                                           nick: "Mac", room: "#p4",
                                                           characterName: start, artDir: art))
                model.onStripImage = { image, _ in box.set(image) }
                func settle() async throws {
                    await withCheckedContinuation { (c: CheckedContinuation<Void, Never>) in
                        model.settleEngineQueue { c.resume() }
                    }
                    try await Task.sleep(nanoseconds: 100_000_000)
                }
                try await model.start()
                try await server.replyToProbeWith451ThenWelcomeAndJoin(nick: "Mac", channel: "#p4")
                model.setViewport(widthPoints: 600, scale: 2.0)
                try await settle()
                if let switchTo {
                    model.changeCharacter(switchTo)
                    try await settle()
                }
                try await model.send("only line")
                _ = try await waitForReceivedLine(server, containing: "only line")
                try await settle()
                defer { model.shutdown(); server.stop() }
                guard let img = box.get() else {
                    Issue.record("no composed strip image captured")
                    return Data()
                }
                return try pixelBytesLocal(img)
            }

            let baseline = try await run(start: "anna", switchTo: nil)      // never switched
            let switchedDifferent = try await run(start: "anna", switchTo: "susan")
            let switchedSame = try await run(start: "susan", switchTo: "susan")
            let plainSusan = try await run(start: "susan", switchTo: nil)

            // Precomputed Bools (NOT `#expect(a == b)` on the raw Data) — a
            // failing large-`Data` comparison triggers Swift Testing's
            // catastrophically slow Myers-diff, as documented on
            // `characterSwitchSurvivesReflowAtOriginalGeometry`.
            let switchTookEffect = baseline != switchedDifferent
            #expect(switchTookEffect,
                    "a switch to a DIFFERENT character must change the composed strip; a byte-identical result means changeCharacter was IGNORED (the reported bug: still renders the starting character). baseline.count=\(baseline.count) switched.count=\(switchedDifferent.count)")

            // A same-character switch is a no-op: its render must match a plain
            // never-switched susan session byte-for-byte (proves the switch
            // path itself doesn't perturb rendering — it only re-avatars).
            let sameSwitchIsNoOp = switchedSame == plainSusan
            #expect(sameSwitchIsNoOp,
                    "a switch to the SAME character must render byte-identically to a never-switched session of that character. switchedSame.count=\(switchedSame.count) plainSusan.count=\(plainSusan.count)")
        }

        // MARK: - Live-fix 4: wheel self-pose preview must track the switch

        /// Plan 4b live-fix 4 (Tim's live report: the emotion-wheel center pose
        /// preview showed a wrong/garbage pose — a head icon, not a full-body
        /// pose — after live-fix 2/3 landed). Bisected at the KIT boundary
        /// (a connected model → `setEmotion` → `onSelfPose`): the Kit fires a
        /// non-nil image in BOTH cases, so a bare non-nil check is too weak. The
        /// real defect is that after `changeCharacter`, the Task-2 self APIs
        /// (`cc_strip_self_pose`/`set_self_emotion`/`preview_self_text`/
        /// `self_annotations`) resolved the SELF avatar via
        /// `GetAvatar(selfParticipant)` — the raw PARTICIPANT id — but
        /// `cc_strip_set_participant_avatar` re-pointed the self CUserInfo at a
        /// freshly-loaded avatar under a NEW avatar id (the same
        /// participant-id-vs-avatar-id conflation live-fix 3 fixed for line
        /// ingestion). So `selfPoseIndex()` read the OLD avatar's poseID, which,
        /// fed into the NEW character's `.avb` (`selfAvatarFile`, reopened on the
        /// switch), landed on a mismatched pose (a head icon) instead of the
        /// intended full-body pose. Fix: every Task-2 self API resolves the
        /// CURRENT avatar id (`pui->GetAvatarID()`) via `selfAvatarID()`.
        ///
        /// OBSERVABLE (differential): a session that switches anna→susan then
        /// drives the wheel must produce the SAME self-pose image as a control
        /// that STARTED as susan and drove the wheel identically. Pre-fix the
        /// switched session produced the stale-avatar pose (different bytes,
        /// even a different SIZE — a head icon vs a full body); post-fix they
        /// match byte-for-byte. A guard scenario (fresh anna, no switch) pins
        /// that the common case still fires a non-nil pose.
        @Test(.timeLimit(.minutes(1)))
        func wheelSelfPoseFollowsCharacterSwitch() async throws {
            let art = repoRoot5Up().appendingPathComponent("v2.5-beta-1-modern/comicart").path

            // Drives one model to a connected+joined state, optionally switches
            // character, then fires setEmotion and returns the last self-pose
            // image's pixel bytes (nil if none fired).
            func selfPose(start: String, switchTo: String?) async throws -> Data? {
                let box = SelfPoseBox()
                let server = try LoopbackIRCServer()
                let model = ChatSessionModel(config: .init(host: "127.0.0.1", port: server.port,
                                                           nick: "Mac", room: "#p4",
                                                           characterName: start, artDir: art))
                model.onSelfPose = { img in box.set(img) }
                func settle() async throws {
                    await withCheckedContinuation { (c: CheckedContinuation<Void, Never>) in
                        model.settleEngineQueue { c.resume() }
                    }
                    try await Task.sleep(nanoseconds: 100_000_000)
                }
                try await model.start()
                try await server.replyToProbeWith451ThenWelcomeAndJoin(nick: "Mac", channel: "#p4")
                try await settle()
                if let switchTo { model.changeCharacter(switchTo); try await settle() }
                model.setEmotion(angle: 0.0, intensity: 1.0)
                try await settle()
                defer { model.shutdown(); server.stop() }
                guard let img = box.lastImage() else { return nil }
                return try pixelBytesLocal(img)
            }

            // Guard: the common (fresh, never-switched) case still fires a pose.
            let fresh = try await selfPose(start: "anna", switchTo: nil)
            #expect(fresh != nil,
                    "a fresh connected model's setEmotion must fire onSelfPose with a non-nil pose image")

            // The fix: a switched anna->susan self-pose must equal a control
            // that started as susan (both drove the wheel identically) — proves
            // the wheel preview tracks the CURRENT avatar, not the stale one.
            let switchedToSusan = try await selfPose(start: "anna", switchTo: "susan")
            let plainSusan = try await selfPose(start: "susan", switchTo: nil)
            #expect(switchedToSusan != nil && plainSusan != nil,
                    "both switched and control self-pose images must be non-nil")
            let poseMatches = switchedToSusan == plainSusan
            #expect(poseMatches,
                    "after a switch to susan, the wheel self-pose must match a susan-from-start control; a mismatch means the Task-2 self APIs read the STALE participant avatar (the reported wrong-pose bug). switched.count=\(switchedToSusan?.count ?? -1) control.count=\(plainSusan?.count ?? -1)")
        }
    }
}

/// Live-fix 4: thread-safe single-slot box for the last `onSelfPose` image.
/// `onSelfPose` is a `@Sendable` main-thread callback read from the test's own
/// task, same lock-guarded shape as `LatestImageBoxLocal`.
private final class SelfPoseBox: @unchecked Sendable {
    private let lock = NSLock()
    private var last: CGImage?
    func set(_ image: CGImage?) {
        lock.lock(); defer { lock.unlock() }
        if let image { last = image }
    }
    func lastImage() -> CGImage? { lock.lock(); defer { lock.unlock() }; return last }
}

/// Live-fix 3: local single-slot image box + full-image pixel readback,
/// duplicated from `ChatSessionModelTests`' private `LatestImageBox`/
/// `pixelBytes` (both `private` to that file) so this suite's LIVE-vs-reflow
/// byte compare has the same thread-safe capture + full-image readback without
/// widening those helpers' access across files.
private final class LatestImageBoxLocal: @unchecked Sendable {
    private let lock = NSLock()
    private var image: CGImage?
    func set(_ image: CGImage) { lock.lock(); defer { lock.unlock() }; self.image = image }
    func get() -> CGImage? { lock.lock(); defer { lock.unlock() }; return image }
}

private func pixelBytesLocal(_ image: CGImage) throws -> Data {
    let width = image.width, height = image.height
    var buffer = [UInt8](repeating: 0, count: width * height * 4)
    let colorSpace = CGColorSpace(name: CGColorSpace.sRGB)!
    guard let ctx = CGContext(
        data: &buffer, width: width, height: height,
        bitsPerComponent: 8, bytesPerRow: width * 4, space: colorSpace,
        bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue) else {
        throw Strip.StripError(message: "pixelBytesLocal readback context failed")
    }
    ctx.draw(image, in: CGRect(x: 0, y: 0, width: width, height: height))
    return Data(buffer)
}
