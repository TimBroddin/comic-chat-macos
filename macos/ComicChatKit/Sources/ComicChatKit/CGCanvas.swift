import Foundation
import CoreGraphics
import CoreText
import ImageIO
import UniformTypeIdentifiers
import cchat_engine

/// A `Canvas` that renders into a CoreGraphics RGBA8 bitmap — the real
/// pixel-producing target (the recording canvas's twin for verification, this
/// one for output). Produces the Plan 2 exit-milestone PNG.
///
/// COORDINATES: the engine works in MM_TWIPS, y-UP (1/1440 inch; larger y is
/// higher on the page), and a composed page spans x in [0, width] and y in
/// [-height, 0] (top row at y=0, descending negative). This maps to the bitmap
/// via a fixed CTM so twips coordinates can be used directly: points = twips/20,
/// times a `scale` supersampling factor, with the y-up page top pinned to the
/// bitmap top. CoreGraphics bitmaps are natively y-up (origin bottom-left), so
/// the engine's y-up convention needs no per-glyph flip — CoreText draws
/// upward, matching. One coordinate is NOT a straight passthrough, though:
/// `drawText`'s `y` is the TOP of the GDI text box (see its doc comment), which
/// must be converted to a CoreText baseline before use.
///
/// COLORS: engine colors are GDI COLORREF (0x00BBGGRR — red in the low byte).
///
/// THREADING: single-threaded like every Canvas (see `Canvas`).
public final class CGCanvas: Canvas {
    /// Pixel dimensions of the backing bitmap.
    public let pixelWidth: Int
    public let pixelHeight: Int
    /// Supersampling factor (points -> pixels). 2.0 gives crisp text.
    public let scale: CGFloat

    private let context: CGContext
    private let heightTwips: CGFloat

    // CTFont cache keyed by the requested spec — creating CTFonts is not free
    // and the engine measures/draws with a small set of specs repeatedly.
    private var fontCache: [FontSpec: CTFont] = [:]
    // A SEPARATE cache of the same fonts built at 20x point size, used only for
    // DRAWING (see `drawFont(for:)`/`drawText`'s doc comment for why: scaling
    // via CGContext.textMatrix instead, against this context's 1/20 CTM, was
    // tried first and corrupts multi-glyph line layout).
    private var drawFontCache: [FontSpec: CTFont] = [:]

    /// Create a bitmap sized for a page of `widthTwips` x `heightTwips`, at the
    /// given supersampling `scale`. Fills opaque white (the comic page ground).
    public init(widthTwips: Int32, heightTwips: Int32, scale: CGFloat = 2.0) {
        self.scale = scale
        self.heightTwips = CGFloat(heightTwips)
        let wPt = CGFloat(widthTwips) / 20.0
        let hPt = CGFloat(heightTwips) / 20.0
        // Guard against a zero-size page producing a 0x0 context (CGContext
        // creation fails): clamp to at least 1 pixel.
        self.pixelWidth = max(1, Int((wPt * scale).rounded()))
        self.pixelHeight = max(1, Int((hPt * scale).rounded()))

        let colorSpace = CGColorSpace(name: CGColorSpace.sRGB)!
        guard let ctx = CGContext(
            data: nil,
            width: pixelWidth,
            height: pixelHeight,
            bitsPerComponent: 8,
            bytesPerRow: pixelWidth * 4,
            space: colorSpace,
            bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)
        else {
            fatalError("CGCanvas: failed to create CGBitmapContext (\(pixelWidth)x\(pixelHeight))")
        }
        self.context = ctx

        // Opaque white background — the comic page ground.
        ctx.setFillColor(red: 1, green: 1, blue: 1, alpha: 1)
        ctx.fill(CGRect(x: 0, y: 0, width: pixelWidth, height: pixelHeight))

        // CTM: user space becomes TWIPS (y-up), page top pinned to bitmap top.
        //   scale(scale)         -> user space is points, origin bottom-left
        //   translate(0, hPt)    -> page top (twips y=0) sits at the bitmap top
        //   scale(1/20)          -> user space is twips
        // A twips coord (x, y in [-height,0]) then lands at device y
        //   ((hPt + y/20) * scale): y=0 -> top, y=-height -> bottom.
        ctx.scaleBy(x: scale, y: scale)
        ctx.translateBy(x: 0, y: hPt)
        ctx.scaleBy(x: 1.0 / 20.0, y: 1.0 / 20.0)

        // Anti-aliasing on for smooth balloons/text.
        ctx.setShouldAntialias(true)
        ctx.setAllowsAntialiasing(true)
    }

