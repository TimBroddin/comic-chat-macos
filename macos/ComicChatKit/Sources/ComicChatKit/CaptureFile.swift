import Foundation

/// Parses the Wine capture rig's JSONL format
/// (`.superpowers/rig/capture-proxy.ts`, documented in
/// docs/superpowers/plans/2026-07-18-plan3-discovery/wine-capture-rig.md):
/// one JSON object per line, `{"t":<ms>,"dir":"c2s"|"s2c"|"meta","hex":"...",
/// "latin1":"..."}` (or `"note":"..."` for `meta` lines). This is the
/// production-code counterpart of the test target's `CaptureEvent`
/// (`Tests/ComicChatKitTests/Support/CaptureReplay.swift`, Plan 3 Task 8) —
/// same parsing logic, kept in the library so `cc-dumpart --replay` (a
/// separate SwiftPM module that cannot import test-target sources) can read
/// a capture file too.
public struct CaptureLine: Sendable {
    public enum Direction: String, Sendable {
        case c2s, s2c, meta
    }

    public let direction: Direction
    /// Decoded from `hex`; empty for `meta` lines.
    public let bytes: Data
    /// Readable view (`latin1`), or the `meta` line's `note`.
    public let latin1: String

    private struct Raw: Decodable {
        let t: Int?
        let dir: String
        let hex: String?
        let latin1: String?
        let note: String?
    }

    public enum CaptureFileError: Error, CustomStringConvertible {
        case notUTF8
        case unknownDirection(String)
        case badHex(String)
        public var description: String {
            switch self {
            case .notUTF8: return "capture file is not valid UTF-8"
            case .unknownDirection(let d): return "unknown \"dir\" value: \(d)"
            case .badHex(let h): return "malformed hex string: \(h)"
            }
        }
    }

    /// Parses one `CaptureLine` per non-blank line, in file order. Throws on
    /// malformed JSON, an unrecognized `dir`, or non-hex `hex` content.
    public static func parse(jsonl data: Data) throws -> [CaptureLine] {
        guard let text = String(data: data, encoding: .utf8) else {
            throw CaptureFileError.notUTF8
        }
        var lines: [CaptureLine] = []
        for line in text.split(separator: "\n", omittingEmptySubsequences: true) {
            let lineData = Data(line.utf8)
            let raw = try JSONDecoder().decode(Raw.self, from: lineData)
            guard let direction = Direction(rawValue: raw.dir) else {
                throw CaptureFileError.unknownDirection(raw.dir)
            }
            let bytes: Data
            if let hex = raw.hex {
                guard let decoded = Data(hexEncodedCaptureBytes: hex) else {
                    throw CaptureFileError.badHex(hex)
                }
                bytes = decoded
            } else {
                bytes = Data()
            }
            lines.append(CaptureLine(direction: direction, bytes: bytes,
                                     latin1: raw.latin1 ?? raw.note ?? ""))
        }
        return lines
    }

    /// Convenience: load + parse a capture file from disk.
    public static func parse(jsonlFile url: URL) throws -> [CaptureLine] {
        try parse(jsonl: Data(contentsOf: url))
    }
}

private extension Data {
    /// Decodes a lowercase (or uppercase) hex string with no separators, as
    /// produced by the rig's `Buffer.toString('hex')`-equivalent logger. `nil`
    /// on odd length or a non-hex-digit character.
    init?(hexEncodedCaptureBytes hex: String) {
        let chars = Array(hex.utf8)
        guard chars.count % 2 == 0 else { return nil }
        var out = [UInt8]()
        out.reserveCapacity(chars.count / 2)
        var i = 0
        while i < chars.count {
            guard let hi = Data.hexNibbleCapture(chars[i]), let lo = Data.hexNibbleCapture(chars[i + 1]) else {
                return nil
            }
            out.append((hi << 4) | lo)
            i += 2
        }
        self = Data(out)
    }

    static func hexNibbleCapture(_ c: UInt8) -> UInt8? {
        switch c {
        case 0x30...0x39: return c - 0x30           // '0'-'9'
        case 0x61...0x66: return c - 0x61 + 10      // 'a'-'f'
        case 0x41...0x46: return c - 0x41 + 10      // 'A'-'F'
        default: return nil
        }
    }
}
