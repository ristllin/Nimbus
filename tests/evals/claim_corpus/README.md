# Claim check corpus

Real replies the claim check has read, each with what it should have said. `noted.json` is the "Noted." class: a plain
acknowledgement must not earn "(Correction: that has not been saved.)", and a reply that says it kept something must.

Rows come from the Virtual Nimbus reality runs (1 Nov 2026, the file names the run) and the device's food chat, with
synthetic test words only. The device's claim check is gone (lane RB-Q1b, v5.2.0) and its suite went with it; the
corpus stays as an offline eval. The truth probe's qualification reads it (`tests/evals/probe/run_probe.py` and
`build_sets.py`: precision and recall of a head's probe model, PRD section 8). (The reply guard's echo drop read it too
until the guard was deleted, lane RB-Q1c.) A new real reply lands here as a row, never as a new one-off test. Each row: `owner` (the message), `reply` (as the model wrote it, no correction line), `claim` (true
when the reply says it kept something and nothing was stored), `from`.
