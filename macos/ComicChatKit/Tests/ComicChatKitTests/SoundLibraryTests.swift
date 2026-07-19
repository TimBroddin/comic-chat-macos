import Testing
import Foundation
@testable import ComicChatKit

// Plan 4b Task 9 — sounds: `SoundLibrary`'s pure path-resolution surface.
// NOT nested inside `EngineGlobalStateSelfTests` (same posture as
// `AvatarDownloaderTests`'s own doc comment): `SoundLibrary` never touches
// `cc_strip_*`/`cc_session_*` or any other process-global engine state — it
// only lists a directory and compares filenames. Runs fully parallel.
struct SoundLibraryTests {
    private func tempDir() -> URL {
        let dir = FileManager.default.temporaryDirectory
            .appendingPathComponent("sound-library-tests-\(UUID().uuidString)")
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir
    }

    // MARK: - Case-insensitive .wav resolution

    @Test func resolvesLowercaseNameAgainstMixedCaseFile() throws {
        let dir = tempDir()
        defer { try? FileManager.default.removeItem(at: dir) }
        let wav = dir.appendingPathComponent("Boing.wav")
        try Data("RIFF".utf8).write(to: wav)

        let library = SoundLibrary(folder: dir)
        let resolved = library.resolve("boing")

        #expect(resolved?.standardizedFileURL == wav.standardizedFileURL)
    }

    @Test func resolvesUppercaseNameWithExtensionAgainstMixedCaseFile() throws {
        let dir = tempDir()
        defer { try? FileManager.default.removeItem(at: dir) }
        let wav = dir.appendingPathComponent("Boing.wav")
        try Data("RIFF".utf8).write(to: wav)

        let library = SoundLibrary(folder: dir)
        let resolved = library.resolve("BOING.WAV")

        #expect(resolved?.standardizedFileURL == wav.standardizedFileURL)
    }

    // MARK: - Path traversal safety

    @Test func pathTraversalNameNeverEscapesFolder() throws {
        let dir = tempDir()
        defer { try? FileManager.default.removeItem(at: dir) }

        let library = SoundLibrary(folder: dir)
        // No "boing.wav" or "passwd.wav" anywhere inside `dir` -- a hostile
        // remote-supplied name that tries to climb out via "../" components
        // (or an absolute path) must resolve to nil, never to a real file
        // outside `dir` (it must not even be able to prove /etc/passwd
        // exists). Whatever the result, it must be inside `dir` if non-nil.
        let resolved = library.resolve("../etc/passwd")

        if let resolved {
            #expect(resolved.standardizedFileURL.path.hasPrefix(dir.standardizedFileURL.path))
        }
        #expect(resolved == nil)
    }

    @Test func pathTraversalNameResolvesToBasenameInsideFolderWhenPresent() throws {
        // A stronger version of the traversal check: even when a file with
        // the traversal name's BASENAME legitimately exists inside `dir`,
        // resolution must land ON that in-folder file, never attempt to
        // actually walk the "../" components against the real filesystem.
        let dir = tempDir()
        defer { try? FileManager.default.removeItem(at: dir) }
        let wav = dir.appendingPathComponent("passwd.wav")
        try Data("RIFF".utf8).write(to: wav)

        let library = SoundLibrary(folder: dir)
        let resolved = library.resolve("../../etc/passwd")

        #expect(resolved?.standardizedFileURL == wav.standardizedFileURL)
        #expect(resolved!.standardizedFileURL.path.hasPrefix(dir.standardizedFileURL.path))
    }

    // MARK: - Miss

    @Test func missingNameResolvesToNil() throws {
        let dir = tempDir()
        defer { try? FileManager.default.removeItem(at: dir) }

        let library = SoundLibrary(folder: dir)
        let resolved = library.resolve("missing")

        #expect(resolved == nil)
    }

    // MARK: - MIDI dropped (spec §5)

    @Test func midiFileIsNeverResolved() throws {
        let dir = tempDir()
        defer { try? FileManager.default.removeItem(at: dir) }
        let mid = dir.appendingPathComponent("Boing.mid")
        try Data("MThd".utf8).write(to: mid)

        let library = SoundLibrary(folder: dir)
        let resolved = library.resolve("boing")

        #expect(resolved == nil)
    }

    /// A `.mid` present alongside a real `.wav` of the same basename must
    /// still resolve to the `.wav`, not accidentally match the `.mid` (the
    /// extension check in `resolve` gates on `.wav` explicitly, not merely on
    /// "some file with this basename exists").
    @Test func midiSiblingDoesNotShadowWavMatch() throws {
        let dir = tempDir()
        defer { try? FileManager.default.removeItem(at: dir) }
        let wav = dir.appendingPathComponent("Boing.wav")
        let mid = dir.appendingPathComponent("Boing.mid")
        try Data("RIFF".utf8).write(to: wav)
        try Data("MThd".utf8).write(to: mid)

        let library = SoundLibrary(folder: dir)
        let resolved = library.resolve("boing")

        #expect(resolved?.standardizedFileURL == wav.standardizedFileURL)
    }

    // MARK: - Empty/nonexistent folder (D1 §4.2: ships empty)

    @Test func nonexistentFolderResolvesToNilRatherThanCrashing() throws {
        let dir = FileManager.default.temporaryDirectory
            .appendingPathComponent("sound-library-tests-never-created-\(UUID().uuidString)")

        let library = SoundLibrary(folder: dir)
        let resolved = library.resolve("boing")

        #expect(resolved == nil)
    }
}