    // MARK: color / font helpers

    /// COLORREF (0x00BBGGRR) -> a CGColor in the context's sRGB space.
    private func cgColor(_ color: UInt32) -> (r: CGFloat, g: CGFloat, b: CGFloat) {
        let r = CGFloat(color & 0xFF) / 255.0
        let g = CGFloat((color >> 8) & 0xFF) / 255.0
        let b = CGFloat((color >> 16) & 0xFF) / 255.0
        return (r, g, b)
    }

    // Build a CTFont for `spec` at the given point size, applying bold/italic
    // symbolic traits if requested and available. Shared by `font(for:)` (real
    // point size, for measurement) and `drawFont(for:)` (20x point size, for
    // drawing).
    private func makeFont(spec: FontSpec, sizePt: CGFloat) -> CTFont {
        let face = spec.face.isEmpty ? "Helvetica" : spec.face
        var ct = CTFontCreateWithName(face as CFString, sizePt, nil)
        var symbolic: CTFontSymbolicTraits = []
        if spec.weight >= 600 { symbolic.insert(.traitBold) }
        if spec.italic { symbolic.insert(.traitItalic) }
        if !symbolic.isEmpty {
            if let styled = CTFontCreateCopyWithSymbolicTraits(ct, sizePt, nil, symbolic, symbolic) {
                ct = styled
            }
        }
        return ct
    }

    /// The real-point-size CTFont for `spec` (LOGFONT lfHeight/20 points),
    /// used for measurement (`measureText`/`fontMetrics`) where callers expect
    /// true point-based metrics.
    private func font(for spec: FontSpec) -> CTFont {
        if let cached = fontCache[spec] { return cached }
        // LOGFONT lfHeight is in twips; negative means character height. Point
        // size = |height| / 20 (twips per point).
        let sizePt = abs(CGFloat(spec.height)) / 20.0
        let ct = makeFont(spec: spec, sizePt: sizePt)
        fontCache[spec] = ct
        return ct
    }

    /// The DRAWING CTFont for `spec`, built at 20x the real point size.
    ///
    /// Why: this context's CTM maps user space to TWIPS (a 1/20 scale, see
    /// init's doc), so a real-point-size CTFont's glyph outlines render at
    /// 1/20 the intended size (Task 11 review finding 1: 12pt -> 12 twips =
    /// 0.6pt, empty-looking balloons). The first fix attempted was
    /// compensating with `context.textMatrix = scaleX:20,y:20` before
    /// `CTLineDraw` — that DOES restore correct glyph SIZE, but corrupts
    /// multi-glyph line layout: with the CTM already holding a 1/20 scale, a
    /// 20x textMatrix against it collapses every glyph after the first onto
    /// (approximately) the same position instead of advancing along the line
    /// (verified with a minimal repro: drawing "MMMMMMMMMM" that way renders
    /// ONE blob, not ten M's; building the CTFont at 20x point size instead
    /// and leaving the text matrix untouched renders all ten correctly
    /// spaced). So: scale the FONT, not the text matrix -- glyph outlines
    /// AND advances come out already twips-scaled, and `CTLineDraw` positions
    /// them normally through the existing 1/20 CTM.
    private func drawFont(for spec: FontSpec) -> CTFont {
        if let cached = drawFontCache[spec] { return cached }
        let sizePt = abs(CGFloat(spec.height)) / 20.0
        let ct = makeFont(spec: spec, sizePt: sizePt * 20.0)
        drawFontCache[spec] = ct
        return ct
    }

