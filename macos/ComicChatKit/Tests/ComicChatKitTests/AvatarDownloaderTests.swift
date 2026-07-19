import Testing
import Foundation
@testable import ComicChatKit

// Plan 4b Task 6 — avatar auto-download (`URLSession`). NOT nested inside
// `EngineGlobalStateSelfTests` (unlike almost every other suite in this
// target): `AvatarDownloader` never touches `cc_strip_*`/`cc_session_*` or any
// other process-global engine state — it only downloads bytes to a temp file
// and parses them via `AvatarFile(path:)` (a standalone, non-registry-touching
// wrapper, per that type's own doc comment: "Opens a standalone .avb file",
// same posture `ArtTests`/`PNGExportTests` already rely on to run
// unserialized). So this suite runs fully parallel, same as `ArtTests`.
//
// `URLSession` handles `file://` URLs natively — no HTTP server needed for
// the happy-path/oversize/junk/sanitize cases (brief's Step 1 note).
struct AvatarDownloaderTests {
    private func tempDir() -> URL {
        let dir = FileManager.default.temporaryDirectory
            .appendingPathComponent("avatar-downloader-tests-\(UUID().uuidString)")
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir
    }

    // MARK: - Happy path

    @Test func happyPathDownloadsAndValidates() async throws {
        let downloader = AvatarDownloader()
        let source = URL(fileURLWithPath: fixture("armando.avb"))
        let dir = tempDir()
        defer { try? FileManager.default.removeItem(at: dir) }

        let result = try await downloader.fetch(name: "Armando", url: source, into: dir)

        #expect(result.deletingLastPathComponent().standardizedFileURL == dir.standardizedFileURL)
        // The validator itself — parse success == valid .avb (brief's
        // "Consumes" list): a fresh AvatarFile(path:) over the downloaded
        // file must succeed and report real pose data.
        let parsed = try AvatarFile(path: result.path)
        #expect(parsed.poseCount > 0)
    }

    // MARK: - Oversize

    @Test func oversizeFileThrows() async throws {
        let downloader = AvatarDownloader(maxBytes: 2_097_152)
        let dir = tempDir()
        defer { try? FileManager.default.removeItem(at: dir) }

        let oversizeSource = dir.appendingPathComponent("oversize-source.avb")
        let junk = Data(repeating: 0x41, count: 3 * 1024 * 1024) // 3 MB
        try junk.write(to: oversizeSource)

        await #expect(throws: (any Error).self) {
            _ = try await downloader.fetch(name: "Oversize", url: oversizeSource, into: dir)
        }
    }

    // MARK: - Junk content

    @Test func junkContentFailsValidationAndThrows() async throws {
        let downloader = AvatarDownloader()
        let dir = tempDir()
        defer { try? FileManager.default.removeItem(at: dir) }

        let junkSource = dir.appendingPathComponent("junk-source.avb")
        let junk = Data(repeating: 0x00, count: 1024) // 1 KB of zeros
        try junk.write(to: junkSource)

        await #expect(throws: (any Error).self) {
            _ = try await downloader.fetch(name: "Junk", url: junkSource, into: dir)
        }
    }

    // MARK: - Name sanitization

    @Test func nameSanitizationStaysInsideDir() async throws {
        let downloader = AvatarDownloader()
        let source = URL(fileURLWithPath: fixture("armando.avb"))
        let dir = tempDir()
        defer { try? FileManager.default.removeItem(at: dir) }

        let result = try await downloader.fetch(name: "../evil", url: source, into: dir)

        // Must land INSIDE dir -- no path traversal via the announced name.
        #expect(result.deletingLastPathComponent().standardizedFileURL == dir.standardizedFileURL)
        #expect(result.standardizedFileURL.path.hasPrefix(dir.standardizedFileURL.path))
        #expect(!result.lastPathComponent.contains("/"))
        #expect(!result.lastPathComponent.contains(".."))
    }

    // MARK: - Live-fix 2 (crypthome.com real announce): apostrophe + uppercase extension

    /// Live-reproduced real-world announce: "# Appears as
    /// Belle'sBotTinkerBelle.http://www.mermeliz.com/anneseeker/dl/
    /// Belle'sBotTinkerBelle.AVB" -- the announced NAME carries an apostrophe
    /// (a legal HFS+/APFS filename character, but worth a direct test given
    /// `sanitizedFileName`'s job is exactly to make an announced name
    /// filesystem-safe) and the URL's own path component ends in the
    /// uppercase extension ".AVB" (not ".avb") -- `sanitizedFileName`'s
    /// extension check (`cleaned.lowercased().hasSuffix(".avb")`) must be
    /// extension-insensitive so it doesn't double-append a second ".avb"
    /// suffix onto a name that (if it already carried a bare, uppercase-cased
    /// extension) would already end in one.
    @Test func sanitizedFileNameHandlesApostropheAndUppercaseExtension() {
        // The announced NAME itself never carries an extension in practice
        // (`.appearsAs`'s `avatarName` is the bare display name off the "#
        // Appears as <name>" grammar, not the URL's path) -- this asserts the
        // apostrophe survives untouched (original casing preserved) and the
        // bare ".avb" gets appended.
        #expect(AvatarDownloader.sanitizedFileName(for: "Belle'sBotTinkerBelle") == "Belle'sBotTinkerBelle.avb")

        // Extension-insensitive: a name that ALREADY ends in an
        // uppercase-cased ".AVB" must not gain a second suffix -- the
        // extension check itself is case-insensitive even though the rest of
        // the name keeps its original casing.
        #expect(AvatarDownloader.sanitizedFileName(for: "Belle'sBotTinkerBelle.AVB") == "Belle'sBotTinkerBelle.AVB")
    }

    /// End-to-end: fetch a real `.avb` fixture using this exact live-observed
    /// name (apostrophe included) and confirm the downloaded file lands under
    /// the sanitized name inside `dir` and parses as a valid avatar.
    @Test func fetchHandlesApostropheInAnnouncedName() async throws {
        let downloader = AvatarDownloader()
        let source = URL(fileURLWithPath: fixture("armando.avb"))
        let dir = tempDir()
        defer { try? FileManager.default.removeItem(at: dir) }

        let result = try await downloader.fetch(name: "Belle'sBotTinkerBelle", url: source, into: dir)

        #expect(result.lastPathComponent == "Belle'sBotTinkerBelle.avb")
        #expect(result.deletingLastPathComponent().standardizedFileURL == dir.standardizedFileURL)
        let parsed = try AvatarFile(path: result.path)
        #expect(parsed.poseCount > 0)
    }

    // MARK: - Retry

    // Injects a URLSession pointed at a nonexistent file:// URL -- fetch must
    // retry exactly once (chat.cpp:2361-2363 mirror: `GetRetryCount() < 1`
    // then increment) and still ultimately throw. A simple "throws" assertion
    // is acceptable per the brief -- the retry itself is one line, not worth
    // a counting URLProtocol harness for this task.
    @Test func retriesOnceThenThrows() async throws {
        let downloader = AvatarDownloader()
        let dir = tempDir()
        defer { try? FileManager.default.removeItem(at: dir) }
        let missing = dir.appendingPathComponent("does-not-exist.avb")

        await #expect(throws: (any Error).self) {
            _ = try await downloader.fetch(name: "Missing", url: missing, into: dir)
        }
    }
}
