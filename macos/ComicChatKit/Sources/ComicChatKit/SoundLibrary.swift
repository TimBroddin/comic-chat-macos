import Foundation

/// Resolves an inbound `.sound` event's filename to a playable `.wav` on disk
/// inside a user-configured sounds folder (Plan 4b Task 9).
///
/// D1 §4.2 finding (binding, spec §5 amended): no original WAV assets exist
/// anywhere in this repo's trees — the 1998 client played from the Windows
/// media directory by filename, and sounds were never bundled/downloaded
/// (plan3 command-surface.md:288). So this folder ships EMPTY (DECIDED, Tim,
/// plan review) — `resolve` simply misses for every name until the user drops
/// files in themselves. MIDI is dropped per spec §5 (`.wav` only).
///
/// Pure `Foundation` path resolution — no `AVFoundation` here (that lives in
/// the APP target's `AppState`, which owns actual playback); keeps the Kit
/// free of the AVFoundation dependency per the brief.
///
/// `Sendable`: holds only an immutable `URL`, so a single instance may be
/// shared/reused freely (mirrors `AvatarDownloader`'s posture).
public struct SoundLibrary: Sendable {
    private let folder: URL

    public init(folder: URL) {
        self.folder = folder
    }

    /// Resolves `name` (a remote peer's claimed filename, off the wire — see
    /// `ProtocolEvent.sound`'s `file` field) to a `.wav` inside `folder`, or
    /// `nil` if no match exists.
    ///
    /// - Case-insensitive basename match: tries `<name>` then `<name>.wav`
    ///   against every entry actually present in `folder`, comparing
    ///   case-insensitively (`Boing.wav` matches `resolve("boing")` and
    ///   `resolve("BOING.WAV")` alike) — a real directory listing, not a
    ///   guessed-case file-exists probe, so it works regardless of the
    ///   filesystem's own case-sensitivity setting.
    /// - `.wav`-only: a same-named `.mid`/anything-else present in `folder` is
    ///   never returned (spec §5: MIDI dropped).
    /// - Path-traversal-safe: `name` is reduced to `(name as NSString)
    ///   .lastPathComponent` FIRST, so a hostile/odd remote-supplied name
    ///   (`"../../etc/passwd"`, `"/etc/passwd"`, `"sub/dir/file"`) can never
    ///   name anything outside `folder` — the lookup only ever considers a
    ///   bare basename against `folder`'s own listing.
    public func resolve(_ name: String) -> URL? {
        let bare = (name as NSString).lastPathComponent
        guard !bare.isEmpty else { return nil }

        let candidates: [String]
        if bare.lowercased().hasSuffix(".wav") {
            candidates = [bare]
        } else {
            candidates = [bare, bare + ".wav"]
        }

        guard let entries = try? FileManager.default.contentsOfDirectory(
            at: folder, includingPropertiesForKeys: nil
        ) else {
            return nil
        }

        for candidate in candidates {
            if let match = entries.first(where: {
                $0.lastPathComponent.caseInsensitiveCompare(candidate) == .orderedSame
                    && $0.pathExtension.caseInsensitiveCompare("wav") == .orderedSame
            }) {
                return match
            }
        }
        return nil
    }
}