    /// Build a CTLine for `bytes` in `spec` with `color`, using the CTFont
    /// `fontForLine` returns (real point size for measurement, 20x for
    /// drawing -- see `font(for:)` vs `drawFont(for:)`). Bytes are decoded as
    /// CP-1252, falling back to ISO Latin-1 (both are 1:1 for ASCII).
    private func line(_ spec: FontSpec, bytes: UnsafePointer<CChar>?, len: Int32,
                      color: (r: CGFloat, g: CGFloat, b: CGFloat),
                      fontForLine: (FontSpec) -> CTFont) -> CTLine? {
        let text = Self.decodeBytes(bytes, len: len)
        guard !text.isEmpty else { return nil }
        let ctFont = fontForLine(spec)
        let fgColor = CGColor(colorSpace: CGColorSpace(name: CGColorSpace.sRGB)!,
                              components: [color.r, color.g, color.b, 1.0])!
        // Use CoreText attribute keys directly (no AppKit dependency).
        let attrs: [CFString: Any] = [
            kCTFontAttributeName: ctFont,
            kCTForegroundColorAttributeName: fgColor,
        ]
        let attr = CFAttributedStringCreate(nil, text as CFString, attrs as CFDictionary)!
        return CTLineCreateWithAttributedString(attr)
    }

    /// Decode raw engine bytes: CP-1252 first (the engine's text encoding),
    /// falling back to ISO Latin-1 so no byte sequence is ever un-renderable.
    static func decodeBytes(_ bytes: UnsafePointer<CChar>?, len: Int32) -> String {
        guard let bytes = bytes, len > 0 else { return "" }
        let buf = UnsafeBufferPointer(start: bytes, count: Int(len))
        let unsigned = buf.map { UInt8(bitPattern: $0) }
        return String(bytes: unsigned, encoding: .windowsCP1252)
            ?? String(bytes: unsigned, encoding: .isoLatin1)
            ?? ""
    }

    // MARK: Canvas

    public func measureText(_ f: FontSpec, bytes: UnsafePointer<CChar>?, len: Int32) -> (w: Int32, h: Int32) {
        let color = (r: CGFloat(0), g: CGFloat(0), b: CGFloat(0))
        guard let ctLine = line(f, bytes: bytes, len: len, color: color, fontForLine: font(for:)) else {
            return (0, 0)
        }
        var ascent: CGFloat = 0, descent: CGFloat = 0, leading: CGFloat = 0
        let widthPt = CTLineGetTypographicBounds(ctLine, &ascent, &descent, &leading)
        let heightPt = ascent + descent + leading
        // Round to twips (points * 20). CT metrics are OS-dependent; layout
        // fidelity is bounded by the recording-canvas snapshot tests (spec §9).
        let w = Int32((widthPt * 20.0).rounded())
        let h = Int32((heightPt * 20.0).rounded())
        return (w, h)
    }

    public func fontMetrics(_ f: FontSpec) -> TextMetrics {
        let ctFont = font(for: f)
        let ascentPt = CTFontGetAscent(ctFont)
        let descentPt = CTFontGetDescent(ctFont)
        let leadingPt = CTFontGetLeading(ctFont)
        let ascent = Int32((ascentPt * 20.0).rounded())
        let descent = Int32((descentPt * 20.0).rounded())
        let leading = Int32((leadingPt * 20.0).rounded())
        let height = ascent + descent
        let ave = Int32(((ascentPt + descentPt) * 0.5 * 20.0).rounded())
        return TextMetrics(height: height, ascent: ascent, descent: descent,
                           internalLeading: 0, externalLeading: leading,
                           aveCharWidth: max(1, ave), maxCharWidth: max(1, ave * 2))
    }

