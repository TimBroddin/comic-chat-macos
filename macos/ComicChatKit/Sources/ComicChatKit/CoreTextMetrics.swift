import Foundation
import CoreText
import CoreGraphics

/// Real CoreText-backed text measurement and font metrics, shared by
/// `CTMetricsCanvas` (the production layout-time metrics canvas) and
/// `CGCanvas.fontMetrics` (the drawing canvas — see that file's doc comment:
/// the three placeholder fields it used to synthesize are replaced with calls
/// into this type, so the canvas that MEASURES for layout and the canvas that
/// DRAWS agree by construction).
///
/// TABLE-READ CONVENTION (Plan 4a Task 4 / D3 open Q5, binding): `aveCharWidth`
/// and `maxCharWidth` are read from the font's own OS/2/hhea tables FIRST —
/// these are the values a real Windows GDI `GetTextMetrics` call would itself
/// be sourcing (`tmAveCharWidth`/`tmMaxCharWidth` derive from exactly these
/// fields), so reading them directly is the more faithful port, not a
/// convenience shortcut. Table reads use the RAW font file bytes: big-endian,
/// `xAvgCharWidth` at OS/2 byte offset 2 (`int16_t`), `advanceWidthMax` at hhea
/// byte offset 10 (`uint16_t`), scaled `value * pointSize / unitsPerEm * 20`
/// (unitsPerEm -> points -> twips, the same points-to-twips ×20 convention
/// `CGCanvas.measureText`/`fontMetrics` use throughout). Only if a table is
/// missing or too short to read does this fall back to a live MEASUREMENT:
/// `aveCharWidth` = width of "abcdefghijklmnopqrstuvwxyz" / 26; `maxCharWidth`
/// = the widest single printable-ASCII glyph advance. Both paths are
/// deliberately kept (rather than always measuring) because the table values
/// are what a real font's `TEXTMETRIC` reports even for glyphs the measured
/// fallback never samples (e.g. "average character width" per the OS/2 spec
/// is an English-letter-frequency weighting, not a plain arithmetic mean —
/// it can legitimately sit outside the range the fallback's simple average
/// would produce for the same font).
public enum CoreTextMetrics {
    // CTFont cache keyed by FontSpec, shared across every call. Building a
    // CTFont is not free (name lookup + trait synthesis); CTMetricsCanvas and
    // CGCanvas.fontMetrics both call through here per glyph run, so caching
    // matters exactly like CGCanvas's own `fontCache` (CGCanvas.swift:38).
    //
    // NOT thread-safe, matching the engine's single-thread contract (Canvas's
    // own doc comment): every Canvas implementation, and now this shared
    // helper, is driven from one thread at a time. `nonisolated(unsafe)`
    // records that as a deliberate, verified exception (same convention as
    // cc-dumpart/ReplayStrip.swift's EventDrainBox) rather than silencing
    // Swift 6's global-mutable-state check with a broader escape hatch.
    private nonisolated(unsafe) static var fontCache: [FontSpec: CTFont] = [:]

    /// The CTFont for `spec`, built at its real point size (LOGFONT lfHeight
    /// in twips / 20 -- the "character height" GDI convention). Cached.
    static func font(for spec: FontSpec) -> CTFont {
        if let cached = fontCache[spec] { return cached }
        let face = spec.face.isEmpty ? "Helvetica" : spec.face
        let sizePt = abs(CGFloat(spec.height)) / 20.0
        var ct = CTFontCreateWithName(face as CFString, sizePt, nil)
        var symbolic: CTFontSymbolicTraits = []
        if spec.weight >= 600 { symbolic.insert(.traitBold) }
        if spec.italic { symbolic.insert(.traitItalic) }
        if !symbolic.isEmpty {
            if let styled = CTFontCreateCopyWithSymbolicTraits(ct, sizePt, nil, symbolic, symbolic) {
                ct = styled
            }
        }
        fontCache[spec] = ct
        return ct
    }

    /// Measure `bytes` (`len` raw CP-1252/Latin-1 bytes) in font `f`. Returns
    /// (width, height) in twips — the same contract as `Canvas.measureText`.
    public static func measure(_ f: FontSpec, bytes: UnsafePointer<CChar>?, len: Int32) -> (w: Int32, h: Int32) {
        guard let ctLine = makeLine(font(for: f), bytes: bytes, len: len) else {
            return (0, 0)
        }
        var ascent: CGFloat = 0, descent: CGFloat = 0, leading: CGFloat = 0
        let widthPt = CTLineGetTypographicBounds(ctLine, &ascent, &descent, &leading)
        let heightPt = ascent + descent + leading
        let w = Int32((widthPt * 20.0).rounded())
        let h = Int32((heightPt * 20.0).rounded())
        return (w, h)
    }

