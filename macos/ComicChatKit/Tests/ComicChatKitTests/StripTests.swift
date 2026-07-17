import Testing
import Foundation
import CoreGraphics
import cchat_engine
@testable import ComicChatKit

// Plan 2 Task 11: the Swift Canvas layer end-to-end. These tests drive the
// ENTIRE lifted layout engine through the typed `Strip` wrapper and the two
// concrete `Canvas` implementations (`RecordingCanvas`, `CGCanvas`).
//
// SERIALIZATION (load-bearing): `Strip`/the engine mutate process-global state
// (the avatar registry, the session, the font statics, the backdrop registries,
// AND the shared metrics canvas installed via cc_set_metrics_canvas) with no
// internal locking. These tests must run one-at-a-time with each other AND with
// the C-selftest tests in EngineGlobalStateSelfTests — a Swift-side metrics
// canvas installed here would otherwise race the C selftest's own metrics
// canvas (verified: running the two suites in parallel corrupts BOTH the strip
// snapshot and cc_run_strip_selftest). `.serialized` only serializes WITHIN a
// suite, and separate top-level suites still run in parallel — so this suite is
// nested INSIDE EngineGlobalStateSelfTests (declared with the extension below),
// which is `.serialized`; a serialized ancestor serializes all descendant tests,
// putting every engine-global-state test on one timeline.
extension EngineGlobalStateSelfTests {
  @Suite(.serialized)
  struct StripTests {
    // The fixed 2x4 alternating conversation, IDENTICAL to the C++ strip
    // selftest (cc_selftest.cpp cc_selftest_strip): same fixture avatar
    // (anna.avb loaded twice), same nicks, same lines, same addressees, same
    // SAY mode, same backdrop (field.bgb). Building the same conversation
    // through Strip + Swift RecordingCanvas must reproduce the frozen C++
    // compose log line-for-line, proving the cc_canvas bridge end to end.
    private func buildFixedConversation(_ strip: Strip) throws -> (a: Int32, b: Int32) {
        let avatar = fixture("anna.avb")
        let backdrop = fixture("field.bgb")
        let a = try strip.addParticipant(nick: "Anna", avbPath: avatar)
        let b = try strip.addParticipant(nick: "Boris", avbPath: avatar)
        #expect(a == 1)
        #expect(b == 2)
        try strip.setBackdrop(backdrop)
        try strip.addLine(speaker: a, text: "Hello there", modes: .say, addressees: [b])
        try strip.addLine(speaker: b, text: "Hi yourself", modes: .say, addressees: [a])
        try strip.addLine(speaker: a, text: "How are you", modes: .say, addressees: [b])
        try strip.addLine(speaker: b, text: "Doing great", modes: .say, addressees: [a])
        return (a, b)
    }

    // (a) The snapshot test: Strip + Swift RecordingCanvas vs the frozen
    // Fixtures/strip-golden.txt (a byte-exact copy of the C++ kExpected
    // snapshot). Line-for-line equality proves the Swift `cc_canvas` bridge
    // (CanvasBox + static thunks) carries every op — text/rect/image/path/clip —
    // faithfully; any grammar drift in RecordingCanvas would break here.
    @Test func stripSnapshot() throws {
        // Layout-time text measurement must route through a deterministic
        // recording metrics canvas — the SAME fake metrics the C++ snapshot was
        // frozen under (measure_text = len*120 x 240). Hold the box for the whole
        // test so its cc_canvas outlives every engine call.
        let metricsCanvas = RecordingCanvas()
        let metricsBox = CanvasBox(metricsCanvas)
        cc_set_metrics_canvas(metricsBox.handle)

        try withExtendedLifetime(metricsBox) {
            let strip = try Strip()
            _ = try buildFixedConversation(strip)

            let recorder = RecordingCanvas()
            try strip.compose(onto: recorder)

            let goldenURL = Bundle.module.url(
                forResource: "strip-golden", withExtension: "txt", subdirectory: "Fixtures")!
            let goldenText = try String(contentsOf: goldenURL, encoding: .utf8)
            // One line per log entry; drop the trailing empty element the final
            // newline produces.
            var expectedLines = goldenText.components(separatedBy: "\n")
            if expectedLines.last == "" { expectedLines.removeLast() }

            #expect(recorder.log.count == expectedLines.count)
            let count = min(recorder.log.count, expectedLines.count)
            for i in 0..<count {
                if recorder.log[i] != expectedLines[i] {
                    Issue.record("""
                        strip snapshot mismatch at line \(i):
                          expected: \(expectedLines[i])
                          actual:   \(recorder.log[i])
                        """)
                }
            }
        }
    }

