import Testing
import Foundation
import CoreGraphics
import cchat_engine
@testable import ComicChatKit

// Plan 2 follow-on (Task 13): `--script` mode for cc-dumpart renders a
// user-authored JSON conversation to a comic-strip PNG. `StripScript` (in
// ComicChatKit, not the cc-dumpart executable target) is the testable core:
// decode JSON -> validate -> drive `Strip` -> compose onto `CGCanvas` -> PNG
// bytes. main.swift stays a thin CLI shell around it.
//
// Nested inside EngineGlobalStateSelfTests (see StripTests.swift's doc comment
// for why): StripScript drives the same process-global engine state (avatar
// registry, session, font statics, metrics canvas) as every other Strip test,
// so it must run on the same serialized timeline.
extension EngineGlobalStateSelfTests {
  @Suite(.serialized)
  struct StripScriptTests {
    // (a) THE FIRST end-to-end think/whisper coverage in the project: every
    // other Strip-driving test (StripTests' fixed conversation, the C++
    // selftest) only ever exercises CC_MODE_SAY. A whisper line routes through
    // the aura-drawing path in compose (a visual "whisper cloud" around the
    // balloon) that say/action never touch, and a think line swaps the balloon
    // tail for a thought-bubble chain -- neither has ANY coverage before this
    // test. Two participants, four lines (say, think, whisper, say) -- deliberately
    // includes both of the previously-uncovered modes.
    @Test func scriptModeRendersJSON() throws {
        let metricsCanvas = RecordingCanvas()
        let metricsBox = CanvasBox(metricsCanvas)
        cc_set_metrics_canvas(metricsBox.handle)

        try withExtendedLifetime(metricsBox) {
            let json = """
            {
              "backdrop": "\(fixture("field.bgb"))",
              "participants": [
                {"nick": "Anna", "avatar": "\(fixture("anna.avb"))"},
                {"nick": "Boris", "avatar": "\(fixture("anna.avb"))"}
              ],
              "lines": [
                {"speaker": "Anna", "text": "Hello there!", "mode": "say", "to": ["Boris"]},
                {"speaker": "Boris", "text": "Hmm, who is this?", "mode": "think"},
                {"speaker": "Anna", "text": "psst... it's me", "mode": "whisper", "to": ["Boris"]},
                {"speaker": "Boris", "text": "Oh, hi Anna!", "mode": "say", "to": ["Anna"]}
              ]
            }
            """
            let tmpDir = FileManager.default.temporaryDirectory
            let scriptURL = tmpDir.appendingPathComponent("stripscript-\(UUID().uuidString).json")
            try json.data(using: .utf8)!.write(to: scriptURL)
            defer { try? FileManager.default.removeItem(at: scriptURL) }

            let script = try StripScript(contentsOf: scriptURL)
            let result = try script.render(scale: 2.0)

            #expect(!result.pngData.isEmpty)
            #expect(result.pngData.starts(with: [0x89, 0x50, 0x4E, 0x47]))
            #expect(result.pixelWidth > 0)
            #expect(result.pixelHeight > 0)
            #expect(result.panelCount > 0)

            // >1% non-white pixels: proof something actually drew (the same
            // sanity bar stripPNG in StripTests.swift uses).
            let fraction = try nonWhiteFraction(result.pngData)
            #expect(fraction > 0.01)
        }
    }

    // (b) Bad-input rejection: unknown mode, unknown speaker nick, and missing
    // avatar file each produce a SPECIFIC typed error case (not just "any
    // error") -- asserted by pattern-matching StripScript.ScriptError, not by
    // scraping stderr text.
    @Test func scriptModeRejectsBadInput() throws {
        // The missing-avatar sub-case below reaches into the engine (a `Strip`
        // is created and `addParticipant` attempted), and `cc_strip_create`'s
        // metrics-DC path ASSERTs a metrics canvas is installed even for that
        // -- so one must be live for the whole test, same as every other
        // engine-touching test in this file/StripTests.swift.
        let metricsCanvas = RecordingCanvas()
        let metricsBox = CanvasBox(metricsCanvas)
        cc_set_metrics_canvas(metricsBox.handle)
        try withExtendedLifetime(metricsBox) {
            try runScriptModeRejectsBadInput()
        }
    }

    private func runScriptModeRejectsBadInput() throws {
        let tmpDir = FileManager.default.temporaryDirectory

        func write(_ json: String) throws -> URL {
            let url = tmpDir.appendingPathComponent("stripscript-bad-\(UUID().uuidString).json")
            try json.data(using: .utf8)!.write(to: url)
            return url
        }

        // Unknown mode.
        do {
            let url = try write("""
            {
              "participants": [{"nick": "Anna", "avatar": "\(fixture("anna.avb"))"}],
              "lines": [{"speaker": "Anna", "text": "Hi", "mode": "yell"}]
            }
            """)
            defer { try? FileManager.default.removeItem(at: url) }
            #expect(throws: StripScript.ScriptError.self) {
                _ = try StripScript(contentsOf: url)
            }
            do {
                _ = try StripScript(contentsOf: url)
                Issue.record("expected unknownMode error")
            } catch let error as StripScript.ScriptError {
                guard case .unknownMode(let mode, let valid) = error else {
                    Issue.record("expected .unknownMode, got \(error)")
                    return
                }
                #expect(mode == "yell")
                #expect(valid.contains("say"))
            }
        }

        // Unknown speaker nick.
        do {
            let url = try write("""
            {
              "participants": [{"nick": "Anna", "avatar": "\(fixture("anna.avb"))"}],
              "lines": [{"speaker": "Ghost", "text": "Boo"}]
            }
            """)
            defer { try? FileManager.default.removeItem(at: url) }
            do {
                _ = try StripScript(contentsOf: url)
                Issue.record("expected unknownNick error")
            } catch let error as StripScript.ScriptError {
                guard case .unknownNick(let nick) = error else {
                    Issue.record("expected .unknownNick, got \(error)")
                    return
                }
                #expect(nick == "Ghost")
            }
        }

        // Missing avatar file.
        do {
            let missingPath = tmpDir.appendingPathComponent("no-such-avatar-\(UUID().uuidString).avb").path
            let url = try write("""
            {
              "participants": [{"nick": "Anna", "avatar": "\(missingPath)"}],
              "lines": [{"speaker": "Anna", "text": "Hi"}]
            }
            """)
            defer { try? FileManager.default.removeItem(at: url) }
            let script = try StripScript(contentsOf: url)
            do {
                _ = try script.render(scale: 2.0)
                Issue.record("expected artLoadFailed error")
            } catch let error as StripScript.ScriptError {
                guard case .artLoadFailed(let kind, let path) = error else {
                    Issue.record("expected .artLoadFailed, got \(error)")
                    return
                }
                #expect(kind == "avatar")
                #expect(path == missingPath)
            }
        }
    }

    // Fraction of non-white pixels, decoded straight from PNG bytes.
    private func nonWhiteFraction(_ pngData: Data) throws -> Double {
        guard let provider = CGDataProvider(data: pngData as CFData),
              let cg = CGImage(pngDataProviderSource: provider, decode: nil,
                                shouldInterpolate: true, intent: .defaultIntent)
        else {
            throw Strip.StripError(message: "PNG decode failed")
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
            if r < 250 || g < 250 || b < 250 { nonWhite += 1 }
            i += 4
        }
        return Double(nonWhite) / Double(total)
    }
  }
}
