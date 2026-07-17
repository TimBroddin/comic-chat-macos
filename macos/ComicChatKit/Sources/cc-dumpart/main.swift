import Foundation
import ComicChatKit

// cc-dumpart <art-dir> — builds the art catalog (see Catalog.swift) for
// every *.avb/*.bgb file directly inside <art-dir> and prints it as pretty,
// deterministically-ordered JSON on stdout. Used both ad hoc and to
// regenerate the golden fixture consumed by GoldenCatalogTests.swift:
//
//   swift run cc-dumpart <art-dir> > Tests/ComicChatKitTests/Fixtures/comicart-catalog.json

guard CommandLine.arguments.count > 1 else {
    FileHandle.standardError.write(Data("usage: cc-dumpart <art-dir>\n".utf8))
    exit(64) // EX_USAGE
}

let artDir = CommandLine.arguments[1]

do {
    let catalog = try buildCatalog(artDir: artDir)
    let encoder = JSONEncoder()
    encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
    let data = try encoder.encode(catalog)
    FileHandle.standardOutput.write(data)
    FileHandle.standardOutput.write(Data("\n".utf8))
} catch {
    FileHandle.standardError.write(Data("cc-dumpart: \(error)\n".utf8))
    exit(1)
}
