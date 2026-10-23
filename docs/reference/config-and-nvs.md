# Config and NVS Reference

Every setting the device persists, where it lives, and what its default is. Use this
page when you need an exact NVS key, want to know which fields are write-only, or are
diagnosing a save that did not land. It covers the `solide` NVS namespace and every key
in it, the credential-gate rails (what the model may and may not change), the
sparse-override `Config` blob on the SD card and its no-SD limitation (F10), and the
set-once embedding config.

Related: [Tools & Commands](../tools-and-commands.md) (provisioning over
serial), [Orchestrator World](../orchestrator-world.md)
(the security rails in context).

## Two backends

Persistence rides `solide::memory` ([solide-drivers](https://github.com/ristllin/solide-drivers), `solide/memory.h`),
which splits by size:

| Backend | Store | Survives | Used for |
|---|---|---|---|
| **NVS** (Arduino Preferences) | flash | reflash; atomic | typed key-value config - keys must be **≤ 15 chars** |
| **SD** under `/memory/` | SD card | only if a card is mounted | larger JSON / blobs; returns `false`/`0` when the card is absent |

The NVS namespace is **`solide`** (opened via `solide::memory::begin("solide")`).
Every typed config value below is an NVS key in that namespace. The one blob
(`nimbus_cfg`, the profile override `Config`) rides the SD half - see
[Sparse-override Config + the SD blob](#sparse-override-config--the-sd-blob).

## NVS budget and the full-storage contract

The NVS partition is small and shared: `0x5000` = 20 KB in `default_16MB.csv`, which
is 5 pages of 126 32-byte entries. NVS holds one page back for garbage collection, so
about **504 entries (~16 KB)** carry every key in every namespace - `solide`, the
orchestrator's `orchmem` memory settings and `agjournal` journal, the RF calibration
(`cal_data`, ~61 entries), Wi-Fi, and Bluetooth bonds. A string of `n` bytes costs one
header entry plus `ceil((n + 1) / 32)` data entries; an int or bool costs one.

**What may live there (the headroom rule).** NVS holds owner config and *bounded*
values only. Nothing rebuildable and unbounded may live in NVS:

| Value | Where | Why |
|---|---|---|
| Provider model list (`GET /api/models`, the short list, the model checks) | LittleFS file `/data/models_<provider>.txt` | The model store: every verify rewrites it. Up to 160 models and 20 KB per provider, about 85 B a model, with the fetch time and the short list in its header. Nothing about the model list is written to NVS (before v4.6 the short list lived in NVS as `mch_<provider>`, about 832 B on a device with five providers) |
| Usage ledger | LittleFS file `/data/usage_ledger.txt` | Owner spend history and budgets per provider, at most 12 providers and 48 attribution rows, and always under 3,800 B (a deleted routine's rows go first, then the smallest; provider counters never). Until v4.5.13 it lived in NVS as `usgLedger`, rewritten after every turn, and on a full partition most of a month's spend was lost. The move follows the rule below. Per-model prices left it unchanged, byte for byte: everything per model lives in the files below. OpenAI's cached and cache-written tokens are split out of its input in memory and in the files below, but the ledger keeps OpenAI's input with them included and its cache fields 0, so it does not grow; the split is restored from the per-model file at boot. While flash is unmounted and the ledger is served from its legacy NVS copy, a turn adds no new (provider, tag) attribution row: spend is still counted in the provider's totals and the files below |
| Scratchpad | LittleFS file `/data/scratchpad.bin` | The assistant's working notes. Until v4.5.13 the `orchmem` key `scratchB` (and the older `scratch`); the same rule below |
| Mistral workspace answer | LittleFS file `/data/mistral_ws.txt` | Which Studio connectors the Mistral key's workspace lists, from the last check that landed, so a restart or a check that cannot finish keeps a confirmed connector usable. Tagged with a short SHA-256 fingerprint of the key it was learned with (never the key); read only under that key, and removed when the Mistral key changes. Written only when a fresh answer differs from the stored one. Never in NVS |
| Per-model spend | LittleFS file `/data/usage_models.txt` | Which models each provider's spend went to this period and all time. At most 32 rows, 8 named models per provider: a new model takes over that provider's oldest model with no spend this period, otherwise it folds into the provider's "other models" row, which is priced at the provider's highest known price; a provider with no row always gets one (another provider's smallest row folds into its own "other models"). Tracking starts at the first boot of a release with per-model prices, and the file is written then, before any turn records. The file marks that, whether the latest ledger write did not land, and keeps, per provider, the spend from before tracking started (earlier usage, priced at your default for that provider, else the highest price you set for one of its models, else the built-in default), spend whose model was lost, and the provider's all-time totals as of the latest ledger write that landed, with the part of them from before that write's billing period. At boot the rows must add up to the provider counters in the ledger file. Counters holding less all-time spend than that write did, or less than its spend from before its period plus their spend this period, were re-created (a settings-only reset, on this release or an earlier one): the file then describes none of them, and all of that provider's spend this period is priced at its highest known price. When the rows do not add up and the history shows an earlier release recorded since the latest turn a row recorded (it rewrites the history without models), that provider's spend this period is priced the same way. Otherwise, when the file is only behind the counters (one of its writes did not land) the rows stay and what they miss is priced at the provider's highest known price; when it may be ahead of them (a ledger write did not land), that provider's spend since tracking began loses its model and is priced at the provider's highest known price, never a cheaper one. If this file and the history are both gone while the ledger still counts spend (Erase Storage on a device with no SD card erases `/data`), the period's spend is priced the same way. What a boot decides about re-created or vanished counters is written to the file at once. While the file on flash lacks what the boot decided (none is there, or that write failed), a turn writes it before its ledger write, and while a stale file is there the turn's history bucket names no model. When that write at boot fails, the file is removed only while the history shows tracking started (the next boot then prices the period's spend at the highest known price); otherwise it stays. Rewritten after each turn once tracking has started |
| Your model prices | LittleFS file `/data/usage_mrates.txt` | The prices you enter per model on the Usage page, at most 32 rows. Written only when you save a price; a write that does not land answers 507 like an NVS save (failure key `lfs_mrates`) and changes nothing |
| Cumulo router prices | LittleFS file `/data/usage_rprices.txt` | What Cumulo bills for each model the device keeps (its choice list plus the head and sub-agent models), at most 24 rows. Written only by the Cumulo verify; when it is missing or empty and the Cumulo key is verified, the device runs one Cumulo verify shortly after boot to fill it |
| Daily usage history | LittleFS file `/data/usage_hist.txt` | One bucket per day, provider and model (plus prompt-cache counts since per-model prices), at most 12 per day and 720 in all (60 days). Written atomically (a copy, then a rename) after each turn |
| Monthly spend ceiling | LittleFS file `/data/spend_ceiling.txt` | One line: the ceiling (default 20 USD, and whether the owner set it), the calendar month it counts (device time zone, UTC when unset), this month's estimate across all providers, whether paid calls are paused, and which channels were already told about this pause and about the ceiling itself after the update. Written after each turn that billed something and on each change; never NVS, so a full settings store neither loses the count nor lifts a pause. A write that does not land keeps the state in memory (the pause holds) and is retried every minute. See [Monthly spend ceiling](../spend-ceiling.md) |

**The moved stores (v4.5.14).** The usage ledger and the scratchpad were the two
values rewritten in NVS on every turn. They now live in flash files, and every boot
follows one rule:

- Load the file; only when there is no file, load the legacy NVS key.
- Save by writing the file atomically (a copy, then a rename) and reading it back; the
  save counts only when the read-back matches. NVS is never written again.
- Erase the legacy NVS copy only on a boot whose image was already valid when it
  started (not the trial boot after an OTA), and only once the file reads back. A
  rollback from a trial boot to v4.5.13 therefore still finds that release's last
  values in NVS.

A ledger save that does not land leaves the ledger marked unsaved: every later turn
retries (each rewrites the whole file), the main loop retries once a minute, and
`/api/state` shows `usageSaved: false` until one lands. A scratchpad save that does not
land keeps the notes in RAM until restart and is reported as `lfs_scratch` (the
change result says "kept until restart only (saving failed)"); a full NVS partition
does not affect it. The file itself is written only when the notes actually change
(the device remembers the bytes that last landed), so a turn with no edit writes no
flash.

**The reserve.** Every key the firmware defines has a class
(`lib/core/src/store_nvs_reserve.cpp`; `test_nvs_full` fails on a defined key without
one):

| Class | Examples | At the reserve |
|---|---|---|
| Critical | `tgOffset`, the OTA guard (`otaPend`, `otaBoots`, `otaPrev`, `otaLast`), `webTok`, `apPass`, the cloud credential (`cloudDevId`, `cloudCred`, `cloudHost`), `otaType`, `embLocked` | Always written |
| Owner config | Provider keys, routing, connectors, Telegram allowlist and owners, Wi-Fi, every setting a person saves | Refused, unless the device is starved (then refused only below the floor of 8) |
| Rebuildable | `vfy_*`, `vrs_*`, `vrm_*`, `mch_*`, `antEnv`, `antAgents`, `dreamScrHash`, `zaiBase`, `tgBotName` | Refused |
| Telemetry | `orchConv`, `otaNotif`, `otaNotes`, `battModel`, `lbPingEp`, the `agjournal` records, the legacy `usgLedger` and `scratchB` copies | Refused |

A write that would leave fewer than **24 free entries** is refused unless its key is
critical, and the refusal is reported like any failed write (a save route answers 507,
`nvs.recentFails` names the key). Erases and rewrites that do not grow a value are
never refused, so removing a key or connector always works. A refusal asks the
**eviction pass**, which runs on the main loop (never inside a web request, at most
every 30 s): it erases rebuildable caches in a fixed order (`vrs_*` and `vrm_*`, then
any legacy `mcat_*`, then `dreamScrHash`, then `fbRules` only while it is a cloud copy,
`fbSyncTs` set) until 24 entries are free again. It never erases `antEnv` or
`antAgents` (that would orphan managed agents that keep costing money) or `mch_*`.
When eviction cannot restore the reserve the device is **starved**: owner-config
writes are accepted again and only rebuildable and telemetry writes stay refused, said
once per boot in the device log, so a full device never answers 507 to every save.
Starved owner saves still stop at a **floor of 8 free entries**: a save that would
leave fewer is refused (507, like any reserve refusal). Those last entries are what
the Telegram position and the update rollback safety net need to keep saving: the
storage writes a new value before it removes the old one, so even a one-entry rewrite
fails on a partition with nothing free, and messages would then be answered again
after every restart. Eight covers arming the rollback safety net (about 6 entries)
and the Telegram position (1), with one to spare.

Before any write that grows a value, the gate reads the partition's free entries
(`nvs_get_stats`), critical keys included: a write whose value cannot fit at all is
refused for every class, reported the same way.

**Blobs must fit their whole span (a driver defect, and the pinned toolchain).** A
blob is the one value type that can span NVS pages, and the NVS driver in the
toolchain this firmware pins has a defect: when a rewrite of a stored multi-page blob
runs out of space partway, the cleanup erases the *old* version's chunk indices while
the new version's chunks were written under shifted ones, so every entry the failed
rewrite took stays stranded, counted as used, until the next boot. One failed rewrite
can take a partition from about a kilobyte free to nothing in a single turn. The
defect was seen on **ESP-IDF 5.5.4**, the version the build pins: `platformio.ini`
locks the pioarduino platform at 55.03.39, whose Arduino core (3.3.9) is built on
ESP-IDF 5.5.4; a toolchain bump must re-check the failure cleanup before it lands.
The device therefore refuses a blob write, new or rewrite and whatever its class,
*before* the driver is called, unless the free entries cover the value's whole span:
one data entry per 32 bytes, one chunk header per 126-entry page, the index entry,
and up to 13 entries of a page tail too small for the value (the driver skips such a
tail but still counts it free). No blob writer ships in the device today; a host test
scan fails the build if one appears outside the gated helpers.

The guide `/api/state` checks: at least **4 KB** stays free, room for the largest
single owner value (the connectors blob, capped at 3500 B = 3552 B of entries). Below
that, `nvs.low` reads `true`. It is a guide, not a guarantee: a string must fit inside
one 126-entry page, so free entries scattered across pages can still refuse a big
value. The 507 contract below is what stays exact.

Earlier builds kept the catalogs in NVS as `mcat_<provider>`. On the first boot of a
build with the move (in either mode), each legacy catalog is copied to its file when
the file is missing (Orchestrator mode, where flash storage is mounted) and **every
legacy `mcat_` key is erased**. An erase only frees entries, so this heals a device
that is completely full today.

Up to v4.5.x the model list was a JSON catalog, `/data/mcat_<provider>.json`, plus
the short list in NVS (`mch_<provider>`). On the first boot of the build with the
model store, both are merged into `/data/models_<provider>.txt` (a model the
catalog's size trim had lost but the short list still named is kept). The NVS short
lists are erased on a **later** boot, once the new image has been marked valid and
the store holds the list, so an update that rolls back still finds them. The JSON
catalog files are never written or removed again, so a rollback to an older build
keeps reading its catalog.

**A save that does not land never answers ok.** Every `agent::store` setter returns
whether its write landed, and every write reports to a per-task persist ledger. The
owner-config save routes (`POST /api/config`, `/api/orch`, `/api/connectors` in blob,
patch, and delete modes, `/api/onboard/complete`, `/api/token/regen`, the Wi-Fi
saves `/savewifi` (the setup wizard) and `POST /api/wifi`, `POST /api/fallbacks`, and
the Telegram routes `add`, `remove`, `rename`, `role`, `approve` and `public`) open a
scope before their first write; `POST /api/mem/embedcfg`, `/api/mem/scratchpad` and
`/api/mem/config` check their writes directly. If any write of
that request did not land, the route answers **HTTP 507**:

```json
{"error":"storage_full","message":"Couldn't save: device storage is full. Remove an unused provider key or connector and try again."}
```

A scratchpad change whose save did not land is still applied until restart, so its 507
also carries the change result as `msg`, as the route's 200 body does
(`"msg":"added to short; kept until restart only (saving failed)"`).

The web page shows that sentence as its failure toast (and in the setup wizard's
status lines), never "Saved". Side effects wait for the write: a provider key that did
not store is not verified (that would check the old key), a mode switch that did not
store does not restart the device, and `/api/mem/embedcfg` stores the new embedding
config **before** it erases any memories, so a reset never erases memories for a config
that was not saved. A Wi-Fi list change or a budget edit that does not store is
undone in memory too, so the device never shows a change it will lose at the next
restart. Values the Save buttons post unchanged (a field still at its default, never
stored) need no write, so they never cause a 507 on their own.

**The remedy works on a full partition.** Clearing a value whose default is empty is
an erase, not a write of `""`: removing a provider key, its verify verdict reset
(`-1, 0` is stored as an absent `vfy_<provider>`), the Telegram offset reset, turning
Telegram open access off, and clearing the directive all free entries instead of
needing one. Removing a connector
rewrites a smaller connectors blob; when that cannot land because NVS writes the new
value before it erases the old one, the device erases first and then writes (only for
a value that shrinks, so a growing save can never lose the set; if that write still
fails, the old value is written back). The same applies to removing a chat from the
Telegram allowlist, whose worst case is an empty allowlist (nobody allowed). This
fallback is not power-atomic: a power cut inside that one write can lose the value.

**Runbook.** Free entries come from removing a value (an unused connector card, a
rebuildable cache the device can fetch again), never from a reboot: a restart removes
nothing, and what a failed write stranded comes back only until the next failed write
strands it again.

**Observability.**

| Surface | Field | Meaning |
|---|---|---|
| `GET /api/state` | `nvs.used`, `nvs.free`, `nvs.total` | Bytes, from `nvs_get_stats` over the whole partition. `free` excludes the garbage-collection page, so it is the room left for new data |
| `GET /api/state` | `nvs.low` | `free` is below the 4 KB headroom guide |
| `GET /api/state` | `nvs.fails`, `nvs.lastFail` | Writes that did not land since boot, and the most recent failing key |
| `GET /api/state` | `nvs.recentFails` | The newest 8 failed writes, newest first: `key`, `n` (the failure's number since boot, so a gap between two reads counts the failures in between) and `ageS` (seconds ago). Every failure is recorded, unlike the log line |
| `GET /api/state` | `tgOffsetSaved`, `tgPoll` | Whether the stored Telegram offset matches the one in use (see `tgOffset` below), plus `tgPoll.offsetWrites`, `offsetWriteFails`, `staleDrops` (updates dropped below the offset, expected 0), `backoffMs` and `fails` (the getUpdates backoff) |
| `GET /api/nvs/stats` (sign-in required) | `entries`, `keys[]` | What fills the partition: totals in 32-byte entries (`used`, `free`, `total`, `namespaces`), and every key in every namespace with `ns`, `key`, `type` (`int`, `str`, `blob`), `bytes` and `entries`. Names and sizes only, never a value. At most 512 keys (`truncated` says when more exist) |
| `GET /api/state` | `nvsReserve.entries`, `.refused`, `.evicted`, `.starved` | The reserve (24 entries), writes it refused and keys the eviction pass erased since boot, and whether the device is starved |
| `GET /api/state` | `usageSaved` | `false` while the usage ledger holds spend its file did not take (being retried) |
| `GET /api/state` | `spend.limitCents`, `.spentCents`, `.frozen`, `.ownerSet`, `.active`, `.month`, `.resetsOn`, `.saved` | The monthly spend ceiling: the ceiling and this month's estimate in cents, whether paid calls are paused, whether the owner set the ceiling, whether it applies (`false` when the owner turned it off, or set provider limits and never chose a ceiling), the month it counts (YYYYMM, device time zone) and the day it starts again, and `false` while its file write is being retried. `setupDefaults` is the setup wizard's line about the default models and the ceiling |
| `GET /api/state` | `spend.dailyWarnCents`, `.todayCents` | The owner's daily spend warning (0 = the built-in per-provider warning) and today's estimate across all providers (device date) |
| `GET /api/state` | `spend.providers[]` | Each counted provider: `p`, its monthly limit `capCents` (the budget's `$ per month`, 0 = none), this period's count `spentCents` (every paid call), `frozen` and `resetsOn` |
| `GET /api/orch` | `usage.ceiling` | The same object, for the Usage page's meter. `POST /api/orch` `spendCeil=<cents>` sets the ceiling (1 to 100,000 USD) or `spendCeil=off` turns it off; `dailyWarn=<cents>` sets the daily warning (1 cent to 100,000 USD; 0 = the built-in one); a write that does not land answers 507. A provider's limit is the `budget=<prov>:<tokens>:<calls>:<resetDay>:<cents>` field, applied at once |
| `POST /api/chat` | `409 {"code":"spend_ceiling"}` | Paid calls are paused: the send is answered at once instead of waiting for a reply that will not come |
| Test console `STATUS` | `nvs=<used>/<total> nvsfree=<free> nvsfail=<n>` | The same numbers, in bytes, at the end of the line |
| Device log (`GET /api/log`, durable `nvs` class) | `nvs: write FAILED [<k>] - storage full? (<free> B free of <total> B)` | One line per failing key per minute. The key sits in brackets so the log's secret masking (which hides any `key=` value) leaves it readable |
| Telegram, to the owner | "settings storage is almost full, so some changes may not be saved..." | Sent once per boot, after the first write that did not land, by the Telegram task on its own send path. Needs a bot token and an owner chat |

## NVS keys

Two families write to the `solide` namespace: the **system** keys
(`src/sys/config_nvs.cpp`) and the **agent/orchestrator** keys
(`src/agent/store.cpp`, names in `src/agent/agent_config.h`). "Read back?" = No
marks **write-only** fields the UI never reads back into itself.

### System keys (`src/sys/config_nvs.cpp`)

| Key | Type | Default | Holds | Read back? |
|---|---|---|---|---|
| `nimbus_mode` | int | `Notifier` (0) | Operating mode: `0=Notifier`, `1=Orchestrator` (`Mode` enum). Any value other than 1 resolves to Notifier. | Yes |
| `nimbus_ble` | int (bool) | `1` (on) | Bluetooth advertising enable (Notifier-mode nsn transport). Applies live, no restart. | Yes |
| `nimbus_name` | string | `""` (auto) | Device name / identity; `""` = auto-derived. Sanitized on write. | Yes |
| `nimbus_cfg` | **SD blob** | absent | Versioned profile-override `Config` (see below). **Not an NVS key** - rides `putBlob`/`getBlob` on SD. | - |

### Provider credentials (`src/agent/store.cpp`)

Every credential getter returns `""` until provisioned - this is the
**credential gate** (see below). `has*()` helpers let the turn loop degrade
gracefully instead of firing a doomed TLS call.

| Key | Type | Default | Holds | Read back? |
|---|---|---|---|---|
| `oaiKey` | string | `""` | OpenAI API key | No - write-only; UI exposes only a "has key" flag |
| `antKey` | string | `""` | Anthropic API key | No - write-only |
| `mistralKey` | string | `""` | Mistral API key (`== Nuage store::hasKey()`) | No - write-only |
| `tavilyKey` | string | `""` | Tavily API key for the orchestrator `web.search` / deep-research tool | No - write-only |
| `custBase` | string | `""` | Custom / proxy endpoint base (registers backend `custom` when non-empty) | Yes |
| `custKey` | string | `""` | Custom endpoint key | No - write-only |
| `custConv` | string | `"openai"` | Custom wire convention: `openai\|mistral\|anthropic` | Yes |
| `custModel` | string | `""` | Custom endpoint model | Yes |
| `zaiKey` | string | `""` | Z.ai (GLM) API key | No - write-only |
| `zaiBase` | string | `""` | Z.ai probed working host (`api.z.ai \| open.bigmodel.cn`) | Yes |
| `cumuloKey` | string | `""` | Cumulo router key (one key, every upstream); see [Use your Cumulo key](../cloud/cumulo-key.md) | No - write-only |
| `cumuloBase` | string | `""` | Cumulo router host or full base URL; `""` uses the built-in default host (`app.cumulo-nimbus.ai`). Minting a key from the Cloud access card also sends the device pairing credential to this host, so an override must point only at a trusted router | Yes |

Writing or clearing a provider key **resets that provider's verify cache** to
`-1, 0` (couldn't-verify / never), so a swapped key can't ride the old verdict. A clear
(and the `-1, 0` reset) is stored as an erase, so it lands even when NVS is full; a
key that did not store resets nothing and starts no verify.

### Orchestrator routing + models (`src/agent/store.cpp`)

| Key | Type | Default | Holds | Read back? |
|---|---|---|---|---|
| `orchHost` | string | `""` | Explicit host provider; `""` = top of priority. **Human-only + model-protected.** | Yes |
| `orchConv` | string | `""` | `"host\|convId"` per-host conversation state | Yes |
| `provPrio` | string | `"openai,anthropic,mistral"` | Orchestrator-**host** priority list (alias `orchPriority`). **Human-only + model-protected.** | Yes |
| `subPrio` | string | `"openai,anthropic,mistral"` | Sub-session provider priority - **the only model-writable routing knob** (`setSubPriority`) | Yes |
| `orchM_<provider>` | string | `""` → provider flagship | Per-provider orchestrator model override (prefix `orchM_` + provider) | Yes |
| `subM_<provider>` | string | `""` → provider flagship | Per-provider sub-session model override (prefix `subM_`) | Yes |
| `agFabric` | string | `"code:openai,research:openai,ops:anthropic"` | Legacy category→provider bindings | Yes |
| `sysPrompt` | string | `""` (empty uses the shipped default directive) | The owner directive - **immutable by the model**, owner-only. Bounded to 1500 bytes (honest error on the web route + store-layer clamp). Empty = the compiled-in default (`kOwnerDirectiveDefault`); set in the setup wizard or the web Directive box, with "Revert to default" | Yes |

All three hosts (OpenAI, Anthropic, Mistral) are in the default `provPrio` /
`subPrio` lists; list order is the failover order, and an explicit `orchHost`
overrides it.

Default per-provider models: `OPENAI_MODEL` = `gpt-5.6`, `ANT_MODEL` =
`claude-sonnet-5`, `MISTRAL_MODEL` = `mistral-large-latest`; custom →
`custModel`.

### Turn behavior + TLS transport (`src/agent/store.cpp`)

| Key | Type | Default | Holds | Read back? |
|---|---|---|---|---|
| `midFail` | bool | `true` (on) | Mid-turn provider failover: on a tool-loop turn, fall back to the next host in priority order when the active provider call fails. | Yes |
| `tlsSlots` | int | `1` | Concurrent outbound work-TLS sessions the arbiter permits, clamped to `1..2`. Latched at boot; `1` keeps a heavy turn's contiguous heap intact, `2` raises throughput on a board with headroom. | Yes |
| `tlsVerify` | bool | `true` (on) | Validate provider TLS certificates against the bundled CA set. | Yes |

### Telegram (`src/agent/store.cpp`)

| Key | Type | Default | Holds | Read back? |
|---|---|---|---|---|
| `tgToken` | string | `""` | Telegram bot token | **No - WRITE-ONLY.** UI exposes only `hasTg` (a boolean); the token is never read back into the UI |
| `tgAllow` | string | `""` | Allowlisted chat ids (comma-separated) | Yes |
| `tgOwners` | string | `""` | Owner chat ids (comma-separated subset of `tgAllow`); empty means the first allow entry is owner. | Yes |
| `tgNames` | string | `""` | Display-name sidecar for known chats (`id:name,...`). Only allow-listed chats keep a name: a rename of any other id is refused, and each rename drops the names of chats no longer on the allowlist. | Yes |
| `tgBotName` | string | `""` | The connected bot's `@username` (from getMe), display only. | Yes |
| `tgPublic` | bool | `false` | Open access: accept anyone who messages the bot. Default off. | Yes |
| `tgOffset` | int | `0` | Telegram long-poll offset: a copy of the one in use, read once when the Telegram task starts. While the device runs, the offset in RAM is the truth and only moves forward, so a write that does not land (a full NVS) never makes Telegram serve a page again; a restart right after a page can repeat that one page. Written once per new page, and after each turn of a page that holds a command, so a restart repeats at most the turn in progress, never a finished command; while a write is owed and failing, a page gets one attempt and the write is retried at most once a minute, so it lands within a minute of room appearing (`tgOffsetSaved` in `/api/state` reads `false` meanwhile). A token change resets it to `0`, stored as an erase, which lands on a full NVS too | Yes (internal) |

### Cloud access / relay (`src/agent/store.cpp`)

Reaching the device from anywhere through CumuloNimbus. Orchestrator-only; ships
dark (off). See [Cloud access](../cloud-relay.md) for the pairing flow and the
security model.

| Key | Type | Default | Holds | Read back? |
|---|---|---|---|---|
| `cloudOptIn` | bool | `false` | Cloud relay enabled. | Yes |
| `cloudDevId` | string | `""` | Cloud device id assigned at pairing. | Yes |
| `cloudCred` | string | `""` | Cloud-minted device credential (bearer); erased on unpair. | No - write-only |
| `cloudHost` | string | `app.cumulo-nimbus.ai` | Relay host. | Yes |
| `cloudName` | string | `""` | Paired device display name for the web status line. | Yes |

### OTA / update engine (`src/agent/store.cpp`)

Persisted across the install restart so the device can validate a fresh image and
roll back if it fails. See [OTA updates](../ota.md).

| Key | Type | Default | Holds | Read back? |
|---|---|---|---|---|
| `otaPend` | bool | `false` | A fresh image is awaiting first-boot validation. | Yes (internal) |
| `otaBoots` | int | `0` | Boot attempts since the slot flip. | Yes (internal) |
| `otaPrev` | string | `""` | Previous app slot label (`app0`/`app1`) for rollback. | Yes (internal) |
| `otaLast` | string | `""` | Last OTA outcome (`ok vX` / `rollback vX` / ...). | Yes |
| `autoUpd` | bool | `false` | Auto-install a pending update in an idle window. | Yes |
| `usbCfm` | bool | `false` | Require an on-device confirm before a USB-cable update. | Yes |
| `otaNotif` | string | `""` | Last version already Telegram-notified (no re-nag). | Yes (internal) |
| `otaType` | string | board-derived | Typed-OTA device slug (`nimbus-tft`, `freenove-28`, ...) so a board is only offered a matching image. | Yes |
| `otaNotes` | string | `""` | `"ver\|notes"` carried across the install restart. | Yes (internal) |

### Voice, LED, and misc (`src/agent/store.cpp`)

| Key | Type | Default | Holds | Read back? |
|---|---|---|---|---|
| `sttProv` | string | `"mistral"` (Voxtral) | STT provider (`mistral\|openai\|cumulo`). Cumulo Nimbus routes voice through the metered router. **Freely changeable.** | Yes |
| `ttsProv` | string | `"mistral"` | TTS provider (`mistral\|openai\|cumulo`). Cumulo Nimbus routes voice through the metered router. **Freely changeable.** | Yes |
| `ttsVoice` | string | `""` | TTS voice id/slug; `""` = provider default | Yes |
| `tftFlip` | bool | `false` | Display flip: rotate the color panel 180 degrees for an upside-down mount (touch panel only). Applies live; touch mapping follows. The stored value is a delta from the board's correct orientation, which the firmware sets per board, so a fresh unit is upright out of the box on every board (the all-in-one panel is mounted 180 from the hand-built board). | Yes |
| `ttsEnabled` | bool | `true` (on) | Spoken-reply enable (the **Voice replies** toggle) | Yes |
| `theme` | string | `"teal"` | LED color theme slug | Yes |
| `ledBright` | int | `128` | LED brightness (`ledBright` NVS key) | Yes |
| `sfxLvlN` | int | `0` (Off) | Sound-effect level in Notifier mode: `0` Off / `1` Low / `2` Medium / `3` High. | Yes |
| `sfxLvlO` | int | `2` (Medium) | Sound-effect level in Orchestrator mode (same scale). | Yes |
| `sfxTheme` | string | `"pulse"` | Sound theme slug. `pulse` is the only shipping theme; an unknown value is coerced back to `pulse` on write. | Yes |
| `sfxVol` | int | `50` | Master speaker volume, `0..100`. | Yes |
| `saverMin` | int | `-1` (unset) | Screensaver idle threshold in minutes; `0` = off, unset (`-1`) reads as 5 min. Choosing a battery mode on the web page writes that mode's default here (Dark 2 min, Balanced 5 min, Full 0 = off) unless you have set your own value. Set from Settings > Display on the web (the Screen rest field) or the Screensaver row on the device menu; the web control applies live. | Yes |
| `webTok` | string | gen on first use | Per-device web/MCP auth token behind the LAN surface. | No - printed only over UART (`WEBTOK?`) |
| `apPass` | string | gen on first use | Per-device setup-network passphrase, shown on the setup screen. | Yes (setup screen; Settings > Connectivity for a signed-in owner) |
| `codeSbx` | bool | `false` | Code sandbox switch (Assistant > Tools > Tool use): the one control for the hosted code_interpreter on OpenAI and Mistral, saved when it changes. A Code Interpreter connector card from an older version is removed at the first start of this release and, when it was enabled, turns this on. | Yes |
| `orchPromptV2` | bool | `false` | Use the simplified v2 system prompt (A/B flag). | Yes |
| `fetchPol` | int | `1` (ask) | Download trust for `files.fetch`: `0` off / `1` ask per link / `2` scan then keep / `3` full trust (Assistant > Safety > Downloads). | Yes |
| `onbrded` | bool | `false` | First-run onboarding completed. Plain NVS bool (survives a restart with no SD), not the profile override blob. | Yes |
| `scrModel` | string | `"tft"` | Display type (`eink\|tft`), boot-applied. Exempt from Revert to Defaults (hardware identity). `"tft"` is the only supported value and the default: a fresh or NVS-erased device comes up on the color panel silently. Only an **explicit** stored `"eink"` (a real e-ink migration) boots the unsupported-display notice - an absent key never does. On all-in-one boards it is **fixed to `tft`** and the selector is locked - see the note below. | Yes |
| `devTz` | string | `""` (= UTC) | POSIX timezone for daily/weekly routines + the device clock display (Settings → Mode & identity). Applies immediately; wall-clock routines rebase budget-neutrally. | Yes |
| `dreamScrHash` | string | `""` | Legacy: fnv64-hex of the scratchpad after the last dream. Read once to migrate the quiet-night baseline into `/data/dream.json`, never written since v4.5.14. Device-managed. | Yes |
| `tchCal` | string | `""` | Touch-panel calibration (XPT2046); `""` = the board-model default (also the fresh-boot state, which arms the one-time first-run calibration on a resistive panel). Set by the first-run step, Settings > Display > Calibrate touch, or the web/console field; clearing it restores the board-model default. | Yes |
| `lbRing` | bool | `false` | Low-battery ring cue (owner opt-in; off by default) | Yes |
| `lbSaver` | bool | `true` | Auto-drop to a lower battery mode on low battery | Yes |
| `battMon` | bool | board-derived | Battery monitoring on/off. Default is on for hand-built boards (a pack is part of the build) and off for the all-in-one board (a battery is optional, so it is opt-in). Off means the sense pin is never read, the glyph is hidden, and low-battery sleep never fires. Applied at boot. | Yes |

### Battery hardware and chemistry (`src/agent/store.cpp`)

What each value means and how to estimate it without lab tooling is in
[Battery settings and estimation](battery-estimation.md). Divider, capacity,
chemistry, cells, and a custom curve apply live (a divider or chemistry change
re-scales the reading, so the device asks the owner to Recalibrate). Defaults
reproduce the shipped behavior exactly.

| Key | Type | Default | Holds | Read back? |
|---|---|---|---|---|
| `battRtop` | int (ohms) | `220000` | Voltage-divider top resistor. With `battRbot` sets the pack:node ratio. Clamped `1000..10000000`. | Yes |
| `battRbot` | int (ohms) | `100000` | Voltage-divider bottom resistor. | Yes |
| `battCapMah` | int | `3500` | Pack capacity in mAh. Drives the measured-load time-to-empty and the capacity = health x capacity readout. Clamped `100..20000`. | Yes |
| `battChem` | string | `liion` | Battery chemistry: `liion` (Li-ion / LiPo) or `lifepo4` (lithium iron phosphate). Picks the per-cell voltage to state-of-charge curve. | Yes |
| `battCells` | int | `0` (board) | Series-cell count override (`1` or `2`); `0`/absent uses the board default (1S Freenove, 2S Solide). Pack mV / cells = per-cell mV. Clamped to the board's physical cell count: a 1S board cannot be set to 2S (that would read a real 1S pack as an implausible 2S one and reject every sample), so the override is capped and the web tile shows a toast. | Yes |
| `battCurve` | string | `""` | Optional custom per-cell curve, `"mv:pct,mv:pct,..."` high-mV first, strictly descending in mV. Empty uses the chemistry curve. A malformed string is rejected, never stored. | Yes |
| `sleepMv` | int | `3000` x cells (2S `6000`, 1S `3000`) | Low-battery deep-sleep threshold in pack mV; `0` disarms the protection. The default and the clamp ceiling scale with the series-cell count, so a full 1S pack (~4200 mV) is not judged against a 2S floor and slept immediately. | Yes |
| `wakeMv` | int | `3250` x cells (2S `6500`, 1S `3250`) | Stay-awake bar after a low-battery sleep (rested-empty packs read a bit higher than the sleep mark). Scales per cell like `sleepMv`. | Yes |

**`scrModel` vs. the board pinout.** `scrModel` selects the display renderer on a
hand-built Solide S3 board. `"tft"` is the only supported value; `"eink"` is a
frozen legacy value that boots an unsupported-display notice rather than binding a
display. The **board pinout** is a separate, coarser identity fixed at **compile
time** by `SOLIDE_BOARD`
(`solide_s3` or `freenove_s3`): a Freenove all-in-one runs its own firmware image,
so its pinout is baked in, its `scrModel` is fixed to `tft`, and the web display
selector is locked (driven by board id, not by `scrModel`). See the
[hardware reference](../hardware.md#board-configurations).

### Head tool-loop caps (`src/agent/store.cpp`)

The multi-round tool-use loop the head runs per turn. `0`/absent on the byte caps
means "auto" (the engine derives the cap from the model's context window); a set
value is the owner's override and wins under the listed clamp.

| Key | Type | Default | Holds | Read back? |
|---|---|---|---|---|
| `orchLoop` | bool | `true` (on) | Head multi-round tool-use loop enable. | Yes |
| `orchLoopRnds` | int | `8` | Max tool-dispatch rounds per turn. Clamped `1..32`. | Yes |
| `orchLoopDlS` | int | `600` | Wall-clock budget for the loop, in seconds. Clamped `30..3600`. | Yes |
| `orchLoopRCap` | int | `0` (auto) | Per-tool-result byte clamp. `0` = auto; else `512..65536`. | Yes |
| `orchLoopTCap` | int | `0` (auto) | Cumulative tool-output byte budget. `0` = auto; else `2048..1048576`. | Yes |

### Routine governor caps (`src/agent/store.cpp`)

Owner overrides for the routine/scheduler governor. Each defaults to the hard
ceiling in `lib/core/include/nimbus/orch/caps.h`; an override may only make a cap
**stricter**, never looser, and the model can never touch any of it. `0`/absent
means "no override, use the default". The fold is `nimbus::orch::clampLoopCaps`
(a looser value is ignored, not trusted), applied at loops `begin()` and live on
each web write.

| Key | Type | Default (cap) | Holds | Read back? |
|---|---|---|---|---|
| `loopMaxCnt` | int | `8` | Most routines that can exist at once. Override may only lower it. | Yes |
| `loopMinIvl` | int | `300` | Minimum seconds between fires. Override may only raise it. | Yes |
| `loopFires` | int | `24` | Per-routine daily fire ceiling. Override may only lower it. | Yes |
| `loopTokens` | int | `120000` | Per-routine daily token ceiling. Override may only lower it. | Yes |
| `loopDevTok` | int | `400000` | Device-wide daily token ceiling. Override may only lower it. | Yes |
| `loopDevFir` | int | `6` | Device-wide fires per rate window. Override may only lower it. | Yes |

### Guest moderation gates (`src/agent/store.cpp`)

Owner opt-in checks that screen non-admin traffic only (the owner is never
classified). Each costs one classifier call per screened item. Fail behavior is
fixed per gate (see [security.md](../security.md)). Default off.

| Key | Type | Default | Holds | Read back? |
|---|---|---|---|---|
| `modInbound` | bool | `false` | Screen inbound guest/member text before a turn (fail-closed). | Yes |
| `modOutbound` | bool | `false` | Screen outbound replies to guests (fail-open). | Yes |
| `modInject` | bool | `false` | Injection-screen fetched world content (fail-open, marks untrusted). | Yes |

### Anthropic managed-agents caches (`src/agent/store.cpp`)

| Key | Type | Default | Holds |
|---|---|---|---|
| `antEnv` | string | `""` | Managed-agents environment id |
| `antAgents` | string | `""` | Agent map cache |
| `antOrchAg` | string | `""` | Orchestrator agent id |

### Provider verify cache (`src/agent/store.cpp`, written by `provider_verify`)

One key per provider: `vfy_<provider>` (e.g. `vfy_anthropic` = 13 chars, within
the ≤ 15-char NVS limit). Value is `"R:TS"` where:

| `R` | Meaning |
|---|---|
| `1` | verified (HTTP 200) |
| `0` | rejected (401/403) |
| `-1` | couldn't verify (no key / connect failed / never attempted) |

`TS` = `millis()` when the result landed. An **absent** key reads `R = -1`,
`TS = 0`, which distinguishes "never verified" from a real verdict.

For Telegram there is also a **live** signal that does not live in this cache: the
poll loop debounces consecutive getUpdates auth failures (401/403) into a "token
rejected" verdict (`agent::telegram::authRejected()`, RAM-only, not persisted).
When it trips, `/api/orch` reports `tgAuthFail = true` and overrides `tgVerify` to
`0` so a token revoked after it was verified (a key rotation) stops reading
"verified". A successful poll clears it; a 409 conflict or a transient
network error never trips it.

A companion key `vrs_<provider>` (e.g. `vrs_anthropic` = 13 chars, written by
`provider_verify` alongside the verdict) holds a short machine token explaining a
non-verified outcome, surfaced in the web JSON as `vfyReason` for the badge copy:
`nocredits` (HTTP 402: needs credits or subscription), `router_outdated` (HTTP 404
from the Cumulo `/router/models` probe), `deferred` (skipped: the largest
contiguous internal block was below the handshake floor), `connectfail` (TCP/TLS
connect failed), `tlsbusy` (could not take the work TLS slot), or `""` when the
verdict carries its own copy (verified, or a plain rejection) or is a generic
transient. It is cleared to `""` whenever the verdict is `1`, so a stale reason
never lingers after a provider comes good.

### Capability validation (`src/agent/store.cpp`)

Controls whether the device claims a provider capability is "verified" and how
often it re-checks. Both feed the provider catalog (`connectors::catalog()`) and
are read back into the web UI.

| Key | Type | Default | Holds | Read back? |
|---|---|---|---|---|
| `capProbe` | int | `1` (passive) | Capability-validation mode: `0` = off (trust key presence, make no "verified" claim); `1` = passive (report the cached verify verdict); `2` = active (passive **plus** a periodic free provider re-verify). Clamped `0..2`. | Yes |
| `capProbeH` | int | `24` | Active-mode re-verify interval, in hours. Clamped `1..168`. | Yes |

### Embedding config for the vector memory (`src/agent/store.cpp`)

**Set-once** - see the invariant below. Defaults are OpenAI
`text-embedding-3-small` truncated to 256 dims.

| Key | Type | Default | Holds |
|---|---|---|---|
| `embProv` | string | `"openai"` | Embedding provider (`openai\|mistral\|cumulo`). `cumulo` routes the OpenAI embed models through the Cumulo Nimbus router with the one router key, metered on credits like every other router call, so a device that carries only a Cumulo key still has working memory. |
| `embModel` | string | `"text-embedding-3-small"` | Embedding model |
| `embDims` | int | `256` | Embedding width. Must equal what the model returns: Mistral has no width field, so `mistral-embed` needs `1024`. Saving `0` stores the model's own width, read with one live call |
| `embLocked` | bool | `false` | Flips `true` the first time a vector is embedded |

## Credential-gate rails (model vs. human)

The rails keep the model from redirecting its own brain or granting itself
access. There are two enforcement layers:

**1. `store.cpp` - human-only setters.** The write side of the credential gate.
The setters for keys, `orchHost`, `provPrio` (provider-**host** list), and
`sysPrompt` may be called **only** from surfaces a human drives directly (the
config web page, the provision/test consoles) - never from any code path the
model can reach (device-action executor, turn loop, tools). `setSubPriority()`
is the **only** model-writable routing knob; it targets the **sub-session** list
(`subPrio`), never the orchestrator-host list.

**2. `lib/core` - the deny policy (portable security core).** The model's
`config` action carries only the benign owner knobs the wire schema lists
(`ledBrightness`, `priority` - a routing **preference**, i.e. the sub-session
list, not a host - `posture`, `profile`, `theme`, `attnHoldMs`, the voice and
sound knobs, `ttsOn`, `sleepOvr`, `brightOvr`, `devName`), each clamped; a
scheduled turn may not set `ttsOn`, `devName`, `sleepOvr` or `brightOvr`. Everything else is
ignored (unknown) or **BLOCKED**. The protected list
(`lib/core/src/orch_device_actions.cpp`, nullptr-terminated) is:

| Key | Why blocked |
|---|---|
| `password`, `token`, `connector` | Secrets - an LLM-issued chat action must never set a credential |
| `allowlist` | The device's own auth gate - the model must not grant itself access |
| `orchHost`, `fabric` | Provider / sub-agent routing - the model must not redirect its own brain / sub-agent backends |
| `loops` | The routines blob - only the `loop.*` tools write it |
| `providerPriority`, `provPrio`, `orchPriority` | The orchestrator-host priority list under every alias |
| `tgToken`, `tgAllow`, `sysPrompt` | The Telegram credential and allowlist, and the owner directive |
| `orchLoop`, `budget`, `tlsSlots` | The tool-loop and budget governors and the TLS slot count |
| `oaiKey`, `antKey`, `mistralKey`, `custKey`, `tavilyKey` | Provider API keys, by name; any other key ending in "Key" (for example `zaiKey`, `cumuloKey`) is refused by a generic backstop |

If **any** protected key is present, the **whole** config action is refused
(reason `"protected-BLOCKED"`). The reason string reports only the policy tag,
never the blocked key's value, so a secret can never leak into the log or the
next-turn context.

See [Orchestrator World](../orchestrator-world.md)
for these rails in the wider control-surface context.

## Sparse-override Config + the SD blob

Battery **modes** (machine ids `BatterySaver`, `Balanced`, `Desk` - displayed
as Dark / Balanced / Full) are named sets of defaults. The `Config` model (`lib/core/include/nimbus/profile.h`)
layers **sparse user overrides** on top of the active profile:

```
effective(key) = userOverride(key) ?? activeProfile.preset(key)
```

Only keys the user actually touched are stored, so switching profiles never
loses user intent. `Config` holds the active `ProfileId` plus a per-`Param`
`has_[]`/`val_[]` override pair over the `Param` enum
(`kParamCount = Param::COUNT`).

**Serialization** (`lib/core/include/nimbus/config_store.h`,
`kConfigStoreVersion = 2`): little-endian, magic `'N' 'C'`, then version,
profile, count, and `count` × `{ param:u8, value:i32 }` records - only the
overridden params. Header is `kConfigHeaderBytes = 5`, each record
`kConfigRecordBytes = 5`, cap
`kConfigMaxBytes = kConfigHeaderBytes + kParamCount * kConfigRecordBytes`.
`deserializeConfig()` is **all-or-nothing**: any bad magic/version/param/length
leaves `out` untouched and returns `false`, so a corrupt blob falls back to
defaults rather than a half-applied config.

**Persistence glue** (`src/sys/config_nvs.cpp`): the serialized `Config` blob is
stored under name `nimbus_cfg` via `solide::memory::putBlob` - the **SD-backed**
half of the store. Everything degrades gracefully: with no backing store
(`solide::memory::ok()` false, blob absent, or corrupt) `loadConfig` returns
`false` / leaves defaults and `saveConfig` returns `false`, never crashing the
caller.

### F10 - no-SD non-persistence limitation

Because the `Config` override blob rides the **SD** half of `solide::memory`
(`putBlob`), **with no SD card the overrides do not survive a restart**. Only
`nimbus_mode` (an NVS int) and the other NVS keys persist. This is a known open
limitation (**F10**). The hardware test `R_F10_persist_no_sd` encodes the intended
behavior and is marked as an expected failure until the blob moves to a store that
survives without an SD card.

See [Tools & Commands](../tools-and-commands.md) for provisioning over
serial (`[env:provision]` or the test console `WIFI` / `MODE` commands).

## Set-once embed-config invariant

Vectors embedded under different `provider`/`model`/`dims` are **incomparable**,
so the embed config is **set-once**: `embLocked` flips `true` the first time any
vector is embedded, and after that changing the config invalidates the whole
vector DB (VDB).

`store::setEmbedConfig()` itself is a plain accessor that reports whether all three
values landed - the reset/lock **policy** lives with the caller. The server-side guard
is `POST /api/mem/embedcfg` (`src/net/web_memory.cpp`): while locked, a change is
refused with **HTTP 409** unless `reset=1` is passed. The route stores the new config
**first**; if it does not land (a full NVS) it puts the old config back and answers
**HTTP 507** `storage_full` without touching the memories. `reset=1` is the
destructive path: after the config is stored it **flushes and erases the vector store**
(`flushAll` + `persistVectors`), clears `embLocked`, and reconfigures the engine to the
new dims. A change can therefore never silently strand incomparable vectors; the web
UI must warn and require the explicit reset. `provider` must be `openai`,
`mistral`, or `cumulo` and `model` is required (else HTTP 400). `dims=0` is
resolved to the model's own width with one live embeddings call before it is
stored (HTTP 400 with the cause if that call fails), so the vector store never
holds a width no model fills. At boot an empty vector store takes the configured
width, so an erased store never keeps the width of the config it replaced.
