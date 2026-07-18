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
