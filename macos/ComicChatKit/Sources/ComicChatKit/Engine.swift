import cchat_engine

public enum Engine {
    public static var version: Int32 { cc_engine_version() }
}
