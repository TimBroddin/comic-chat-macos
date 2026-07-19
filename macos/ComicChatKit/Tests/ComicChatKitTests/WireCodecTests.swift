import Testing
import Foundation
@testable import ComicChatKit

// WireCodec is pure (no engine-global state touched), so these need no
// serialization -- unlike EngineGlobalStateSelfTests's descendants.

@Test func cp1252DecodesByte0x93AsLeftDoubleQuote() {
    // 0x93 is the sharpest divergence case (spec §4.5 / the Task 7 brief):
    // as CP-1252 it's U+201C LEFT DOUBLE QUOTATION MARK ("smart quote"), but
    // as a lone UTF-8 byte it's an invalid continuation byte with no lead
    // byte (0x93 = 0b10010011) -- decoding the SAME byte through the two
    // encodings must diverge.
    let bytes: [UInt8] = [0x93]
    let decoded = WireCodec.decode(bytes, encoding: .cp1252)
    #expect(decoded == "\u{201C}")
    #expect(decoded.unicodeScalars.first?.value == 0x201C)
}

@Test func utf8RejectsLoneByte0x93AndFallsBackToCp1252() {
    // WireCodec.decode(.utf8) is documented to fall back to CP-1252 on
    // invalid UTF-8 (matching CGCanvas.decodeBytes's established fallback
    // posture) rather than silently dropping the byte -- so a lone 0x93 byte
    // decoded as "UTF-8" still yields the CP-1252 smart quote, proving the
    // fallback path actually engages (as opposed to e.g. silently returning
    // empty string, which would also technically not equal the CP-1252
    // decode by coincidence).
    let bytes: [UInt8] = [0x93]
    let decoded = WireCodec.decode(bytes, encoding: .utf8)
    #expect(decoded == "\u{201C}")
}

@Test func utf8PassesThroughValidMultiByteSequences() {
    // A genuinely valid UTF-8 sequence (e.g. "café", where é is 2 bytes:
    // 0xC3 0xA9) must decode correctly under .utf8 and WOULD NOT match a
    // CP-1252 decode of the same bytes -- proving UTF-8 mode doesn't just
    // always fall back to CP-1252.
    let s = "café"
    let bytes = Array(s.utf8)
    let decodedUtf8 = WireCodec.decode(bytes, encoding: .utf8)
    #expect(decodedUtf8 == s)
    let decodedCp1252 = WireCodec.decode(bytes, encoding: .cp1252)
    #expect(decodedCp1252 != s, "the 2-byte UTF-8 encoding of é must NOT also be valid CP-1252 for 'café' -- otherwise this test can't distinguish the two decode paths")
}

@Test func cp1252RoundTripsAllAsciiAndC1ReplacementBytes() {
    // Round-trip every byte 0x00-0xFF through decode then encode and recover
    // the original byte, proving the table has no holes and the reverse map
    // is a true inverse of the forward map.
    for byte in 0...255 {
        let original = [UInt8(byte)]
        let decoded = WireCodec.decode(original, encoding: .cp1252)
        let reencoded = WireCodec.encode(decoded, encoding: .cp1252)
        #expect(reencoded == original, "byte 0x\(String(byte, radix: 16)) did not round-trip: decoded to \(decoded.unicodeScalars.map { $0.value }), re-encoded to \(reencoded)")
    }
}

@Test func cp1252EncodeAsciiIsIdentity() {
    let s = "Hello, World! 123"
    let encoded = WireCodec.encode(s, encoding: .cp1252)
    #expect(encoded == Array(s.utf8))  // ASCII agrees with UTF-8 byte-for-byte
}

@Test func decodeFromCStringHandlesNilAndEmpty() {
    #expect(WireCodec.decode(nil, encoding: .cp1252) == "")
    #expect(WireCodec.decode(UnsafePointer<CChar>?.none, len: 0, encoding: .cp1252) == "")
}

// Plan 4b live-fix 6, Fix 2 (RECORDED DEVIATION, modern-usability -- see
// WireCodec.stripMircFormatting's doc comment for the fidelity check: the
// 1998 original does NOT strip these codes; this is a Swift-display-boundary
// deviation, never applied to wire bytes/outbound text/annotations).
@Test func stripMircFormattingRemovesTheExactCapturedOrphanedColorDigit() {
    // The exact live wire bytes from the task/diagnosis: 0x03 0x34 (color
    // code "4", no background) followed by " Hi JefPober". Built from hex,
    // not retyped, per the task's instruction.
    let bytes: [UInt8] = [0x03, 0x34, 0x20, 0x48, 0x69, 0x20, 0x4A, 0x65, 0x66, 0x50, 0x6F, 0x62, 0x65, 0x72]
    let decoded = WireCodec.decode(bytes, encoding: .cp1252)
    #expect(decoded == "\u{03}4 Hi JefPober")   // decode itself must NOT strip (wire bytes stay intact)

    let stripped = WireCodec.stripMircFormatting(decoded)
    // Documented choice: strip the code (and its digit run) only; the space
    // that followed it in the original text is real content, not part of
    // the color code's own grammar, so it survives -- " Hi JefPober" (one
    // leading space), not "Hi JefPober".
    #expect(stripped == " Hi JefPober")
}

@Test func stripMircFormattingHandlesEveryControlCode() {
    // 0x02 bold, 0x1F underline, 0x16 reverse, 0x0F reset -- all lone, no
    // following digits to consume.
    #expect(WireCodec.stripMircFormatting("\u{02}bold\u{02}") == "bold")
    #expect(WireCodec.stripMircFormatting("\u{1F}under\u{1F}") == "under")
    #expect(WireCodec.stripMircFormatting("\u{16}rev\u{16}") == "rev")
    #expect(WireCodec.stripMircFormatting("plain\u{0F}reset") == "plainreset")
}

@Test func stripMircFormattingHandlesColorWithBackground() {
    // "\x03<fg>,<bg>" -- 1-2 digits each side of the comma.
    #expect(WireCodec.stripMircFormatting("\u{03}4,8red on gray") == "red on gray")
    #expect(WireCodec.stripMircFormatting("\u{03}12,08two-digit both") == "two-digit both")
    #expect(WireCodec.stripMircFormatting("\u{03}bare color, no digits") == "bare color, no digits")
}

@Test func stripMircFormattingLeavesPlainTextUntouched() {
    let s = "Hello, World! No control codes here."
    #expect(WireCodec.stripMircFormatting(s) == s)
}

@Test func stripMircFormattingDoesNotConsumeATrailingCommaWithNoBackgroundDigits() {
    // "\x034," -- a foreground color immediately followed by a bare comma
    // with NO background digits after it is not a valid "fg,bg" pair per
    // mIRC's own grammar; the comma is ordinary text and must survive.
    #expect(WireCodec.stripMircFormatting("\u{03}4,hi") == ",hi")
}
