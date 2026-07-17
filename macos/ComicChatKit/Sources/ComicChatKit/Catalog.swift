import Foundation
import cchat_engine

/// One row of the art catalog: a single .avb or .bgb file, its parsed
/// metadata, and a CRC32 fingerprint of every decoded RGBA image it
/// produces. Shared by `cc-dumpart` (which prints it) and the golden
/// catalog test (which diffs it against a committed fixture) so there is
/// exactly one definition of "what the catalog looks like".
public struct CatalogEntry: Codable, Equatable {
    public let file: String
    public let kind: String          // "avatar" | "backdrop"
    public let name: String
    public let poseCount: Int        // 0 for backdrops
    public let poseNames: [String]
    public let imageCRCs: [UInt32]   // crc32 of RGBA bytes per pose (or the single backdrop image)
}

/// Enumerates every `*.avb`/`*.bgb` file directly inside `artDir` (sorted by
/// filename for determinism), opens each with `AvatarFile`/`BackdropFile`,
/// and builds one `CatalogEntry` per file.
public func buildCatalog(artDir: String) throws -> [CatalogEntry] {
    let dirURL = URL(fileURLWithPath: artDir, isDirectory: true)
    let fm = FileManager.default
    let contents = try fm.contentsOfDirectory(at: dirURL, includingPropertiesForKeys: nil)
    let artFiles = contents
        .filter { ["avb", "bgb"].contains($0.pathExtension.lowercased()) }
        .sorted { $0.lastPathComponent < $1.lastPathComponent }

    var entries: [CatalogEntry] = []
    entries.reserveCapacity(artFiles.count)

    for url in artFiles {
        let ext = url.pathExtension.lowercased()
        let fileName = url.lastPathComponent
        if ext == "avb" {
            let avatar = try AvatarFile(path: url.path)
            var poseNames: [String] = []
            var crcs: [UInt32] = []
            poseNames.reserveCapacity(avatar.poseCount)
            crcs.reserveCapacity(avatar.poseCount)
            for i in 0..<avatar.poseCount {
                poseNames.append(avatar.poseName(i))
                let image = try avatar.poseImage(i)
                crcs.append(crc32(of: image.rgba))
            }
            entries.append(CatalogEntry(
                file: fileName,
                kind: "avatar",
                name: avatar.name,
                poseCount: avatar.poseCount,
                poseNames: poseNames,
                imageCRCs: crcs
            ))
        } else {
            let backdrop = try BackdropFile(path: url.path)
            let image = try backdrop.image()
            entries.append(CatalogEntry(
                file: fileName,
                kind: "backdrop",
                name: backdrop.name,
                poseCount: 0,
                poseNames: [],
                imageCRCs: [crc32(of: image.rgba)]
            ))
        }
    }

    return entries
}

private func crc32(of data: Data) -> UInt32 {
    data.withUnsafeBytes { raw -> UInt32 in
        let base = raw.bindMemory(to: UInt8.self).baseAddress
        return cc_crc32(base, raw.count)
    }
}
