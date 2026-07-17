import Testing
import Foundation
@testable import ComicChatKit

// Internal (not private): PNGExportTests.swift (Task 8) uses this helper too.
func fixture(_ name: String) -> String {
    Bundle.module.url(forResource: name, withExtension: nil, subdirectory: "Fixtures")!.path
}

@Test func avatarOpensAndDecodes() throws {
    let anna = try AvatarFile(path: fixture("anna.avb"))
    #expect(!anna.name.isEmpty)
    #expect(anna.poseCount > 0)
    #expect(!anna.poseName(0).isEmpty)
    let img = try anna.poseImage(0)
    #expect(img.width > 0 && img.height > 0)
    #expect(img.rgba.count == img.width * img.height * 4)
    // Transparency: at least one pixel must be fully transparent (avatars are
    // sprites on a transparent field) and at least one fully opaque.
    let alphas = stride(from: 3, to: img.rgba.count, by: 4).map { img.rgba[$0] }
    #expect(alphas.contains(0))
    #expect(alphas.contains(255))
}

@Test func backdropOpensAndDecodes() throws {
    let field = try BackdropFile(path: fixture("field.bgb"))
    let img = try field.image()
    #expect(img.width > 0 && img.height > 0)
    #expect(img.rgba.count == img.width * img.height * 4)
}

@Test func openMissingFileThrows() {
    #expect(throws: (any Error).self) { try AvatarFile(path: "/nonexistent.avb") }
}
