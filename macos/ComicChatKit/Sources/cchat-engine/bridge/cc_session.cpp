#include "cc_session.h"

#ifndef CC_PROTO_EVENT_DEFINED
// Temporary forward declaration (Task 1): the real union lands in Task 5a.
// This lets cc_session.cpp compile ahead of that definition; Task 5a removes
// this block.
struct cc_proto_event { int32_t type; };
#endif

#include "comicchat.h"     // cc_proto_event (Task 5 fills it; Task 1 forward-uses type=0)
#include <cassert>
#include <cstring>

static CCSession* g_session = nullptr;   // single-threaded: one active session
CCSession* ccSession() { assert(g_session && "no active cc_session"); return g_session; }

void ccEmitProtoEvent(const cc_proto_event* ev) {
    CCSession* s = ccSession();
    if (s->cfg.on_event) s->cfg.on_event(s->cfg.user_data, ev);
}

cc_session* cc_session_create(const cc_session_config* cfg) {
    if (!cfg || !cfg->send) return nullptr;
    CCSession* s = new CCSession();
    s->cfg = *cfg;
    return reinterpret_cast<cc_session*>(s);
}
void cc_session_destroy(cc_session* h) {
    CCSession* s = reinterpret_cast<CCSession*>(h);
    if (g_session == s) g_session = nullptr;
    delete s;
}
void cc_session_feed_bytes(cc_session* h, const uint8_t* d, size_t n) {
    CCSession* s = reinterpret_cast<CCSession*>(h);
    g_session = s;                       // activate for lifted code (Task 3+)
    s->inbuf.append(reinterpret_cast<const char*>(d), n);
    // Task 3 replaces this with the lifted line-framer + parse dispatch.
    g_session = nullptr;
}
void cc_session_fire_timer(cc_session* h, int32_t) { (void)h; /* Task 4 */ }

void cc_session_test_echo(cc_session* h) {
    CCSession* s = reinterpret_cast<CCSession*>(h);
    g_session = s;
    if (s->cfg.send) s->cfg.send(s->cfg.user_data,
                                 reinterpret_cast<const uint8_t*>("ECHO\r\n"), 6);
    cc_proto_event ev; std::memset(&ev, 0, sizeof(ev));  // type 0 placeholder
    ccEmitProtoEvent(&ev);
    g_session = nullptr;
}
