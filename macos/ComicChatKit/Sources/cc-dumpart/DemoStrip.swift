import Foundation
import CoreGraphics
import cchat_engine
import ComicChatKit

// The built-in demo comic strip for `cc-dumpart --strip <out.png>`: two comicart
// avatars, a 4-line conversation, rendered through CGCanvas — the Plan 2 exit
// milestone as a runnable command.

/// Resolve the comicart directory (repo-root/v2.5-beta-1-modern/comicart) from
/// this source file's location, or the CC_COMICART_DIR override. Same 5-deletion
/// walk the golden catalog test uses:
///   DemoStrip.swift -> cc-dumpart -> Sources -> ComicChatKit -> macos -> repoRoot
func comicartDir() -> String {
    if let override = ProcessInfo.processInfo.environment["CC_COMICART_DIR"] {
        return override
    }
    let repoRoot = URL(fileURLWithPath: #filePath)
        .deletingLastPathComponent().deletingLastPathComponent()
        .deletingLastPathComponent().deletingLastPathComponent()
        .deletingLastPathComponent()
    return repoRoot.appendingPathComponent("v2.5-beta-1-modern/comicart").path
}

func renderDemoStrip(toPath outPath: String, metricsCanvas: Canvas = CTMetricsCanvas()) throws {
    let art = comicartDir()
    let anna = "\(art)/anna.avb"
    let armando = "\(art)/armando.avb"
    let backdrop = "\(art)/field.bgb"

    // Layout-time text measurement routes through `metricsCanvas`: real
    // CoreText metrics (CTMetricsCanvas) by default (Plan 4a Task 4), or the
    // deterministic fake RecordingCanvas table when the caller passes
    // `--fake-metrics` (main.swift). Held for the whole render so its
    // cc_canvas outlives every engine call.
    let metricsBox = CanvasBox(metricsCanvas)
    cc_set_metrics_canvas(metricsBox.handle)

    try withExtendedLifetime(metricsBox) {
        let strip = try Strip()
        let a = try strip.addParticipant(nick: "Anna", avbPath: anna)
        let b = try strip.addParticipant(nick: "Armando", avbPath: armando)
        try strip.setBackdrop(backdrop)
        try strip.addLine(speaker: a, text: "Hey Armando!", modes: .say, addressees: [b])
        try strip.addLine(speaker: b, text: "Hi Anna!", modes: .say, addressees: [a])
        try strip.addLine(speaker: a, text: "Comic Chat lives", modes: .say, addressees: [b])
        try strip.addLine(speaker: b, text: "On the Mac now", modes: .say, addressees: [a])

        let (w, h) = strip.size
        guard w > 0, h > 0 else {
            throw NSError(domain: "DemoStrip", code: 1, userInfo: [
                NSLocalizedDescriptionKey: "empty strip (size \(w)x\(h))"])
        }

        let canvas = CGCanvas(widthTwips: w, heightTwips: h, scale: 2.0)
        try strip.compose(onto: canvas)

        guard let png = canvas.pngData() else {
            throw NSError(domain: "DemoStrip", code: 2, userInfo: [
                NSLocalizedDescriptionKey: "pngData() returned nil"])
        }
        try png.write(to: URL(fileURLWithPath: outPath))
        FileHandle.standardError.write(Data(
            "cc-dumpart: wrote \(canvas.pixelWidth)x\(canvas.pixelHeight) strip to \(outPath)\n".utf8))
    }
}