    // (b) THE EXIT MILESTONE: the same conversation composited through CGCanvas
    // into real pixels. Asserts non-empty PNG data, expected pixel dimensions
    // (page size in twips / 20 points, at 2x scale), and >1% non-white pixels
    // (proof something actually drew). Writes the PNG to
    // .superpowers/sdd/plan2-exit.png for human visual verification.
    @Test func stripPNG() throws {
        let metricsCanvas = RecordingCanvas()
        let metricsBox = CanvasBox(metricsCanvas)
        cc_set_metrics_canvas(metricsBox.handle)

        try withExtendedLifetime(metricsBox) {
            let strip = try Strip()
            _ = try buildFixedConversation(strip)

            let (w, h) = strip.size
            #expect(w > 0 && h > 0)

            let scale: CGFloat = 2.0
            let canvas = CGCanvas(widthTwips: w, heightTwips: h, scale: scale)
            try strip.compose(onto: canvas)

            // Expected pixel dimensions: twips/20 points * scale, rounded.
            let expectedW = Int((CGFloat(w) / 20.0 * scale).rounded())
            let expectedH = Int((CGFloat(h) / 20.0 * scale).rounded())
            #expect(canvas.pixelWidth == expectedW)
            #expect(canvas.pixelHeight == expectedH)

            guard let png = canvas.pngData() else {
                Issue.record("pngData() returned nil")
                return
            }
            #expect(!png.isEmpty)
            // PNG magic number sanity.
            #expect(png.starts(with: [0x89, 0x50, 0x4E, 0x47]))

            // Non-white pixel fraction from the rendered CGImage.
            let fraction = try nonWhiteFraction(canvas)
            #expect(fraction > 0.01)

            // Write the PNG for human review (out of tree; .superpowers/sdd
            // is gitignored). Resolve the repo root the same way the golden
            // catalog test does: 5 deletions from #filePath.
            let repoRoot = URL(fileURLWithPath: #filePath)
                .deletingLastPathComponent().deletingLastPathComponent()
                .deletingLastPathComponent().deletingLastPathComponent()
                .deletingLastPathComponent()
            let outURL = repoRoot
                .appendingPathComponent(".superpowers/sdd/plan2-exit.png")
            try png.write(to: outURL)
        }
    }

    // (c) CGCanvas.measure_text sanity: "Hello" in Comic Sans 12pt returns a
    // width in (300, 3000) twips and height in (200, 400) — loose bounds, since
    // CT metrics are OS-dependent (layout fidelity is bounded by the recording-
    // canvas snapshot test above, spec §9). Comic Sans 12pt: height -240 twips
    // (negative = char height), 12pt * 20 = 240.
    @Test func cgCanvasMeasures() throws {
        let canvas = CGCanvas(widthTwips: 4000, heightTwips: 2000, scale: 2.0)
        let font = FontSpec(face: "Comic Sans MS", height: -240, weight: 400)
        let text = "Hello"
        let (w, h): (Int32, Int32) = text.withCString { cptr in
            canvas.measureText(font, bytes: cptr, len: Int32(text.utf8.count))
        }
        #expect(w > 300 && w < 3000)
        #expect(h > 200 && h < 400)
    }

    // Fraction of non-white pixels in the canvas's rendered bitmap.
    private func nonWhiteFraction(_ canvas: CGCanvas) throws -> Double {
        guard let cg = canvas.makeCGImage() else {
            throw Strip.StripError(message: "makeCGImage failed")
        }
        let width = cg.width
        let height = cg.height
        var buffer = [UInt8](repeating: 0, count: width * height * 4)
        let colorSpace = CGColorSpace(name: CGColorSpace.sRGB)!
        guard let ctx = CGContext(
            data: &buffer, width: width, height: height,
            bitsPerComponent: 8, bytesPerRow: width * 4, space: colorSpace,
            bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue) else {
            throw Strip.StripError(message: "readback context failed")
        }
        ctx.draw(cg, in: CGRect(x: 0, y: 0, width: width, height: height))
        var nonWhite = 0
        let total = width * height
        var i = 0
        while i < buffer.count {
            let r = buffer[i], g = buffer[i + 1], b = buffer[i + 2]
            // "Non-white" = clearly off pure white.
            if r < 250 || g < 250 || b < 250 { nonWhite += 1 }
            i += 4
        }
        return Double(nonWhite) / Double(total)
    }
  }
}
