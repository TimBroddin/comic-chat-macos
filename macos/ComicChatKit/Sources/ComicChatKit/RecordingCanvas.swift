import Foundation
import cchat_engine

/// A `Canvas` that draws nothing and instead appends one human-readable line to
/// `log` per op — the Swift twin of the C++ `CCRecordingCanvas`
/// (Sources/cchat-engine/bridge/cc_recording_canvas.{h,cpp}). The log grammar is
/// a STABLE CONTRACT shared by both recorders; this class reproduces it
/// LINE-FOR-LINE so the same conversation composed through the C engine yields
/// byte-identical logs whether the canvas is the C++ recorder (selftests) or
/// this one (`StripTests.stripSnapshot`). That equality is what proves the
/// Swift `cc_canvas` bridge (`CanvasBox`) carries every op faithfully.
///
/// Deterministic fake metrics (no real font engine consulted), matching the C++
/// recorder exactly so it can also serve as the LAYOUT-TIME metrics canvas
/// (`cc_set_metrics_canvas`): every byte is 120 twips wide regardless of font or
/// value; height is always 240. font_metrics is a fixed table.
public final class RecordingCanvas: Canvas {
    public private(set) var log: [String] = []

    public init() {}

    // MARK: color/coordinate formatting (must match cc_recording_canvas.cpp)

    /// COLORREF (0x00BBGGRR) -> "RRGGBB" display-order hex, 6 uppercase digits.
    /// R = low byte, G = byte 1, B = byte 2 (GDI GetRValue/GetGValue/GetBValue).
    private static func formatColor(_ color: UInt32) -> String {
        let r = color & 0xFF
        let g = (color >> 8) & 0xFF
        let b = (color >> 16) & 0xFF
        return String(format: "%02X%02X%02X", r, g, b)
    }

    private static func formatXY(_ x: Int32, _ y: Int32) -> String {
        "\(x),\(y)"
    }

    // MARK: Canvas

    public func measureText(_ f: FontSpec, bytes: UnsafePointer<CChar>?, len: Int32) -> (w: Int32, h: Int32) {
        (w: len * 120, h: 240)
    }

    public func fontMetrics(_ f: FontSpec) -> TextMetrics {
        TextMetrics(height: 240, ascent: 190, descent: 50,
                    internalLeading: 40, externalLeading: 20,
                    aveCharWidth: 120, maxCharWidth: 240)
    }

    public func drawText(_ f: FontSpec, x: Int32, y: Int32, color: UInt32,
                         bkOpaque: Bool, bkColor: UInt32,
                         bytes: UnsafePointer<CChar>?, len: Int32) {
        // Bytes are printed verbatim between quotes, no escaping — exactly like
        // the C++ `std::string(bytes, bytes+len)`. Reconstruct the string from
        // the raw bytes so multi-byte / high-bit CP-1252 content survives
        // byte-for-byte. .isoLatin1 maps every byte 0x00-0xFF 1:1 to a scalar,
        // so the rendered text matches the C++ recorder's raw-byte output.
        let text = Self.bytesToString(bytes, len: len)
        log.append("text \(Self.formatXY(x, y)) color=\(Self.formatColor(color)) \"\(text)\"")
    }

    public func fillRect(l: Int32, t: Int32, r: Int32, b: Int32, color: UInt32) {
        log.append("rect \(l),\(t),\(r),\(b) fill=\(Self.formatColor(color))")
    }

    public func drawImage(_ img: UnsafePointer<cc_image>?,
                          dl: Int32, dt: Int32, dr: Int32, db: Int32,
                          sl: Int32, st: Int32, sr: Int32, sb: Int32) {
        log.append("image \(dl),\(dt),\(dr),\(db) src=\(sl),\(st),\(sr),\(sb)")
    }

    public func path(_ pts: UnsafePointer<cc_path_pt>?, n: Int32,
                     doFill: Bool, fillColor: UInt32,
                     doStroke: Bool, strokeColor: UInt32,
                     strokeWidth: Int32, dashed: Bool) {
        var entries: [String] = []
        var i: Int32 = 0
        while i < n, let pts = pts {
            let pt = pts[Int(i)]
            switch pt.verb {
            case PathVerb.move.rawValue:
                entries.append("M \(Self.formatXY(pt.x, pt.y))")
                i += 1
            case PathVerb.line.rawValue:
                entries.append("L \(Self.formatXY(pt.x, pt.y))")
                i += 1
            case PathVerb.cubic.rawValue:
                // Three consecutive CC_PATH_CUBIC entries render as one C triple.
                // Match the C++ truncation guard exactly: if the triple would
                // read past the end, stop parsing (no partial C entry emitted).
                if i + 2 >= n {
                    i = n
                    break
                }
                let p1 = pts[Int(i)]
                let p2 = pts[Int(i + 1)]
                let p3 = pts[Int(i + 2)]
                entries.append("C \(Self.formatXY(p1.x, p1.y)) " +
                               "\(Self.formatXY(p2.x, p2.y)) " +
                               "\(Self.formatXY(p3.x, p3.y))")
                i += 3
            case PathVerb.close.rawValue:
                entries.append("Z")
                i += 1
            default:
                // Unknown verb: skip one entry (matches the C++ default branch).
                i += 1
            }
        }

        // NOTE the trailing space after dashed=%d and before the "[" — the C++
        // format string is "... dashed=%d " then appends "[" + entries + "]".
        let head = "path n=\(n) fill=\(doFill ? 1 : 0) fillc=\(Self.formatColor(fillColor)) " +
                   "stroke=\(doStroke ? 1 : 0) strokec=\(Self.formatColor(strokeColor)) " +
                   "w=\(strokeWidth) dashed=\(dashed ? 1 : 0) "
        log.append(head + "[" + entries.joined(separator: " ") + "]")
    }

    public func clipPush(l: Int32, t: Int32, r: Int32, b: Int32) {
        log.append("clip+ \(l),\(t),\(r),\(b)")
    }

    public func clipPop() {
        log.append("clip-")
    }

    public func isPrinting() -> Bool { false }

    // MARK: helpers

    /// Reconstruct a String from `len` raw signed bytes 1:1 via ISO Latin-1
    /// (every byte 0x00-0xFF maps to exactly one scalar), matching the C++
    /// recorder's verbatim-byte behavior. Latin-1 is used deliberately over a
    /// CP-1252 decode here: the recorder log is a raw-byte transcript, not a
    /// display rendering, so a lossless 1:1 byte->scalar mapping is what makes
    /// the two recorders' logs compare equal byte-for-byte.
    static func bytesToString(_ bytes: UnsafePointer<CChar>?, len: Int32) -> String {
        guard let bytes = bytes, len > 0 else { return "" }
        let buf = UnsafeBufferPointer(start: bytes, count: Int(len))
        let unsigned = buf.map { UInt8(bitPattern: $0) }
        return String(bytes: unsigned, encoding: .isoLatin1) ?? ""
    }
}
