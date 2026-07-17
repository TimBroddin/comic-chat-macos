import Testing
import Foundation
@testable import ComicChatKit

// CatalogEntry and buildCatalog(artDir:) live in Sources/ComicChatKit/Catalog.swift
// (single definition, shared by this test and cc-dumpart).

@Test func comicartMatchesGoldenCatalog() throws {
    // 5 deletions to reach the repo root (see Task 5 note)
    let repoRoot = URL(fileURLWithPath: #filePath)
        .deletingLastPathComponent().deletingLastPathComponent()
        .deletingLastPathComponent().deletingLastPathComponent()
        .deletingLastPathComponent()
    let artDir = ProcessInfo.processInfo.environment["CC_COMICART_DIR"]
        ?? repoRoot.appendingPathComponent("v2.5-beta-1-modern/comicart").path
    let golden = try JSONDecoder().decode([CatalogEntry].self, from: Data(contentsOf:
        Bundle.module.url(forResource: "comicart-catalog", withExtension: "json", subdirectory: "Fixtures")!))
    let built = try buildCatalog(artDir: artDir)   // same walk cc-dumpart uses
    #expect(built.count == 32)
    #expect(built == golden)
}