    /// The TEXTMETRIC-equivalent fields for `f`, in twips.
    public static func metrics(for f: FontSpec) -> TextMetrics {
        let ctFont = font(for: f)
        let ascentPt = CTFontGetAscent(ctFont)
        let descentPt = CTFontGetDescent(ctFont)
        let leadingPt = CTFontGetLeading(ctFont)
        let ascent = Int32((ascentPt * 20.0).rounded())
        let descent = Int32((descentPt * 20.0).rounded())
        let externalLeading = Int32((leadingPt * 20.0).rounded())
        let height = ascent + descent

        // internalLeading = the gap between the requested LOGFONT character
        // height and this font's actual ascent+descent -- GDI's TEXTMETRIC
        // convention (tmInternalLeading), clamped to >= 0 since a font can
        // legitimately have real ascent+descent >= the requested height.
        let requestedHeight = abs(f.height)
        let internalLeading = max(0, height - requestedHeight)

        let unitsPerEm = CTFontGetUnitsPerEm(ctFont)
        let sizePt = abs(CGFloat(f.height)) / 20.0

        let ave = aveCharWidth(ctFont, f: f, unitsPerEm: unitsPerEm, sizePt: sizePt)
        let maxW = maxCharWidth(ctFont, f: f, unitsPerEm: unitsPerEm, sizePt: sizePt)

        return TextMetrics(height: height, ascent: ascent, descent: descent,
                           internalLeading: internalLeading, externalLeading: externalLeading,
                           aveCharWidth: max(1, ave), maxCharWidth: max(1, maxW))
    }

    // MARK: - OS/2 xAvgCharWidth / hhea advanceWidthMax

    private static func aveCharWidth(_ ctFont: CTFont, f: FontSpec, unitsPerEm: UInt32, sizePt: CGFloat) -> Int32 {
        if let raw = readOS2XAvgCharWidth(ctFont), unitsPerEm > 0 {
            let scaled = Double(raw) * Double(sizePt) / Double(unitsPerEm) * 20.0
            return Int32(scaled.rounded())
        }
        // Fallback: measured width of the 26 lowercase letters / 26.
        let lower = "abcdefghijklmnopqrstuvwxyz"
        let (w, _) = lower.withCString { cptr in
            measure(f, bytes: cptr, len: Int32(lower.utf8.count))
        }
        return Int32((Double(w) / 26.0).rounded())
    }

    private static func maxCharWidth(_ ctFont: CTFont, f: FontSpec, unitsPerEm: UInt32, sizePt: CGFloat) -> Int32 {
        if let raw = readHheaAdvanceWidthMax(ctFont), unitsPerEm > 0 {
            let scaled = Double(raw) * Double(sizePt) / Double(unitsPerEm) * 20.0
            return Int32(scaled.rounded())
        }
        // Fallback: the widest single printable-ASCII glyph advance.
        var maxAdvance: Int32 = 0
        for scalar in UInt8(0x20)...UInt8(0x7E) {
            let ch = String(UnicodeScalar(scalar))
            let (w, _) = ch.withCString { cptr in
                measure(f, bytes: cptr, len: Int32(ch.utf8.count))
            }
            if w > maxAdvance { maxAdvance = w }
        }
        return maxAdvance
    }

    /// Reads big-endian `int16_t xAvgCharWidth` at OS/2 table byte offset 2.
    /// `nil` if the table is absent or too short.
    private static func readOS2XAvgCharWidth(_ ctFont: CTFont) -> Int16? {
        guard let data = CTFontCopyTable(ctFont, CTFontTableTag(kCTFontTableOS2), CTFontTableOptions()) as Data?,
              data.count >= 4 else { return nil }
        return data.withUnsafeBytes { raw -> Int16 in
            let hi = Int16(raw[2]), lo = Int16(raw[3])
            return Int16(bitPattern: UInt16((UInt16(hi) << 8) | UInt16(lo)))
        }
    }

    /// Reads big-endian `uint16_t advanceWidthMax` at hhea table byte offset
    /// 10. `nil` if the table is absent or too short.
    private static func readHheaAdvanceWidthMax(_ ctFont: CTFont) -> UInt16? {
        guard let data = CTFontCopyTable(ctFont, CTFontTableTag(kCTFontTableHhea), CTFontTableOptions()) as Data?,
              data.count >= 12 else { return nil }
        return data.withUnsafeBytes { raw -> UInt16 in
            let hi = UInt16(raw[10]), lo = UInt16(raw[11])
            return (hi << 8) | lo
        }
    }

    // MARK: - shared line builder

    /// Decode raw bytes (CP-1252, falling back to ISO Latin-1 -- both are 1:1
    /// for ASCII) and build a plain (no color/attrs needed for measurement)
    /// CTLine, matching CGCanvas's own decode convention (`decodeBytes`).
    private static func makeLine(_ ctFont: CTFont, bytes: UnsafePointer<CChar>?, len: Int32) -> CTLine? {
        guard let bytes = bytes, len > 0 else { return nil }
        let buf = UnsafeBufferPointer(start: bytes, count: Int(len))
        let unsigned = buf.map { UInt8(bitPattern: $0) }
        let text = String(bytes: unsigned, encoding: .windowsCP1252)
            ?? String(bytes: unsigned, encoding: .isoLatin1)
            ?? ""
        guard !text.isEmpty else { return nil }
        let attrs: [CFString: Any] = [kCTFontAttributeName: ctFont]
        let attr = CFAttributedStringCreate(nil, text as CFString, attrs as CFDictionary)!
        return CTLineCreateWithAttributedString(attr)
    }
}
