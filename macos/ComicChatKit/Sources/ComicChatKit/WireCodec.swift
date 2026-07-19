import Foundation

/// The two wire-text encodings the engine can be configured with (mirrors
/// `cc_session_config.encoding`: 0 = CP-1252, 1 = UTF-8 — spec §4.5). The
/// engine itself never transcodes; it treats every `const char*` payload as
/// opaque bytes and hands them back verbatim through `cc_proto_event`. Swift
/// is the only layer that needs a `String`, so `WireCodec` does the
/// byte<->String conversion at the C boundary.
public enum WireEncoding: Int32, Sendable {
    case cp1252 = 0
    case utf8 = 1
}

/// CP-1252 (Windows-1252) <-> Unicode scalar tables, and the `WireCodec`
/// namespace that picks CP-1252 or UTF-8 transcoding based on a
/// `WireEncoding`.
///
/// CP-1252 agrees with ASCII/Latin-1 everywhere except the C1 control block
/// 0x80-0x9F, which Windows-1252 repurposes for printable characters (curly
/// quotes, dashes, the euro sign, etc. — "smart quotes" being the most
/// visible example). Byte 0x93 is the sharpest test case: as CP-1252 it is
/// U+201C LEFT DOUBLE QUOTATION MARK, but as a UTF-8 lead/continuation byte
/// (0x93 = 0b10010011) it is an invalid *lone* continuation byte with no
/// leading byte — decoding it as UTF-8 must fail. That divergence is exactly
/// why the engine needs the caller-supplied `encoding` flag instead of
/// guessing.
public enum WireCodec {
    /// `table[byte]` is the Unicode scalar CP-1252 byte `byte` decodes to.
    /// Bytes 0x00-0x7F and 0xA0-0xFF are identical to Latin-1/Unicode; only
    /// the 0x80-0x9F block differs (the Windows-1252 "C1 replacements").
    /// Source: the standard Windows-1252 code page table. 0x81, 0x8D, 0x8F,
    /// 0x90, 0x9D are undefined in Windows-1252 proper; we map them to their
    /// Latin-1/C1-control codepoints (matching what MultiByteToWideChar does
    /// on Windows for those five slots: it just passes the C1 control
    /// through unchanged) so every byte value has a defined, round-trippable
    /// mapping and the table has no holes.
    public static let cp1252ToUnicode: [UInt32] = {
        var table = [UInt32](repeating: 0, count: 256)
        for b in 0..<256 { table[b] = UInt32(b) }
        let c1: [Int: UInt32] = [
            0x80: 0x20AC, // €
            0x81: 0x0081, // undefined -> pass-through
            0x82: 0x201A, // ‚
            0x83: 0x0192, // ƒ
            0x84: 0x201E, // „
            0x85: 0x2026, // …
            0x86: 0x2020, // †
            0x87: 0x2021, // ‡
            0x88: 0x02C6, // ˆ
            0x89: 0x2030, // ‰
            0x8A: 0x0160, // Š
            0x8B: 0x2039, // ‹
            0x8C: 0x0152, // Œ
            0x8D: 0x008D, // undefined -> pass-through
            0x8E: 0x017D, // Ž
            0x8F: 0x008F, // undefined -> pass-through
            0x90: 0x0090, // undefined -> pass-through
            0x91: 0x2018, // '
            0x92: 0x2019, // '
            0x93: 0x201C, // "
            0x94: 0x201D, // "
            0x95: 0x2022, // •
            0x96: 0x2013, // –
            0x97: 0x2014, // —
            0x98: 0x02DC, // ˜
            0x99: 0x2122, // ™
            0x9A: 0x0161, // š
            0x9B: 0x203A, // ›
            0x9C: 0x0153, // œ
            0x9D: 0x009D, // undefined -> pass-through
            0x9E: 0x017E, // ž
            0x9F: 0x0178, // Ÿ
        ]
        for (byte, scalar) in c1 { table[byte] = scalar }
        return table
    }()

    /// Reverse map: Unicode scalar value -> CP-1252 byte, built from
    /// `cp1252ToUnicode`. Scalars with no CP-1252 representation are absent.
    public static let unicodeToCp1252: [UInt32: UInt8] = {
        var map = [UInt32: UInt8]()
        for byte in 0..<256 {
            map[cp1252ToUnicode[byte]] = UInt8(byte)
        }
        return map
    }()

