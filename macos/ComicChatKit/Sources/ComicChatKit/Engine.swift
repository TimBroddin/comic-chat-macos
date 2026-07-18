import cchat_engine

public enum Engine {
    public static var version: Int32 { cc_engine_version() }
}

// `ProtocolSession` (Plan 3 Task 7 — Sources/ComicChatKit/ProtocolSession.swift)
// is the Swift-side driver for the `cc_session` C boundary (bytes in via
// NWConnection, typed `ProtocolEvent`s out via an AsyncStream). It's `public`
// in its own file already; re-exported here in spirit only (no facade type
// needed — `Engine` stays the thin version-probe it always was, and callers
// construct `ProtocolSession` directly, matching how `Strip`/`CGCanvas` are
// also used directly rather than vended through `Engine`).
