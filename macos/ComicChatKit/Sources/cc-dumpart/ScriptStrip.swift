import Foundation
import cchat_engine
import ComicChatKit

// `cc-dumpart --script <conversation.json> <out.png>`: thin CLI shell around
// StripScript (ComicChatKit) -- decode/validate the JSON, install the same
// deterministic metrics canvas the demo/tests use, render, and write the PNG.

func runScriptMode(jsonPath: String, outPath: String) throws {
    let art = comicartDir()

    // Layout-time text measurement routes through a deterministic recording
    // metrics canvas, exactly like --strip's demo path. Held for the whole
    // render so its cc_canvas outlives every engine call.
    let metricsBox = CanvasBox(RecordingCanvas())
    cc_set_metrics_canvas(metricsBox.handle)

    try withExtendedLifetime(metricsBox) {
        let script = try StripScript(contentsOf: URL(fileURLWithPath: jsonPath), comicartDir: art)
        let result = try script.render(scale: 2.0)

        try result.pngData.write(to: URL(fileURLWithPath: outPath))
        FileHandle.standardError.write(Data(
            "cc-dumpart: wrote \(result.pixelWidth)x\(result.pixelHeight) strip (\(result.panelCount) panels) to \(outPath)\n".utf8))
    }
}
