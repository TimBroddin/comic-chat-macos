import Foundation
import ImageIO
import UniformTypeIdentifiers
import ComicChatKit

func exportPNG(artImage: ArtImage, toPath outPath: String) throws {
    guard let cgImage = artImage.cgImage() else {
        throw NSError(domain: "PNGExport", code: 1, userInfo: [NSLocalizedDescriptionKey: "Failed to convert ArtImage to CGImage"])
    }

    let url = URL(fileURLWithPath: outPath)
    guard let destination = CGImageDestinationCreateWithURL(url as CFURL, UTType.png.identifier as CFString, 1, nil) else {
        throw NSError(domain: "PNGExport", code: 2, userInfo: [NSLocalizedDescriptionKey: "Failed to create image destination"])
    }

    CGImageDestinationAddImage(destination, cgImage, nil)
    guard CGImageDestinationFinalize(destination) else {
        throw NSError(domain: "PNGExport", code: 3, userInfo: [NSLocalizedDescriptionKey: "Failed to finalize PNG export"])
    }
}
