# TGSTALL: Lumi's poll task went silent (4 to 7 Nov 2026)

Lane TGSTALL, program 2026-10-18, v5.0.0 launch blocker. Owner device Lumi (freenove_s3,
v4.9.0). Raw evidence lives outside the repo because it holds the owner's messages:

- `program-2026-10-18/evidence/lumi-outage-20261106T005835Z/` (the orchestrator's capture
  during the first stall: `state.json`, `orch.json`, `errlog.txt`, `nimbus.log.1`)
- `program-2026-10-18/evidence/tgstall-lumi-20261108T141614Z/` (this lane's GET-only read of
  Lumi on boot 14 through `bench/lumiget.sh`: `state.json`, `errlog-list.json`, `nimbus.log`,
  `nimbus.log.1` to `.3`, `api-log-ramring.txt`)
- `program-2026-10-18/evidence/lumi-v490-watch/state-4h-*.json` (the 4-hourly `/api/state`
  polls that ran through the first stall)
- `program-2026-10-18/evidence/tgstall-tests/` (this lane's test runs, failing first and after)

## Status in one paragraph

TASK items 1 and 2 ask to prove which stall class fits and to fix the cause. The class is
proven (a non-yielding loop on tg_poll, "Proof" below); the loop itself is NOT. Nothing the
device logged names it, so it is not fixed either. What this lane ships is containment (the
liveness guard: a stall of any class ends in about 5 minutes with one restart, one plain notice
and the stuck task's PC on record) plus two adjacent fixes found on the way (the DNS late-answer
state and the stream reader's timeout). Neither adjacent fix is shown to be the cause, and the
DNS one cannot be: Lumi ran v4.9.0, which predates `resolveHostLocked`. If the unknown loop
recurs at Lumi's observed rate (three stalls, 38.6 h, 11.5 h and 16.6 h of uptime after the
boot before each, per the table below), each one still costs the owner at least 5 minutes of
silence plus a restart,
and the request in flight runs again (a Telegram one) or is lost (a web, voice or console one).
Naming and then removing the loop needs the guard's first real catch on Lumi (OPEN HARDWARE 6).

## What happened

Three stalls of the same shape on v4.9.0. Times are device uptime (ms) in `nimbus.log` of the
boot 14 read; wall clock from the boot record (`t=` epoch plus uptime).

| Boot | Turn start | Last line of the turn | Reaper | Ended by |
|---|---|---|---|---|
| 11 | 139123777 (4 Nov 20:26:56Z): `orchestrator: turn host=mistral (conv=0 heap=29004)` | the same line | 139844893 (+721 s) | restart 6 Nov 01:00Z (boot 12 `prev=n11/241867s/turn-message`), 28.5 h later |
| 12 | 41441685: turn on mistral, `files: appended 1 row(s)` at +7 s | `sfxsync` line at +8 s | 42159941 (+718 s) | restart at 63488 s (boot 13 `prev=n12/63488s/turn-message/r1`), 6 h later |
| 13 | 59624885: photo turn on mistral | the turn line | 60339678 (+715 s) | restart at 77256 s (boot 14 `prev=n13/77256s/turn-message/r1`), 4.9 h later |

In every case: the turn never logged `claims:`, `fabric-loop:` or `turn end:`, no exchange
failure line (`transport: heaptrace ... fail=`) ever came, the stuck-turn reaper freed the blue
ring at the loop deadline plus 2 minutes (it frees the arc and nothing else), and from then on
nothing the poll task does appeared again: no `tg archive`, no `telegram: msg`, no `telegram:
send ok`, no reply.

## Proof: the poll task was spinning on its core, not waiting

1. **The poll task never returned to its loop top.** In boot 11 the update check was handed to
   it at 172921619 (`ota: otacheck handed to the Telegram poll task`) and never started: no
   `otacheck runs on the Telegram poll task` line followed, and `/api/state` read `"ota":
   "checking"` from 5 Nov 08:47Z to the end (`lumi-v490-watch/state-4h-20261105T084726Z.json`
   onward). The staged work runs at the loop top (`telegram.cpp:2420` at v4.9.0), so the task
   was still inside the turn that started at 20:26:56Z. This also rules out the update check as
   the cause: it never ran.
2. **The tasks below it on the same core went silent; the other core did not.** tg_poll runs at
   priority 3 pinned to core 0 (`telegram.cpp:2657` at v4.9.0). The sound task (priority 1,
   core 0) logs `sfxsync: gate closed` every 300 s like a clock: 462 such lines in the 38.6 h
   before the first stall, then none in the 28.5 h of it, and the same cut at the start of the
   stalls in boots 12 and 13. During a normal 307 s turn on 4 Nov (124195166 to 124502084 in
   `lumi-v490-watch/stuck-errlog-20261104T162414Z.txt`) the line kept coming (124291356), so a
   long turn alone does not silence it. The cloud relay (priority 2, core 0) logged a session
   end 11 times in the 38.6 h before the stall and 0 times in the 28.5 h of it (at its own
   rate, the chance of no event in 28.5 h is about 3 in 10,000). Meanwhile the loop task
   (core 1) ran the reaper and the 6-hourly retention, and the web server answered every poll.
   A task that waits (a lock, a socket, a queue, a semaphore) gives its core away; only a task
   that stays runnable starves the lower priorities of its own core. The task watchdog did not
   fire because it does not watch core 0's idle task (`idle_core_mask=0`, `main.cpp:3796` at
   v4.9.0) and tg_poll is deliberately not subscribed (it long-polls for 25 s).
