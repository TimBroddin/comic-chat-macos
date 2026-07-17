import Foundation
import ComicChatKit

// cc-dumpart <art-dir> — builds the art catalog (see Catalog.swift) for
// every *.avb/*.bgb file directly inside <art-dir> and prints it as pretty,
// deterministically-ordered JSON on stdout. Used both ad hoc and to
// regenerate the golden fixture consumed by GoldenCatalogTests.swift:
//
//   swift run cc-dumpart <art-dir> > Tests/ComicChatKitTests/Fixtures/comicart-catalog.json
//
// cc-dumpart --png <file.avb> <poseIndex> <out.png> — exports a single pose
// as a PNG file.

let args = CommandLine.arguments

do {
    if args.count >= 2 && args[1] == "--png" {
        guard args.count == 5 else {
            FileHandle.standardError.write(Data("usage: cc-dumpart --png <file.avb> <poseIndex> <out.png>\n".utf8))
            exit(64) // EX_USAGE
        }
        let filePath = args[2]
        guard let poseIndex = Int(args[3]) else {
            FileHandle.standardError.write(Data("cc-dumpart: poseIndex must be an integer\n".utf8))
            exit(64)
        }
        let outPath = args[4]

        let avatar = try AvatarFile(path: filePath)
        let image = try avatar.poseImage(poseIndex)
        try exportPNG(artImage: image, toPath: outPath)
    } else {
        guard args.count > 1 else {
            FileHandle.standardError.write(Data("usage: cc-dumpart <art-dir>\n".utf8))
            FileHandle.standardError.write(Data("       cc-dumpart --png <file.avb> <poseIndex> <out.png>\n".utf8))
            exit(64) // EX_USAGE
        }
        let artDir = args[1]
        let catalog = try buildCatalog(artDir: artDir)
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        let data = try encoder.encode(catalog)
        FileHandle.standardOutput.write(data)
        FileHandle.standardOutput.write(Data("\n".utf8))
    }
} catch {
    FileHandle.standardError.write(Data("cc-dumpart: \(error)\n".utf8))
    exit(1)
}
