import Testing
import Foundation
import cchat_engine

@Test func engineSelfTestsPass() {
    #expect(cc_run_selftests() == 0)
}
