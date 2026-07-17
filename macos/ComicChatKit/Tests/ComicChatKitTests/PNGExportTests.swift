import Testing
import Foundation
import CoreGraphics
@testable import ComicChatKit

@Test func poseConvertsToCGImage() throws {
    let anna = try AvatarFile(path: fixture("anna.avb"))
    let img = try anna.poseImage(0)
    let cg = img.cgImage()
    #expect(cg != nil)
    #expect(cg!.width == img.width && cg!.height == img.height)
    #expect(cg!.alphaInfo == .premultipliedLast || cg!.alphaInfo == .last)
}
