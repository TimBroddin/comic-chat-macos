import Testing
import Foundation
import cchat_engine

@Test func engineSelfTestsPass() {
    #expect(cc_run_selftests() == 0)
}

@Test func smokeLoadAnna() throws {
    // comicart/ lives at the repo root; locate it relative to this source file.
    // NOTE: the brief's original 4-call chain landed one level short (at
    // macos/, not the repo root) — #filePath's first deletingLastPathComponent()
    // strips the filename itself (SelfTests.swift -> ComicChatKitTests/), so a
    // 5th call is needed to also strip "macos". Verified empirically (printed
    // the resolved path/fileExists before and after this fix).
    let repoRoot = URL(fileURLWithPath: #filePath)
        .deletingLastPathComponent()  // SelfTests.swift -> ComicChatKitTests
        .deletingLastPathComponent()  // ComicChatKitTests -> Tests
        .deletingLastPathComponent()  // Tests -> ComicChatKit
        .deletingLastPathComponent()  // ComicChatKit -> macos
        .deletingLastPathComponent()  // macos -> repo root
    let anna = repoRoot.appendingPathComponent("v2.5-beta-1-modern/comicart/anna.avb").path
    var name = [CChar](repeating: 0, count: 256)
    var poses: Int32 = 0
    let rc = cc_smoke_load_avatar(anna, &name, 256, &poses)
    #expect(rc == 0)
    #expect(String(cString: name).isEmpty == false)
    #expect(poses > 0)
}