    /// Decode `len` bytes at `bytes` (a `cc_proto_event` field's raw wire
    /// bytes) into a `String`, per `encoding`. `bytes` may be NULL (empty
    /// string) — `cc_proto_event` string fields are documented as valid only
    /// for the callback's duration, so this copies immediately.
    public static func decode(_ bytes: UnsafePointer<CChar>?, len: Int32,
                              encoding: WireEncoding) -> String {
        guard let bytes = bytes, len > 0 else { return "" }
        let buf = UnsafeBufferPointer(start: bytes, count: Int(len))
        let unsigned = buf.map { UInt8(bitPattern: $0) }
        return decode(unsigned, encoding: encoding)
    }

    /// Decode a NUL-terminated C string (`const char*`) into a `String`, per
    /// `encoding`. Convenience for event fields that are plain `const char*`
    /// rather than an explicit (bytes, len) pair.
    public static func decode(_ cString: UnsafePointer<CChar>?,
                              encoding: WireEncoding) -> String {
        guard let cString = cString else { return "" }
        let len = Int32(strlen(cString))
        return decode(cString, len: len, encoding: encoding)
    }

    /// Decode a raw byte buffer per `encoding`.
    public static func decode(_ bytes: [UInt8], encoding: WireEncoding) -> String {
        switch encoding {
        case .utf8:
            // UTF-8 is pass-through: the engine's bytes ARE UTF-8 already.
            // Invalid sequences fall back to CP-1252 so no input is ever
            // silently dropped (matches CGCanvas.decodeBytes's fallback
            // posture).
            return String(bytes: bytes, encoding: .utf8) ?? decodeCp1252(bytes)
        case .cp1252:
            return decodeCp1252(bytes)
        }
    }

    private static func decodeCp1252(_ bytes: [UInt8]) -> String {
        var scalars = String.UnicodeScalarView()
        scalars.reserveCapacity(bytes.count)
        for b in bytes {
            if let scalar = Unicode.Scalar(cp1252ToUnicode[Int(b)]) {
                scalars.append(scalar)
            }
        }
        return String(scalars)
    }

