import Testing
import Foundation
import CoreGraphics
import cchat_engine
@testable import ComicChatKit

// Bug: flipped avatar bodies render un-flipped, so characters don't face each
// other. Root cause: the lifted engine signals "mirror this image horizontally"
// by emitting a dest rect whose left/right edges arrive in REVERSED order
// (dr < dl) -- see FlipBodyBox (engine/bodycam.cpp) and the original GDI
// StretchDIBits negative-width-blit semantics it replicates. CGCanvas.drawImage
// normalized that away with `abs(dr - dl)`, silently discarding the mirror.
// These tests drive `CGCanvas.drawImage` directly with a synthetic 2x1
// asymmetric image (red pixel left column, blue pixel right column) so a
// mirrored draw is trivially distinguishable from a normal one by reading back
// pixels from the bitmap.
//
// No engine/avatar/session state is touched here (a raw `cc_image` is built by
// hand), so unlike EngineGlobalStateSelfTests this suite needs no
// serialization against the process-global engine registry.
struct CGCanvasMirrorTests {
    // 2 wide x 1 tall, RGBA8 straight alpha: column 0 opaque red, column 1
    // opaque blue -- matches cc_image's documented layout (comicchat.h).
    private func makeRedBlueImage(_ body: (UnsafePointer<cc_image>) -> Void) {
        var rgba: [UInt8] = [
            255, 0, 0, 255,   // (0,0) red
            0, 0, 255, 255,   // (1,0) blue
        ]
        rgba.withUnsafeMutableBufferPointer { buf in
            var img = cc_image(width: 2, height: 1, rgba: buf.baseAddress)
            withUnsafePointer(to: &img) { body($0) }
        }
    }

    // Read back the pixel at (x, y) in the canvas's rendered bitmap (top-left
    // origin, matching CGImage row order) as (r, g, b).
    private func pixel(_ canvas: CGCanvas, x: Int, y: Int) throws -> (UInt8, UInt8, UInt8) {
        guard let cg = canvas.makeCGImage() else {
            throw Strip.StripError(message: "makeCGImage failed")
        }
        let width = cg.width, height = cg.height
        var buffer = [UInt8](repeating: 0, count: width * height * 4)
        let colorSpace = CGColorSpace(name: CGColorSpace.sRGB)!
        guard let ctx = CGContext(
            data: &buffer, width: width, height: height,
            bitsPerComponent: 8, bytesPerRow: width * 4, space: colorSpace,
            bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue) else {
            throw Strip.StripError(message: "readback context failed")
        }
        // CGContext is y-up (origin bottom-left); draw the image as-is, then
        // convert the caller's top-left (x,y) request to this buffer's row order.
        ctx.draw(cg, in: CGRect(x: 0, y: 0, width: width, height: height))
        let flippedY = height - 1 - y
        let i = (flippedY * width + x) * 4
        return (buffer[i], buffer[i + 1], buffer[i + 2])
    }

    // Normal-order dest rect (dl < dr): the image draws upright -- red on the
    // left half of the dest rect, blue on the right half.
    @Test func drawImageNormalOrderIsUnmirrored() throws {
        let canvas = CGCanvas(widthTwips: 2000, heightTwips: 1000, scale: 1.0)
        makeRedBlueImage { img in
            canvas.drawImage(img, dl: 0, dt: 0, dr: 2000, db: -1000,
                             sl: 0, st: 0, sr: 2, sb: 1)
        }
        let left = try pixel(canvas, x: 10, y: 10)
        let right = try pixel(canvas, x: canvas.pixelWidth - 10, y: 10)
        #expect(left.0 > 200 && left.2 < 100)   // left is red
        #expect(right.2 > 200 && right.0 < 100) // right is blue
    }

    // Mirrored dest rect (dr < dl): the engine's "face the other way" signal.
    // The image must draw MIRRORED within the same normalized screen rect --
    // blue (source-right) now on the left, red (source-left) on the right.
    // RED before the fix: the old code took abs(dr-dl) and dropped the
    // ordering, rendering identically to the normal-order case above.
    @Test func drawImageMirroredOrderFlipsHorizontally() throws {
        let canvas = CGCanvas(widthTwips: 2000, heightTwips: 1000, scale: 1.0)
        makeRedBlueImage { img in
            // dr < dl: mirrored dest rect, same screen-space extent as the
            // normal-order test above (min=0, extent=2000).
            canvas.drawImage(img, dl: 2000, dt: 0, dr: 0, db: -1000,
                             sl: 0, st: 0, sr: 2, sb: 1)
        }
        let left = try pixel(canvas, x: 10, y: 10)
        let right = try pixel(canvas, x: canvas.pixelWidth - 10, y: 10)
        #expect(left.2 > 200 && left.0 < 100)   // left is now blue
        #expect(right.0 > 200 && right.2 < 100) // right is now red
    }
}
