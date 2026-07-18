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
