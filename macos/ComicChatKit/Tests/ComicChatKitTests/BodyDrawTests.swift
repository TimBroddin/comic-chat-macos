import Testing
import Foundation
import cchat_engine
@testable import ComicChatKit

// Plan 2 Task 7: the CBody draw path (bodycam.cpp CBody* methods, now LIVE).
// The C selftest opens a real avatar, builds a body, draws it through a
// recording canvas, and asserts the image-blit count + dest rects match
// GetBodyBox with no ASSERT traps. It needs the fixture path (the C-side
// cc_run_selftests takes none), so it runs here where `fixture()` (ArtTests.swift)
// resolves the bundled anna.avb.
@Test func bodyDrawSelfTestPasses() {
    let path = fixture("anna.avb")
    #expect(cc_run_bodydraw_selftest(path) == 0)
}
