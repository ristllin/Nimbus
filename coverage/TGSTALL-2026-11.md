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
| A CPU loop that never yields | Below the device code: ESP-IDF's AES engine waits for its DMA in `while (1)` with no timeout (`components/mbedtls/port/aes/dma/esp_aes_dma_core.c:163-170`, `esp_aes_wait_dma_done`, and `aes_hal_wait_done`; ESP-IDF 5.5 source, framework-espidf 3.50504), on the poll task in every TLS record, holding the crypto lock. Arduino's `Stream::timedRead` spins without yielding for the stream timeout (1 s) on every read after the end of a response (`BlockingClientReader`, ArduinoJson reads through `readBytes`) | The AES wait: no. timedRead: 1 s | **Yes**: the only class that matches proofs 1 to 4. Which loop exactly is not readable from the logs: the device kept no record of where the task was |

What is not proven: the exact instruction. The logs prove a non-yielding loop on tg_poll inside
the mistral turn, with the card free and no allocation, but the device recorded nothing that
names the loop. The fix below makes the next occurrence name it (the stuck task's PC and
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
3. **The owner is told once, in plain words.** `orch::stallNotice` / `planStallNotice`: "Lumi
   stopped answering while working on your message from 20:26, so it restarted itself. It tries
   that message again on its own; if it was logging something, check it was not saved twice."
   Telegram re-serves the message after the restart (the offset was not stored), so that line is
   true. Once per streak: a second guard restart without a healthy half hour between them is not
   told again.
4. **No restart loop.** A streak record in RTC no-init memory counts guard restarts in a row.
   After 3, the guard waits 60 minutes of silence instead of 5 before the next one; a healthy
   half hour ends the streak. A deterministic stall therefore costs one restart an hour at worst,
   never a tight loop, and the device is never left stuck for good.
5. **`/api/state` shows it.** `tgPoll.silentS`, `tgPoll.step`, `tgPoll.stallStreak`
   (`docs/reference/config-and-nvs.md`). An orchestrator poll now sees a stall in seconds.
6. **Two fixes at the source found on the way.**
   - `net_util.h resolveHostLocked` (main since lane CRASH2): the wait was bounded at 10 s but the
     state lived on the stack and its semaphore was freed on return, while lwIP's own retries run
     up to 7 s per DNS server (three servers allowed): a late answer wrote into a dead stack frame
     and signalled a freed semaphore, on every task's connect path. The state now lives on the
     heap and whichever side lets go last frees it, under the lwIP core lock
     (`nimbus/net/late_wait.h`).
   - `transport_tls.cpp` `BlockingClientReader`: `setTimeout(0)`, so ArduinoJson's `readBytes`
     no longer spins a second without yielding at every end of a response.

No new task, no second TLS session, no NVS key, no new setting. Static DRAM: recorded in the lane's
PR_BODY from the battery images.

## Tests (host, failing first)

- `test/test_poll_guard`: every stall class in the table above is a row (staged work, store
  refusal retried, a held lock, a name lookup, a TLS read, a queue, a spin, a preempted spin, a
  vanished task): each is noticed once, between the limit and one check past it, with its step
  and its spin or wait judgement; the long healthy work (a day of long-polls, a 12-minute turn,
  120 s silent tool calls, the longest tolerated silence, an update install) never trips it; a
  beat starts a new episode (no thrash); a disarmed guard (factory reset) stays quiet; millis()
  wrap; the streak record across restarts, its pending flag read once, its garbage and
  other-build rejection, the long limit after 3 and the healthy reset; the panic reason bounded,
  printable, read back as the guard's and as a watchdog by the crash notice; a panic restart
  counts a poison try; the owner notice for every turn kind, once per streak, no em dash, no "I",
  never "crash".
- `test/test_late_wait`: both orders of a DNS answer and a waiter that gives up, and 256 mixed
  lookups: each state freed once, never used after it was freed.

Failing first: both suites ran against stubs that keep the pre-fix behavior (nothing watches the
poll task; the waiter always frees the state): 9 of 14 cases failed
(`evidence/tgstall-tests/failing-first-stub.log`), then all passed with the implementation
(`evidence/tgstall-tests/after-fix-1.log`).

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
   exact loop behind the 4 to 7 Nov stalls stays unproven.