    public func drawText(_ f: FontSpec, x: Int32, y: Int32, color: UInt32,
                         bkOpaque: Bool, bkColor: UInt32,
                         bytes: UnsafePointer<CChar>?, len: Int32) {
        let fg = cgColor(color)
        // Real-point-size metrics (ascent/descent/width) drive the twips-space
        // math below (baseline conversion, background rect) -- built from the
        // MEASUREMENT font (`font(for:)`), matching what `measureText`/
        // `fontMetrics` report, and independent of the DRAWING font's 20x
        // scale (see `drawFont(for:)`'s doc comment).
        guard let metricsLine = line(f, bytes: bytes, len: len, color: fg, fontForLine: font(for:))
        else { return }
        var ascent: CGFloat = 0, descent: CGFloat = 0, leading: CGFloat = 0
        let widthPt = CTLineGetTypographicBounds(metricsLine, &ascent, &descent, &leading)

        // `y` is the TOP of the glyph box, not a baseline: the engine is lifted
        // Win32 GDI code (mfc_compat.h's CDC::TextOut forwards straight to
        // draw_text with no SetTextAlign call anywhere in the codebase, so
        // GDI's default TA_TOP|TA_LEFT alignment applies) running in a y-up
        // MM_TWIPS space (comicchat.h's canvas-boundary doc). TA_TOP's "top"
        // in a y-up space is the LARGEST y of the box, with the glyphs
        // extending downward (toward smaller y) from there — confirmed by
        // balloon.cpp's CLabel::iDrawFormattedTextLine, whose `iBaseY` starts
        // at `m_bbox.Top` and steps DOWN by a full line height per line.
        // CoreText's `textPosition`, in contrast, is the BASELINE. Convert:
        // baseline = (top-of-box) - ascent, in the same twips units as `y`.
        let ascTwips = ascent * 20.0
        let descTwips = descent * 20.0
        let baselineY = CGFloat(y) - ascTwips

        context.saveGState()
        // Optional opaque text background: fill the line's typographic box.
        if bkOpaque {
            let bg = cgColor(bkColor)
            context.setFillColor(red: bg.r, green: bg.g, blue: bg.b, alpha: 1)
            // Rect in twips around the baseline (y-up: ascent above, descent below).
            let wTwips = CGFloat(widthPt) * 20.0
            context.fill(CGRect(x: CGFloat(x), y: baselineY - descTwips,
                                width: wTwips, height: ascTwips + descTwips))
        }
        // Draw with the DRAWING font (built at 20x point size, see
        // `drawFont(for:)`): its glyph outlines and advances are already
        // twips-scaled, so `CTLineDraw` positions them correctly through the
        // existing 1/20 CTM (see init's doc) with NO text-matrix scaling
        // needed -- scaling via `context.textMatrix` here instead was tried
        // first and corrupts multi-glyph line layout (see `drawFont(for:)`'s
        // doc comment for the repro). The pen position (`textPosition`) is
        // the baseline in twips user space (matches the layout engine's x,
        // and the baseline derived from its y above).
        guard let drawLine = line(f, bytes: bytes, len: len, color: fg, fontForLine: drawFont(for:))
        else { return }
        context.textPosition = CGPoint(x: CGFloat(x), y: baselineY)
        CTLineDraw(drawLine, context)
        context.restoreGState()
    }

    public func fillRect(l: Int32, t: Int32, r: Int32, b: Int32, color: UInt32) {
        let c = cgColor(color)
        context.setFillColor(red: c.r, green: c.g, blue: c.b, alpha: 1)
        // (l,t,r,b) with y-up: t is the top (larger y), b the bottom (smaller y).
        let x = CGFloat(min(l, r))
        let width = CGFloat(abs(r - l))
        let yBottom = CGFloat(min(t, b))
        let height = CGFloat(abs(t - b))
        context.fill(CGRect(x: x, y: yBottom, width: width, height: height))
    }