3. **Nothing moved for 28 hours.** Internal heap free read exactly 27292 B and DMA free exactly
   19628 B in all six 4-hourly polls from 5 Nov 00:47Z to the 6 Nov 00:58Z capture; PSRAM free
   dropped by about 458 KB when the stall began (5852500 at 16:47Z, 5394344 at 20:47Z) and then
   stayed within 544 B. The stuck turn held its working set and neither allocated nor freed:
   a pure CPU loop.
4. **The card lock was free.** Retention takes the card lock with a 50 ms try on the loop task
   (`memory_subsystem.cpp`, `tryLock(50)`) and ran at every 6-hour mark through the stall; the
   orchestrator's capture of `errlog.txt` and `nimbus.log.1` through the web server needed the
   same lock. So the spin was not inside a file-store or memory loop that holds the card.

## Every way the poll task can stop, and which one fits

| Class | Where (v4.9.0 / lane head) | Bounded? | Fits Lumi? |
|---|---|---|---|
| Staged update check or install on the poll task | `telegram.cpp:2420` / `:2456` (`g_stagedRun()`), `ota_update.cpp` `httpsGetOnce` | Each read loop has a deadline (`before(deadline)`, 15 s socket); a stall inside it would never yield either | No: the check never started (proof 1) |
| File store refusal | `telegram.cpp:1443` / `:1453` `storeInbound` (`attach store failed: too many files`) | Returns at once with a once-per-episode notice; no retry loop | No: refusal returns, and the card lock was free (proof 4) |
| A lock never given back | `memory_subsystem.cpp:733` (card lock, `portMAX_DELAY`), `orchestrator.cpp:198` (TenantLock), `:287` (session mutex), `loops_subsystem.cpp:56`, `connectors.cpp:121`, the lwIP core lock (`net_util.h`, non-recursive, `portMAX_DELAY`) | No | No: a waiting task yields its core (proof 2) and the card lock was free (proof 4) |
| A name lookup with no deadline | v4.9.0: every `client.connect(host, ...)` ran `NetworkManager::hostByName` -> `lwip_getaddrinfo`, which waits on its semaphore with no timeout for lwIP's callback. Lane head: `net_util.h` `resolveHostLocked` (bounded 10 s since lane CRASH2) | v4.9.0: no | No: it waits, it does not spin (proof 2). Found here: the bounded version on main freed its state while a late answer could still arrive (see below) |
| A TLS read with no deadline | Device transport `transport_tls.cpp:70,203,219,241` (v4.9.0) / `:72,211,227,249`, Telegram `readLine` `:537`/`:540`, `readBodyN` `:810`/`:814`, `awaitPollResponse` `:938`/`:942`: every one checks a deadline and yields (`delay(1)`, `vTaskDelay(1)`) | Yes | No: all bounded, and all yield |
| A queue that never drains | `telegram.cpp:2942`/`:3070` `send()` waits 100 ms at most; the reply drain and the inbound drain never block; `sound_fx.cpp:299` `fire()` never blocks | Yes | No |
| A CPU loop that never yields | Below the device code: ESP-IDF's AES engine waits for its DMA in `while (1)` with no timeout (`components/mbedtls/port/aes/dma/esp_aes_dma_core.c:163-170`, `esp_aes_wait_dma_done`, and `aes_hal_wait_done`; ESP-IDF 5.5 source, framework-espidf 3.50504), on the poll task in every TLS record, holding the crypto lock. Arduino's `Stream::timedRead` spins without yielding for the stream timeout (1 s) whenever ArduinoJson asks `BlockingClientReader` for a byte past the end of the input, which only a truncated, early-closed or empty response makes it do (a complete document is never read past its closing brace: `evidence/tgstall-tests/arduinojson-eof-probe.txt`) | The AES wait: no. timedRead: 1 s per cut-off or empty response | **Yes**: the only class that matches proofs 1 to 4. Which loop exactly is not readable from the logs: the device kept no record of where the task was |

