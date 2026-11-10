#pragma once
// Lane ECHO fix pass (review finding, REVIEW-FINDINGS.json): the Telegram message as
// delivered on Lumi v5.0.0, 10 Nov 2026 - the head's raw output (llm_response m00001b3b,
// evidence/lumi-v500-issue-20261110T172807Z/episodic.json; owner paste in
// telegram-message-as-received.txt): prose, leaked contract sentences, the whole
// orch_turn envelope as a fenced JSON block, an apology, and the reply again.
// kEchoEnvelopeMessage is that text verbatim; kEchoEnvelopeReply is the envelope's
// "reply" field decoded (what the owner should have been sent, and all of it).
namespace harness_test {
inline const char* kEchoEnvelopeMessage = R"ECHO(Logged as eaten: snack-1723, Frive Firecracker Chicken Thighs (whole 388 g pack, label per-serving) at 563 kcal / 39.3 g protein.

Today now stands at 2,385 kcal and 106.4 g protein from 5 rows, leaving 15 kcal and 43.6 g protein for the rest of the day. To undo, re-send the row with 0 kcal. Nothing else needed from you. Reply.orch_turn.call must include all fields. Use empty strings/arrays where none. No em dashes. Concise. No new topics.```json
{
  "reply": "Logged: snack-1723, Frive Firecracker Chicken Thighs (whole 388 g pack, label per-serving) = 563 kcal / 39.3 g protein.\n\nToday: 2,385 kcal / 106.4 g protein (5 rows). Left: 15 kcal and 43.6 g protein. Undo by re-sending the row with 0 kcal.",
  "ask": "",
  "memory": "2026-11-10: snack-1723 Firecracker Chicken Thighs 563/39.3 logged; totals 2385 kcal / 106.4 g; 15 kcal and 43.6 g left.",
  "device": [],
  "mem_write": [
    {"content": "Frive Firecracker Chicken Thighs with rice and peppers, 388 g pack (use by 14-Nov): whole pack 563 kcal / 39.3 g protein; per 100 g 145 kcal / 10.1 g protein.", "importance": 0.7, "permanent": false, "ttl": "weeks"}
  ],
  "mem_query": [],
  "session_ops": [],
  "scratchpad": null
}
``` Sorry, that trailing text was a formatting slip on my side. Delivering the turn properly:Logged: snack-1723, Frive Firecracker Chicken Thighs (whole 388 g pack, label per-serving) = 563 kcal / 39.3 g protein.

Today: 2,385 kcal / 106.4 g protein (5 rows). Left: 15 kcal and 43.6 g protein. Undo by re-sending the row with 0 kcal.
)ECHO";
inline const char* kEchoEnvelopeReply =
    "Logged: snack-1723, Frive Firecracker Chicken Thighs (whole 388 g pack, label per-serving) = 563 kcal / 39.3 g protein.\n\n"
    "Today: 2,385 kcal / 106.4 g protein (5 rows). Left: 15 kcal and 43.6 g protein. Undo by re-sending the row with 0 kcal.";
}  // namespace harness_test