    public func drawImage(_ img: UnsafePointer<cc_image>?,
                          dl: Int32, dt: Int32, dr: Int32, db: Int32,
                          sl: Int32, st: Int32, sr: Int32, sb: Int32) {
        guard let img = img else { return }
        let image = img.pointee
        guard image.width > 0, image.height > 0, let base = image.rgba else { return }

        let iw = Int(image.width)
        let ih = Int(image.height)
        // Build a CGImage from the engine's straight-alpha RGBA8 buffer. Copy the
        // bytes (the engine owns the buffer only for this call).
        let byteCount = iw * ih * 4
        let data = Data(bytes: base, count: byteCount)
        guard let provider = CGDataProvider(data: data as CFData) else { return }
        guard let cgImage = CGImage(
            width: iw, height: ih,
            bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: iw * 4,
            space: CGColorSpace(name: CGColorSpace.sRGB)!,
            bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.last.rawValue),
            provider: provider, decode: nil, shouldInterpolate: true, intent: .defaultIntent)
        else { return }

        // Source sub-rect (sl,st,sr,sb) in the engine image's pixel space, which
        // is top-down. Crop to it.
        let srcX = Int(min(sl, sr))
        let srcY = Int(min(st, sb))
        let srcW = Int(abs(sr - sl))
        let srcH = Int(abs(sb - st))
        let cropped: CGImage
        if srcX == 0 && srcY == 0 && srcW == iw && srcH == ih {
            cropped = cgImage
        } else if srcW > 0, srcH > 0,
                  let c = cgImage.cropping(to: CGRect(x: srcX, y: srcY, width: srcW, height: srcH)) {
            cropped = c
        } else {
            cropped = cgImage
        }

        // Destination rect (dl,dt,dr,db) in twips, y-up: bottom is the smaller y.
        // (dr/dl may be swapped — take min for left, abs for extent.)
        let dx = CGFloat(min(dl, dr))
        let dw = CGFloat(abs(dr - dl))
        let dyBottom = CGFloat(min(dt, db))
        let dh = CGFloat(abs(dt - db))

