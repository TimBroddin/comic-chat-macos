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
