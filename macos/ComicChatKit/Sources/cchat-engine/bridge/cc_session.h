// cc_session.h — internal to the bridge; not part of the public API.
#ifndef CC_SESSION_H
#define CC_SESSION_H
#include "comicchat.h"
#include <string>

struct CCSession {
    cc_session_config cfg{};
    std::string       inbuf;      // accumulates fed bytes; line-framed in Task 3
};

// The active session for the current single-threaded call (set on entry to any
// cc_session_* that runs lifted code). Lifted code reaches callbacks/resolvers
// through this, never through theApp. ASSERTs if no session is active.
CCSession* ccSession();

// R18 dispatcher: fill a cc_proto_event and invoke cfg.on_event. Defined in
// cc_session.cpp; declared here so lifted files can call it.
void ccEmitProtoEvent(const struct cc_proto_event* ev);

#endif
