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
//
// cc-dumpart --strip <out.png> — renders a built-in demo comic strip (two
// comicart avatars, a 4-line conversation) through CGCanvas and writes it as a
// PNG. The Plan 2 exit-milestone demo.
//
// cc-dumpart --script <conversation.json> <out.png> — renders a user-authored
// JSON conversation to a comic-strip PNG. See StripScript.swift (ComicChatKit)
// for the JSON schema (also printed by scriptUsage() below) and the
// decode/validate/render pipeline; this file is a thin CLI shell around it.

let scriptUsage = """
usage: cc-dumpart --script <conversation.json> <out.png>

JSON schema:
{
  "backdrop": "field.bgb",
  "participants": [
    {"nick": "Anna", "avatar": "anna.avb"},
    {"nick": "Armando", "avatar": "armando.avb"}
  ],
  "lines": [
    {"speaker": "Anna", "text": "Hello there!", "mode": "say", "to": ["Armando"]},
    {"speaker": "Armando", "text": "Hmm, who is this?", "mode": "think"},
    {"speaker": "Anna", "text": "psst... it's me", "mode": "whisper", "to": ["Armando"]}
  ]
}

- "backdrop" is optional (omit for no backdrop).
- "mode" is optional, default "say"; one of: say, think, whisper, action.
- "to" is optional, default []; entries must be declared participant nicks.
- "avatar"/"backdrop": an absolute path is used as-is; a bare name (e.g.
  "anna.avb") is resolved against the bundled comicart directory. The file
  extension is required -- it is never appended automatically.
"""

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
    } else if args.count >= 2 && args[1] == "--strip" {
        guard args.count == 3 else {
            FileHandle.standardError.write(Data("usage: cc-dumpart --strip <out.png>\n".utf8))
            exit(64) // EX_USAGE
        }
        try renderDemoStrip(toPath: args[2])
    } else if args.count >= 2 && args[1] == "--script" {
        guard args.count == 4 else {
            FileHandle.standardError.write(Data((scriptUsage + "\n").utf8))
            exit(64) // EX_USAGE
        }
        try runScriptMode(jsonPath: args[2], outPath: args[3])
    } else {
        guard args.count > 1 else {
            FileHandle.standardError.write(Data("usage: cc-dumpart <art-dir>\n".utf8))
            FileHandle.standardError.write(Data("       cc-dumpart --png <file.avb> <poseIndex> <out.png>\n".utf8))
            FileHandle.standardError.write(Data("       cc-dumpart --strip <out.png>\n".utf8))
            FileHandle.standardError.write(Data("       cc-dumpart --script <conversation.json> <out.png>\n".utf8))
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
    // StripScript.ScriptError's CustomStringConvertible description (and
    // every other thrown error here) prints as a friendly, specific message.
    FileHandle.standardError.write(Data("cc-dumpart: \(error)\n".utf8))
    exit(1)
}