    /// Strips mIRC inline-formatting control codes from already-decoded
    /// display text. RECORDED DEVIATION (Plan 4b live-fix 6, Fix 2;
    /// modern-usability, coordinator-sanctioned) -- NOT a fidelity fix.
    ///
    /// SCOPE AMENDMENT (live-fix batch review, I1): because this runs at
    /// `ProtocolEvent.from` -- the single event-construction site -- the
    /// stripped text becomes the CANONICAL `ProtocolEvent`, so the SAVED
    /// transcript (`ConversationFile.events`) is display-normalized too: raw
    /// mIRC codes are not preserved on the production save path (the rig's
    /// wire captures remain the only raw record). Wire bytes, outbound text,
    /// and annotations are untouched. This is deliberate and recorded, not
    /// an oversight.
    ///
    /// FIDELITY CHECK (p4b-bugB-diagnosis.md's mandated first step, verified
    /// against the read-only original at v2.5-beta-1-modern/): the 1998
    /// client does NOT strip mIRC control codes from inbound message text.
    /// `ProcessLine` (chatdoc.cpp:447-466) forwards `szMesg` to `AddLine`
    /// unmodified; `CChatDoc::AddLine` (chatdoc.cpp:328-340) forwards it to
    /// `CUnitPanelPage::AddLine` -> `MakeBalloon` (panel.cpp:1036-1136)
    /// unmodified; `ProcessSay` (protsupp.cpp:1545-1922), the inbound PRIVMSG
    /// handler, only does CTCP low-level UNquoting (`\r`/`\n`, protsupp.cpp:
    /// 1563) and this port's own "(#...)" annotation-block parsing -- no
    /// 0x03/0x02/0x1F/0x16/0x0F handling anywhere in that chain.
    /// `protsupp.cpp` has NO `StripFormatting`-shaped function; `format.cpp`'s
    /// `chCtlColor`/`chCtlBold`/etc. scheme is this client's OWN outbound
    /// `^`-prefixed line-wrap/RTF-export formatting for locally composed
    /// text, unrelated to mIRC's wire-level codes. So a literal mIRC control
    /// byte in inbound text reaches the original's balloon/RTF renderer RAW
    /// -- e.g. the captured "\x034 Hi JefPober" (mIRC color 4, no bg) would
    /// render as whatever glyph GDI maps 0x03 to, immediately followed by a
    /// bare, uncolored "4". The port's current passthrough behavior (keep
    /// the bytes, let the renderer show whatever it shows) is therefore
    /// ALREADY faithful -- this fix does not touch WIRE bytes, outbound
    /// text, or annotations; it only cleans up the copy that reaches
    /// on-screen UI/strip rendering (`ProtocolEvent.from`'s `.text`/
    /// `.action`/`.whisper` text field, applied AFTER `decode` above), since
    /// CoreText silently drops the raw control byte and leaves an orphaned,
    /// confusing digit like "4 Hi JefPober" on screen -- worse than either
    /// the original's own rendering or a clean strip.
    ///
    /// Grammar stripped (mIRC's documented inline-formatting codes):
    ///   0x03 ("\u{03}") color, optionally followed by `\d{1,2}(,\d{1,2})?`
    ///        (foreground[,background], 1-2 digits each)
    ///   0x02 bold, 0x1F underline, 0x16 reverse, 0x0F reset -- all lone,
    ///        no following digits.
    /// The code itself is removed; any character(s) immediately after it
    /// (e.g. the space in "\x034 Hi ...") are left exactly as-is -- only the
    /// control byte (and a color code's own digit run) are consumed, so
    /// "\x034 Hi JefPober" strips to " Hi JefPober" (leading space kept,
    /// documented choice: the space is real, separately-typed content the
    /// color code merely prefixed, not part of the code's own grammar).
    public static func stripMircFormatting(_ text: String) -> String {
        guard text.utf8.contains(where: { $0 == 0x03 || $0 == 0x02 || $0 == 0x1F || $0 == 0x16 || $0 == 0x0F }) else {
            return text
        }
        var out = String.UnicodeScalarView()
        var scalars = Substring(text).unicodeScalars[...]
        while let c = scalars.first {
            switch c.value {
            case 0x03:
                scalars.removeFirst()
                // Optional foreground digits (1-2).
                var digits = 0
                while digits < 2, let d = scalars.first, ("0"..."9").contains(Character(d)) {
                    scalars.removeFirst(); digits += 1
                }
                // Optional ",background" digits (1-2), only if a foreground
                // color was actually present (mIRC's own grammar: a bare
                // "\x03," is not a valid color-with-background lead-in).
                if digits > 0, let comma = scalars.first, comma == "," {
                    let save = scalars
                    scalars.removeFirst()
                    var bgDigits = 0
                    while bgDigits < 2, let d = scalars.first, ("0"..."9").contains(Character(d)) {
                        scalars.removeFirst(); bgDigits += 1
                    }
                    if bgDigits == 0 { scalars = save }   // not actually a bg run; restore the comma
                }
            case 0x02, 0x1F, 0x16, 0x0F:
                scalars.removeFirst()
            default:
                out.append(c)
                scalars.removeFirst()
            }
        }
        return String(out)
    }

    /// Encode a `String` to wire bytes per `encoding`, for outbound sends.
    public static func encode(_ s: String, encoding: WireEncoding) -> [UInt8] {
        switch encoding {
        case .utf8:
            return Array(s.utf8)
        case .cp1252:
            var out = [UInt8]()
            out.reserveCapacity(s.unicodeScalars.count)
            for scalar in s.unicodeScalars {
                if let byte = unicodeToCp1252[scalar.value] {
                    out.append(byte)
                } else if scalar.value < 0x100 {
                    // Not a defined CP-1252 mapping but fits a byte anyway
                    // (shouldn't happen given the table has no holes, kept
                    // as a defensive fallback).
                    out.append(UInt8(scalar.value))
                } else {
                    out.append(0x3F) // '?' for un-encodable scalars
                }
            }
            return out
        }
    }
}