What is not proven: which loop. The logs prove a non-yielding loop on tg_poll inside the
mistral turn, with the card free and no allocation, but the device recorded nothing that names
the loop, so this lane does not fix the cause. The AES DMA wait is the leading candidate only
because it is a real unbounded busy-wait on the poll task's TLS path; no PC, instruction or core
dump from a stall supports it over any other loop. The fix below makes the next occurrence name it (the stuck task's PC and
backtrace ride the restart's panic reason) and makes every class above, spinning or waiting,
recover in minutes instead of days.

## The fix

1. **Liveness guard** (`lib/core/include/nimbus/sys/poll_guard.h`, `src/sys/poll_watch.*`). The
   poll task beats at every progress point: its loop top and every wait in it (idle, long-poll,
   Wi-Fi wait, staged work, replies, archive, media), every Telegram socket wait, every TLS
   connect and close (`net_util.h`, so every adapter's sessions count), every provider read and
   write slice (`transport_tls.cpp`), the update download, and every turn, round and tool (the
   engine hooks in `orchestrator.cpp`). Only the poll task's own beats count. The loop task checks
   once a second. Silent for 5 minutes (no legitimate step goes that long: one provider request
   is capped at 120 s, a long-poll at 35 s) means stuck.
2. **Recovery: a device restart, never a task restart.** A stuck task keeps what it holds: the
   TLS work slot, a mutex, the AES engine's lock, sockets and PSRAM. A deleted FreeRTOS task gives
   none of it back, so a restarted poll task would inherit a dead TLS slot or a held card lock.
   The guard reads the stuck task's PC and up to six return addresses from the frame it saved
   when last switched out (at a moment it is off its CPU; nothing in the firmware suspends a
   task, `test_ota_poll_guard` pins that), writes one durable log line, and panics with `esp_system_abort` and a reason
   such as `poll watchdog: tg_poll silent 301 s after provider read, on the CPU 29/29 (a spin), pc
   0x4200a1b2 bt ...`. The panic, not `esp_restart()`, so that:
   - the poison guard counts a try against the Telegram work in flight (`tg_dup_guard.h`): a
     message that stalls the device twice is skipped and the owner told, never a loop;
   - the core dump keeps every task's stack, and the reason (with the PC) comes back durably in
     the next boot's `crash-why:` line even when the stuck task held the card and the log line
     before the panic could not land.
3. **The owner is told once, in plain words, and only what is true.** `orch::stallNotice` /
   `planStallNotice`. The only thing that runs a cut request again is Telegram serving its page
   again: the offset is stored, and the message recorded as answered, only after its turn
   returned (`tg_dup_guard.h`, at-least-once). So the notice promises a retry only when the
   journal's chat is a Telegram chat id, the Telegram poll runs this boot (a token, the task up)
   and the poison guard has not skipped the work (`lostTurnRunsAgain`):
   "Lumi stopped answering while working on your message from 20:26, so it restarted itself. It
   tries that message again on its own; if it was logging something, check it was not saved
   twice." A web chat turn (the cloud app uses the same page), an on-device voice turn and a
   console turn wait in the RAM inbound queue, which the restart empties, and a journal rebuilt
   from the breadcrumb has no chat: for those the notice says the reply was lost and to send it
   again ("That reply was lost and the message is not tried again; if it was logging something,
   check whether it was saved before you send it again."). With no turn in flight it says
   Telegram messages sent meanwhile are being answered and anything typed in the web page or said
   to it was not kept. The boot reads `telegram::enabled()` and `guardStatus().tries` for this
   (`crash_report.cpp queueCrashNotice`). Once per streak: a second guard restart without a
   healthy half hour between them is not told again.
4. **No restart loop.** A streak record in RTC no-init memory counts guard restarts in a row.
   After 3, the guard waits 60 minutes of silence instead of 5 before the next one; a healthy
   half hour ends the streak. A deterministic stall therefore costs one restart an hour at worst,
   never a tight loop, and the device is never left stuck for good.
5. **`/api/state` shows it.** `tgPoll.silentS`, `tgPoll.step`, `tgPoll.stallStreak`
   (`docs/reference/config-and-nvs.md`). An orchestrator poll now sees a stall in seconds.
6. **Two adjacent fixes found on the way. Neither is shown to be the cause of the Lumi stalls.**
   - `net_util.h resolveHostLocked` (main since lane CRASH2, so not on Lumi's v4.9.0): the wait
     was bounded at 10 s but the state lived on the stack and its semaphore was freed on return,
     while lwIP's own retries run up to 7 s per DNS server (three servers allowed): a late answer
     wrote into a dead stack frame and signalled a freed semaphore, on every task's connect path.
     The lookup is now `nimbus::net::DnsLookup` (`nimbus/net/dns_lookup.h`), a template the device
     instantiates with the real lwIP calls (`nimbus_net_detail::LwipDnsOps`, each raw call in its
     own `LOCK_TCPIP_CORE` region) and `test_late_wait` runs against a fake lwIP: the state lives
     on the heap and whichever side lets go last frees it, under the core lock
     (`nimbus/net/late_wait.h`).
   - `transport_tls.cpp` `BlockingClientReader`: `setTimeout(0)`. ArduinoJson reads the stream a
     byte at a time through `readBytes`, whose `timedRead` retries `read()` without yielding for
     1 s once `read()` reports the end. A host probe against ArduinoJson 7.4.3 with the
     transport's filter and nesting limit (`evidence/tgstall-tests/arduinojson-eof-probe.cpp` and
     `.txt`) shows complete documents, with or without trailing whitespace, are never read past
     their closing brace (0 reads past the end); only a truncated, early-closed or empty body is
     (1 read past the end, `IncompleteInput` or `EmptyInput`). So this removed a 1 s spin per
     cut-off or empty response, not per response, and it cannot explain the Lumi stalls (a 1 s
     spin ends; theirs lasted hours) or any routine starvation of lower-priority tasks.
7. **Archive downloads beat on every read.** A file Telegram's file server will not resume is
   fetched whole in one step (up to 20 MB, no time budget, `tg_archive_engine.cpp pumpBytes`
   with `mayStop` false). The first version beat only while waiting for bytes, so a slow link that
   kept bytes coming could have run past 5 minutes without a beat and restarted a healthy device
   mid-download. `TgFileSource::read` now beats on every call (each call is bounded by
   `kReadIdleMs`).

No new task, no second TLS session, no NVS key, no new setting. Static DRAM: recorded in the lane's
PR_BODY from the battery images; the guard's own static RAM from `nm` of its object file
(`evidence/tgstall-tests/fixpass-poll-watch-nm.txt`).

## Tests (host, failing first)

- `test/test_poll_guard`: every stall class in the table above is a row (staged work, store
  refusal retried, a held lock, a name lookup, a TLS read, a queue, a spin, a preempted spin, a
  vanished task): each is noticed once, between the limit and one check past it, with its step
  and its spin or wait judgement; the long healthy work (a day of long-polls, a 12-minute turn,
  120 s silent tool calls, the longest tolerated silence, an update install) never trips it; a
  beat starts a new episode (no thrash); a disarmed guard (factory reset) stays quiet; millis()
  wrap; a beat that lands between the loop task's clock read and its beat read (a millisecond
  newer than the clock) reads as fresh, never as a 49-day silence; the streak record across restarts, its pending flag read once, its garbage and
  other-build rejection, the long limit after 3 and the healthy reset; the panic reason bounded,
  printable, read back as the guard's and as a watchdog by the crash notice; the guard's panic
  restart driven through the real storm-breaker record (`TgGuard`): the first restart leaves the
  Telegram bucket free to run again, the second skips it, and an `esp_restart` would count
  nothing; the notice promises a retry only for a Telegram chat with the poll up and the bucket
  not skipped (14 rows: Telegram message, group photo, file, no poll, skipped, web chat, web
  photo, on-device voice, console, no chat, malformed ids), and with no turn in flight only
  promises Telegram messages; the owner notice for every turn kind, once per streak, no em dash,
  no "I", never "crash".
- `test/test_late_wait`: the cell's rule (both orders, 256 mixed), then the device's own lookup
  (`DnsLookup`) against a fake lwIP: a cached answer, a refused query, an answer or a failure
  arriving in the query, during the wait, between the wait and the waiter leaving, or after the
  waiter gave up, from a task and from the TCPIP thread; the late answer step by step; out of
  memory; 300 lookups whose late answers arrive scrambled. Each state and semaphore freed exactly
  once, nothing touched after it was freed, the wait never holds the core lock.
- `test/test_poll_watch_wiring` (source scan, comments stripped): `loop()` calls
  `pollwatch::tick()` at its top level before any return; `setup()` reads the streak record before
  the crash notice; `telegram::begin()` arms after creating the task; the poll loop top beats; the
  task's exit and `quiesce()` disarm; the restart is `esp_system_abort`, never `esp_restart`; the
  boot passes `telegram::enabled()` and `guardStatus()` to the notice; the long paths beat (update
  download, every provider read, write and connect, archive reads, Telegram socket waits, media
  download, turn start, end, rounds and tools, every `connectHost` overload and `tlsClose`);
  `resolveHostLocked` runs `DnsLookup`; every beat's step is a string literal.

Failing first: both suites ran against stubs that keep the pre-fix behavior (nothing watches the
poll task; the waiter always frees the state): 9 of 14 cases failed
(`evidence/tgstall-tests/failing-first-stub.log`), then all passed with the implementation
(`evidence/tgstall-tests/after-fix-1.log`). The clock race was found in self-review after the
first implementation: its test failed against that implementation (`silentMs` read 4294967293,
`evidence/tgstall-tests/failing-first-race.log`) and passes with the signed, clamped silence
(`evidence/tgstall-tests/after-fix-3.log`).

Fix pass (after an independent review): the new cases ran against stubs that keep the pre-fix
behavior (every request promised a retry; the DNS waiter frees the state whenever it returns;
the boot not wired): 7 failed (`evidence/tgstall-tests/fixpass-failing-first-stub.log`), then
68 of 68 passed across the three suites plus `test_crash_forensics` and `test_ota_poll_guard`
(`evidence/tgstall-tests/fixpass-after-fix-1.log`). The wiring suite has no pre-fix version to
fail against (it pins calls that already existed), so its failing-first evidence is 14 mutation
runs on a scratch copy of the tree, each deleting or bending one pinned call (the loop's check,
the check under a condition, the record read, the arm, the quiesce disarm, the loop-top beat,
the update download beat, the panic swapped for `esp_restart`, the archive beat, a provider read
beat, a tool beat, a `connectHost` beat, the notice's Telegram facts, a non-literal step): 14 of
14 turned it red, the unmutated control green (`evidence/tgstall-tests/fixpass-wiring-mutations.log`).
What the wiring suite cannot do: prove a beat fires at run time or that the frame read works on
the S3. Those stay OPEN HARDWARE.

## OPEN HARDWARE

Nothing here ran on a board. No bench unit is reachable and Lumi is read-only for this lane.

1. The guard end to end on a bench unit, test build: console `TGSTALL spin`, then `TGSTALL
   block`: within about 5 minutes the durable log has `telegram: the poll task has been silent`,
   the unit restarts with reason `crash(panic)`, the next boot's `crash-why:` starts with `poll
   watchdog:` and carries a PC inside `runTestStall` (decode against the image's ELF), and
   `TGSTALL?` reads `afterStall=1 streak=1`. With an owner chat set, exactly one stall notice.
2. Three `TGSTALL spin` in a row: the third restart's next stall waits 60 minutes (`/api/state
   tgPoll.stallStreak` 3); after 30 healthy minutes the streak is 0.
3. A real turn and an update install with the guard armed: `tgPoll.silentS` stays under 120 s
   throughout, no restart.
4. The PC sampling (`readFrames`): that the frame a preempted or blocked task saved gives a PC
   in the spinning (or waiting) function on the S3 (dual core, task on core 0, guard on core 1).
5. DNS late answer: a resolver that answers after 10 s (a firewall delaying UDP 53) with the
   poll task connecting: no crash, the connect fails cleanly, the next one works.
6. On Lumi after the next update: the guard's first real catch names the spin. Until then the
   exact loop behind the 4 to 7 Nov stalls stays unproven, and so does any fix for it.
7. The notice on the device, by chat: a guard restart during a Telegram turn says the message is
   tried again and the reply then arrives; one during a web chat turn says the reply was lost
   and nothing re-runs it; one with no turn in flight answers the Telegram messages that waited.
8. A whole-file archive download (a server that ignores the resume) on a slow link longer than
   5 minutes: `tgPoll.silentS` stays low, no restart.