        // CGContext.draw already renders a top-down CGImage upright in a y-up
        // context (it places row 0 at the rect's top / max-y). No manual flip.
        context.draw(cropped, in: CGRect(x: dx, y: dyBottom, width: dw, height: dh))
    }

    public func path(_ pts: UnsafePointer<cc_path_pt>?, n: Int32,
                     doFill: Bool, fillColor: UInt32,
                     doStroke: Bool, strokeColor: UInt32,
                     strokeWidth: Int32, dashed: Bool) {
        guard let pts = pts, n > 0 else { return }
        let path = CGMutablePath()
        var haveCurrent = false
        var i: Int32 = 0
        while i < n {
            let pt = pts[Int(i)]
            switch pt.verb {
            case PathVerb.move.rawValue:
                path.move(to: CGPoint(x: CGFloat(pt.x), y: CGFloat(pt.y)))
                haveCurrent = true
                i += 1
            case PathVerb.line.rawValue:
                if haveCurrent {
                    path.addLine(to: CGPoint(x: CGFloat(pt.x), y: CGFloat(pt.y)))
                } else {
                    path.move(to: CGPoint(x: CGFloat(pt.x), y: CGFloat(pt.y)))
                    haveCurrent = true
                }
                i += 1
            case PathVerb.cubic.rawValue:
                // Three consecutive .cubic entries = control1, control2, endpoint.
                if i + 2 >= n { i = n; break }
                let c1 = pts[Int(i)]
                let c2 = pts[Int(i + 1)]
                let end = pts[Int(i + 2)]
                if !haveCurrent {
                    path.move(to: CGPoint(x: CGFloat(c1.x), y: CGFloat(c1.y)))
                    haveCurrent = true
                }
                path.addCurve(to: CGPoint(x: CGFloat(end.x), y: CGFloat(end.y)),
                              control1: CGPoint(x: CGFloat(c1.x), y: CGFloat(c1.y)),
                              control2: CGPoint(x: CGFloat(c2.x), y: CGFloat(c2.y)))
                i += 3
            case PathVerb.close.rawValue:
                path.closeSubpath()
                i += 1
            default:
                i += 1
            }
        }

        context.saveGState()
        context.addPath(path)
        if doFill && doStroke {
            let fc = cgColor(fillColor)
            let sc = cgColor(strokeColor)
            context.setFillColor(red: fc.r, green: fc.g, blue: fc.b, alpha: 1)
            context.setStrokeColor(red: sc.r, green: sc.g, blue: sc.b, alpha: 1)
            context.setLineWidth(CGFloat(max(1, strokeWidth)))
            applyDash(dashed)
            // Fill then stroke, keeping the path for the stroke pass.
            context.drawPath(using: .fillStroke)
        } else if doFill {
            let fc = cgColor(fillColor)
            context.setFillColor(red: fc.r, green: fc.g, blue: fc.b, alpha: 1)
            context.fillPath()
        } else if doStroke {
            let sc = cgColor(strokeColor)
            context.setStrokeColor(red: sc.r, green: sc.g, blue: sc.b, alpha: 1)
            context.setLineWidth(CGFloat(max(1, strokeWidth)))
            applyDash(dashed)
            context.strokePath()
        }
        context.restoreGState()
    }

    private func applyDash(_ dashed: Bool) {
        if dashed {
            // A visible dash pattern in twips; scale roughly to the pen width.
            context.setLineDash(phase: 0, lengths: [120, 120])
        } else {
            context.setLineDash(phase: 0, lengths: [])
        }
    }

    // Clip model (matches the engine's logged grammar, NOT a balanced stack):
    //   clip+ l,t,r,b  = INTERSECT the current clip with this rect
    //   clip-          = RESET the clip to none (the engine's SelectClipRgn(NULL))
    // CoreGraphics clips only shrink, so each `clip+` is a saveGState()+clip and
    // `clip-` restores every accumulated clip gstate back to the unclipped
    // baseline. `clipDepth` tracks how many saves are outstanding. Per panel the
    // engine emits two `clip+` (unit rect, twice), draws, then one `clip-`
    // (drains BOTH) and one `clip+` at the +/-2^28 sentinel (effectively no clip)
    // — so a single restore-per-push would leave the panel clip active forever
    // and collapse every later panel into panel 1. Draining fixes that.
    private var clipDepth = 0

    public func clipPush(l: Int32, t: Int32, r: Int32, b: Int32) {
        context.saveGState()
        clipDepth += 1
        let x = CGFloat(min(l, r))
        let width = CGFloat(abs(r - l))
        let yBottom = CGFloat(min(t, b))
        let height = CGFloat(abs(t - b))
        context.clip(to: CGRect(x: x, y: yBottom, width: width, height: height))
    }

    public func clipPop() {
        // Reset to the unclipped baseline: undo every outstanding clip gstate.
        while clipDepth > 0 {
            context.restoreGState()
            clipDepth -= 1
        }
    }

    public func isPrinting() -> Bool { false }

    // MARK: output

    /// The rendered bitmap as a CGImage.
    public func makeCGImage() -> CGImage? {
        context.makeImage()
    }

    /// PNG-encode the rendered bitmap via ImageIO.
    public func pngData() -> Data? {
        guard let cgImage = context.makeImage() else { return nil }
        let mutableData = CFDataCreateMutable(nil, 0)!
        guard let dest = CGImageDestinationCreateWithData(
            mutableData, UTType.png.identifier as CFString, 1, nil) else { return nil }
        CGImageDestinationAddImage(dest, cgImage, nil)
        guard CGImageDestinationFinalize(dest) else { return nil }
        return mutableData as Data
    }
}
