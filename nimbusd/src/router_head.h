#pragma once
// router_head - the hosted Cumulo head's view of a router refusal (CUM-539). The
// rule itself is shared with the device's cumulo head and lives in
// lib/harness (nimbus/harness/router_refusal.h): why the router refused
// (RouterTap), how each kind is answered (classifyRefusal), the honest copy
// (refusalCause, fallbackNotice) and the ladder that answers a refused pick on a
// model that works (runLadder). This header names them for the daemon.
#include "nimbus/harness/router_refusal.h"

namespace nimbusd {

using agent::router_refusal::kEngineCauseMax;
using agent::router_refusal::LadderResult;
using agent::router_refusal::refusalCause;
using agent::router_refusal::RefusalKind;
using agent::router_refusal::refusalSentence;
using agent::router_refusal::RouterTap;
using agent::router_refusal::runLadder;
using agent::router_refusal::sameRoute;
using agent::router_refusal::settleFallback;

}  // namespace nimbusd
