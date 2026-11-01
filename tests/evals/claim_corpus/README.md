# Claim check corpus

Real replies the claim check has read, each with what it should have said. `noted.json` is the "Noted." class: a plain
acknowledgement must not earn "(Correction: that has not been saved.)", and a reply that says it kept something must.

Rows come from the Virtual Nimbus reality runs (1 Nov 2026, the file names the run) and the device's food chat, with
synthetic test words only. `test/test_orch_claims` reads the file, so a new real reply lands here as a row, never as a
new one-off test. Each row: `owner` (the message), `reply` (as the model wrote it, no correction line), `claim` (true
when the reply says it kept something and nothing was stored), `from`.
