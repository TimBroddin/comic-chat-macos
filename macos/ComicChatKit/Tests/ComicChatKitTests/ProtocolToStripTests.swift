import Testing
import Foundation
import CoreGraphics
import cchat_engine
@testable import ComicChatKit

// Plan 3 Task 9 — THE EXIT MILESTONE: prove the two halves of the port
// connect. A stream of `ProtocolEvent` (Task 7's protocol layer) drives
// `ProtocolStripBridge`, which drives the EXISTING `cc_strip` compositor
// (Plan 2) to a rendered comic panel. Wire in, comic out.
//
// SERIALIZATION (load-bearing, same reasoning as StripTests/StripScriptTests/
// ProtocolSessionTests): `cc_strip_*` drives process-global engine state (the
// avatar registry, the session user table, the font statics, the metrics
// canvas) with no internal locking, and `cc_session_*` shares that SAME
// process-global engine per comicchat.h's single-thread contract. Nested
// inside EngineGlobalStateSelfTests (.serialized) so every engine-global-state
// test -- C selftests, Strip tests, ProtocolSession tests, and these -- runs
// on one single timeline.

// Repo root from this source file's location: same 5-deletion walk
// GoldenCatalogTests.swift/StripTests.swift already use (ProtocolToStripTests.swift
// -> ComicChatKitTests -> Tests -> ComicChatKit -> macos -> repoRoot).
func repoRoot5Up() -> URL {
    URL(fileURLWithPath: #filePath)
        .deletingLastPathComponent().deletingLastPathComponent()
        .deletingLastPathComponent().deletingLastPathComponent()
        .deletingLastPathComponent()
}

extension EngineGlobalStateSelfTests {
  @Suite(.serialized)
  struct ProtocolToStripTests {
    // Two participants (Anna, Bob), Anna's .userJoined + Bob's .userJoined,
    // then Bob sends one line carrying a decoded Annotations block (the exact
    // vector ProtocolSessionTests/ProtocolCodecTests already exercise via the
    // real "(#G295E193M1) hello" wire grammar: gesturePose=2, gestureEmotion=9,
    // gestureIntensity=5, facePose=1, faceEmotion=9, faceIntensity=3, mode=1
    // (SM_SAY), cooked=true), then a plain unannotated line from Anna (proves
    // the has_annotations=false fallback still renders a plain "say").
    private func annotatedEventStream() -> [ProtocolEvent] {
        let annotations = Annotations(
            gesturePose: 2, gestureEmotion: 9, gestureIntensity: 5,
            facePose: 1, faceEmotion: 9, faceIntensity: 3,
            requested: false, mode: 1, addressees: [], cooked: true)
        return [
            .userJoined(nick: "Anna", ident: "anna@host"),
            .userJoined(nick: "Bob", ident: "bob@host"),
            .text(nick: "Bob", ident: "bob@host", target: "#comicrig",
                  text: "hello", kind: 1, annotations: annotations),
            .text(nick: "Anna", ident: "anna@host", target: "#comicrig",
                  text: "hi there", kind: 1, annotations: nil),
        ]
    }

    // (a) THE RECORDING-CANVAS ASSERTION: compose the bridged strip onto a
    // RecordingCanvas and assert the draw-call log contains a balloon
    // (drawText) for each participant's message text -- reusing the exact
    // recording-canvas grammar (text/rect/image/path/clip lines) StripTests
    // already established as the stable contract.
    @Test func bridgedEventStreamProducesExpectedDrawCalls() throws {
        let metricsCanvas = RecordingCanvas()
        let metricsBox = CanvasBox(metricsCanvas)
        cc_set_metrics_canvas(metricsBox.handle)

        try withExtendedLifetime(metricsBox) {
            let resolver = ProtocolStripBridge.AvatarResolver(
                defaultOrder: [fixture("anna.avb")])
            let bridge = try ProtocolStripBridge(resolver: resolver)
            try bridge.setBackdrop(fixture("field.bgb"))

            try bridge.apply(annotatedEventStream())

            #expect(bridge.participantOrder == ["Anna", "Bob"])
            #expect(bridge.panelCount > 0)

            let recorder = RecordingCanvas()
            try bridge.compose(onto: recorder)

            // Every text draw call embeds its literal message text between
            // quotes (RecordingCanvas.drawText's log line format: `text X,Y
            // color=RRGGBB "the text"`) -- assert both balloons' text appear
            // somewhere in the draw-call stream. The engine's panel layout
            // uppercases comic balloon text (CUnitPanelPage's own original
            // behavior, unrelated to this bridge), so match case-insensitively.
            let textLines = recorder.log.filter { $0.hasPrefix("text ") }
            #expect(textLines.contains { $0.uppercased().contains("\"HELLO\"") },
                "expected a balloon draw call containing Bob's message \"hello\"; draw calls: \(textLines)")
            #expect(textLines.contains { $0.uppercased().contains("HI THERE") },
                "expected a balloon draw call containing Anna's message \"hi there\"; draw calls: \(textLines)")

            // Sanity: something actually drew (avatar art, panel border,
            // balloon path) beyond just text -- proves this is a real
            // composed page, not an empty one.
            #expect(recorder.log.contains { $0.hasPrefix("image ") })
            #expect(recorder.log.contains { $0.hasPrefix("path ") })
        }
    }

    // (b) An unannotated .text (has_annotations == false) must still render
    // as a plain "say" line -- not be dropped or throw.
    @Test func unannotatedTextStillRendersAsPlainSay() throws {
        let metricsCanvas = RecordingCanvas()
        let metricsBox = CanvasBox(metricsCanvas)
        cc_set_metrics_canvas(metricsBox.handle)

        try withExtendedLifetime(metricsBox) {
            let resolver = ProtocolStripBridge.AvatarResolver(
                defaultOrder: [fixture("anna.avb")])
            let bridge = try ProtocolStripBridge(resolver: resolver)

            try bridge.apply([
                .userJoined(nick: "Anna", ident: "anna@host"),
                .text(nick: "Anna", ident: "anna@host", target: "#comicrig",
                      text: "just a plain line", kind: 1, annotations: nil),
            ])

            #expect(bridge.panelCount > 0)
            let recorder = RecordingCanvas()
            try bridge.compose(onto: recorder)
            // Balloon text can wrap across multiple drawText calls at word
            // boundaries (this line's "JUST A PLAIN" / "LINE" split across
            // two lines observed in practice) -- check for a substring that
            // survives wrapping rather than the whole phrase in one call.
            #expect(recorder.log.contains { $0.uppercased().contains("JUST A PLAIN") })
        }
    }

    // (c) A participant introduced only via .text (never a .userJoined) still
    // gets a participant id lazily -- the bridge doesn't require membership
    // events to precede messages (a real capture can arrive in any order the
    // engine itself permits).
    @Test func lazyParticipantFromTextAlone() throws {
        let metricsCanvas = RecordingCanvas()
        let metricsBox = CanvasBox(metricsCanvas)
        cc_set_metrics_canvas(metricsBox.handle)

        try withExtendedLifetime(metricsBox) {
            let resolver = ProtocolStripBridge.AvatarResolver(
                defaultOrder: [fixture("anna.avb")])
            let bridge = try ProtocolStripBridge(resolver: resolver)

            try bridge.apply([
                .text(nick: "Solo", ident: "solo@host", target: "#comicrig",
                      text: "nobody announced me", kind: 1, annotations: nil),
            ])

            #expect(bridge.participantOrder == ["Solo"])
        }
    }

    // (c2) Plan 4a Task 6: an `.appearsAs` for an EXISTING participant now
    // switches that participant's avatar (cc_strip_set_participant_avatar),
    // rather than the old documented no-op. Sequence: .userJoined("Win")
    // (creates the participant with the resolver's default avatar) + one
    // .text (so a panel is laid out with the OLD avatar), then .appearsAs
    // switching "Win" to "armando", then another .text. Asserts no throw,
    // announcedAvatarNames reflects the switch, and the strip still composes
    // to a non-empty render afterward (proving the switch didn't corrupt the
    // strip/registry).
    @Test func appearsAsSwitchesExistingParticipantAvatar() throws {
        let metricsCanvas = RecordingCanvas()
        let metricsBox = CanvasBox(metricsCanvas)
        cc_set_metrics_canvas(metricsBox.handle)

        try withExtendedLifetime(metricsBox) {
            // comicartDir set to the Fixtures directory so the "armando"
            // .appearsAs below resolves to the bundled armando.avb fixture
            // (added alongside anna.avb for this task) rather than a bare
            // "armando.avb" relative to the test process's CWD.
            let fixturesDir = (fixture("anna.avb") as NSString).deletingLastPathComponent
            let resolver = ProtocolStripBridge.AvatarResolver(
                comicartDir: fixturesDir, defaultOrder: [fixture("anna.avb")])
            let bridge = try ProtocolStripBridge(resolver: resolver)

            try bridge.apply([
                .userJoined(nick: "Win", ident: "win@host"),
                .text(nick: "Win", ident: "win@host", target: "#comicrig",
                      text: "before the switch", kind: 1, annotations: nil),
            ])
            #expect(bridge.participantOrder == ["Win"])

            try bridge.apply(.appearsAs(nick: "Win", avatarName: "armando", url: ""))
            #expect(bridge.announcedAvatarNames["Win"] == "armando")

            try bridge.apply([
                .text(nick: "Win", ident: "win@host", target: "#comicrig",
                      text: "after the switch", kind: 1, annotations: nil),
            ])

            let recorder = RecordingCanvas()
            try bridge.compose(onto: recorder)
            #expect(!recorder.log.isEmpty)
            #expect(recorder.log.contains { $0.hasPrefix("image ") })
        }
    }

    // (c3) Plan 4b live-fix (p4b-a-mac capture): an `.appearsAs` announcing an
    // UNRESOLVABLE avatar name — the `_NoArt` sentinel a peer with no avatar
    // art of its own sends (avatar.cpp:686 `GetNextAvatarName` -> "_NoArt"
    // when the peer's own avatar-name list is empty) — must NOT poison that
    // peer. In the original, `ChangeAvatarEntry::Execute` (histent.cpp:384)
    // resolves it through `GetAvatar3(name, pui, bRandomIfNotFound=TRUE)`,
    // whose `LoadAvatar` miss cycles a DEFAULT avatar (avatar.cpp:699-704) —
    // an unknown/unresolvable announced name is NEVER an error there.
    //
    // The live symptom: after Anonymous announced `_NoArt` (privately),
    // EVERY subsequent event that needed Anonymous as a strip participant —
    // our own T-list say addressing them, and their own bare inbound says —
    // threw inside `ensureParticipant` (resolver produced literal `_NoArt.avb`
    // -> `cc_strip_add_participant`'s `LoadAvatar` fails -> -1 -> throw) and
    // was swallowed by `handleLocked`'s `try? bridge?.apply`, freezing the
    // strip. This asserts the addressee path heals: a not-yet-participant
    // addressee whose only announced name is unresolvable still becomes a
    // participant (cycling the resolver default) and the line renders.
    @Test func unresolvableAnnouncedAddresseeStillRendersAndParticipates() throws {
        let metricsCanvas = RecordingCanvas()
        let metricsBox = CanvasBox(metricsCanvas)
        cc_set_metrics_canvas(metricsBox.handle)

        try withExtendedLifetime(metricsBox) {
            let fixturesDir = (fixture("anna.avb") as NSString).deletingLastPathComponent
            let resolver = ProtocolStripBridge.AvatarResolver(
                comicartDir: fixturesDir, defaultOrder: [fixture("anna.avb")])
            let bridge = try ProtocolStripBridge(resolver: resolver)

            // Self speaks first (becomes a participant), like the Mac app's
            // opening "hi".
            try bridge.apply(.text(nick: "Tim", ident: "tim@host", target: "#comicrig",
                                   text: "hi", kind: 1, annotations: nil))
            #expect(bridge.participantOrder == ["Tim"])

            // Anonymous is announced PRIVATELY with the `_NoArt` sentinel —
            // stashed in announcedAvatarNames but not yet a participant (never
            // spoke). This alone must not throw.
            try bridge.apply(.appearsAs(nick: "Anonymous", avatarName: "_NoArt", url: ""))
            #expect(bridge.announcedAvatarNames["Anonymous"] == "_NoArt")
            #expect(bridge.participantIDs["Anonymous"] == nil)

            // Our own say addressing Anonymous (Task 8 member-list selection
            // as addressee) — this resolves Anonymous as an addressee, which
            // must ensureParticipant despite the unresolvable announced name.
            let addressed = Annotations(
                gesturePose: 2, gestureEmotion: 9, gestureIntensity: 5,
                facePose: 1, faceEmotion: 9, faceIntensity: 3,
                requested: false, mode: 1, addressees: ["Anonymous"], cooked: true)
            let before = bridge.panelCount
            try bridge.apply(.text(nick: "Tim", ident: "tim@host", target: "#comicrig",
                                   text: "test", kind: 1, annotations: addressed))
            // Anonymous is now a participant (cycled the resolver default) and
            // the addressed line laid out a panel.
            #expect(bridge.participantIDs["Anonymous"] != nil)
            #expect(bridge.panelCount > before)

            // And Anonymous's own subsequent BARE inbound say renders too.
            let before2 = bridge.panelCount
            try bridge.apply(.text(nick: "Anonymous", ident: "anon@host", target: "#comicrig",
                                   text: "lol", kind: 1, annotations: nil))
            #expect(bridge.panelCount > before2)

            let recorder = RecordingCanvas()
            try bridge.compose(onto: recorder)
            #expect(!recorder.log.isEmpty)
        }
    }

    // (c4) Companion to (c3) for the OTHER order seen in the capture: the peer
    // announces `_NoArt` privately, then speaks a bare line themselves BEFORE
    // we ever address them. The bare say must ensureParticipant + render
    // instead of throwing on the unresolvable stashed name.
    @Test func unresolvableAnnouncedSpeakerStillRenders() throws {
        let metricsCanvas = RecordingCanvas()
        let metricsBox = CanvasBox(metricsCanvas)
        cc_set_metrics_canvas(metricsBox.handle)

        try withExtendedLifetime(metricsBox) {
            let fixturesDir = (fixture("anna.avb") as NSString).deletingLastPathComponent
            let resolver = ProtocolStripBridge.AvatarResolver(
                comicartDir: fixturesDir, defaultOrder: [fixture("anna.avb")])
            let bridge = try ProtocolStripBridge(resolver: resolver)

            try bridge.apply(.appearsAs(nick: "Anonymous", avatarName: "_NoArt", url: ""))
            let before = bridge.panelCount
            try bridge.apply(.text(nick: "Anonymous", ident: "anon@host", target: "#comicrig",
                                   text: "dddd", kind: 1, annotations: nil))
            #expect(bridge.participantIDs["Anonymous"] != nil)
            #expect(bridge.panelCount > before)
        }
    }

    // (d) THE EXIT-MILESTONE PNG: the same annotated event stream composited
    // through CGCanvas into real pixels -- the wire-fed equivalent of Plan 2's
    // stripPNG exit proof (StripTests.swift). Written to
    // .superpowers/sdd/p3-exit.png for human visual verification.
    @Test func exitMilestonePNG() throws {
        let metricsCanvas = RecordingCanvas()
        let metricsBox = CanvasBox(metricsCanvas)
        cc_set_metrics_canvas(metricsBox.handle)

        try withExtendedLifetime(metricsBox) {
            // Prefer the bundled comicart set (two visually distinct
            // avatars: Anna, Armando) for the exit-milestone PNG so the two
            // speakers are easy to tell apart at a glance; fall back to the
            // test fixture avatar (used twice) if comicart isn't checked out
            // at the expected repo-relative path (e.g. a partial checkout).
            let comicart = repoRoot5Up()
                .appendingPathComponent("v2.5-beta-1-modern/comicart")
            let anna = comicart.appendingPathComponent("anna.avb").path
            let armando = comicart.appendingPathComponent("armando.avb").path
            let defaults = FileManager.default.fileExists(atPath: anna)
                && FileManager.default.fileExists(atPath: armando)
                ? [anna, armando] : [fixture("anna.avb"), fixture("anna.avb")]
            let resolver = ProtocolStripBridge.AvatarResolver(defaultOrder: defaults)
            let bridge = try ProtocolStripBridge(resolver: resolver)
            try bridge.setBackdrop(fixture("field.bgb"))

            try bridge.apply(annotatedEventStream())

            let (w, h) = bridge.size
            #expect(w > 0 && h > 0)

            let scale: CGFloat = 2.0
            let canvas = CGCanvas(widthTwips: w, heightTwips: h, scale: scale)
            try bridge.compose(onto: canvas)

            guard let png = canvas.pngData() else {
                Issue.record("pngData() returned nil")
                return
            }
            #expect(!png.isEmpty)
            #expect(png.starts(with: [0x89, 0x50, 0x4E, 0x47]))

            let outURL = repoRoot5Up()
                .appendingPathComponent(".superpowers/sdd/p3-exit.png")
            try png.write(to: outURL)
        }
    }
  }
}
