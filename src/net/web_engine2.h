#pragma once
// web_engine2 - the web pages that read an engine-2 chat (lane E2-P-SURF): the turn inspector
// (/api/lastturn), the turn log (/api/trace) and the summary card (/api/fold/status). The routes keep
// their paths; the handler of each in webui.cpp / fold_routes.cpp branches here on an engine-2 image.
// The engine's arena belongs to the turn task, so each page stages one ask (plain/surface.h AskBox) and
// its response waits for the task's answer (pollJobs) up to 8 s; a running turn answers a plain line.
//
//   GET /api/lastturn[?round=N]  the request of the last closed turn, rendered again from the stored
//                                conversation, with the turn's figures
//   GET /api/trace?turn=m<hex>   the turn log of the turn that message started ("" or absent: the last)
//   GET /api/fold/status[?chat=] the conversation's current summary and when it was written
//
// The caller has already passed the token gate (webAuthOk).

#include <ESPAsyncWebServer.h>

namespace nimbus::net::e2web {

void lastTurn(AsyncWebServerRequest* r);
void trace(AsyncWebServerRequest* r);
void foldStatus(AsyncWebServerRequest* r);

}  // namespace nimbus::net::e2web
