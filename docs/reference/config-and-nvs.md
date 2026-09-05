# Config and NVS Reference

How Nimbus persists device state: the `solide` NVS namespace and every key it
holds, the write-only credential fields, the model-vs-human credential-gate
**rails**, the sparse-override `Config` model and its SD-backed blob (plus the
no-SD non-persistence limitation, **F10**), and the **set-once** embed-config
invariant.

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

## NVS keys

Two families write to the `solide` namespace: the **system** keys
(`src/sys/config_nvs.cpp`) and the **agent/orchestrator** keys
(`src/agent/store.cpp`, names in `src/agent/agent_config.h`). "Read back?" = No
marks **write-only** fields the UI never reads back into itself.

### System keys (`src/sys/config_nvs.cpp`)

| Key | Type | Default | Holds | Read back? |
|---|---|---|---|---|
| `nimbus_mode` | int | `Notifier` (0) | Operating mode: `0=Notifier`, `1=Orchestrator` (`Mode` enum). Any value other than 1 resolves to Notifier. | Yes |
| `nimbus_ble` | int (bool) | `1` (on) | Bluetooth advertising enable (Notifier-mode nsn transport). Runtime-applied, no reboot. | Yes |
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
| `cumuloBase` | string | `""` | Cumulo router host or full base URL; `""` uses the built-in default host (`app.cumulo-nimbus.ai`) | Yes |

Writing or clearing a provider key **resets that provider's verify cache** to
`-1, 0` (couldn't-verify / never), so a swapped key can't ride the old verdict.

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
| `tgNames` | string | `""` | Display-name sidecar for known chats (`id:name,...`). | Yes |
| `tgBotName` | string | `""` | The connected bot's `@username` (from getMe), display only. | Yes |
| `tgPublic` | bool | `false` | Open access: accept anyone who messages the bot. Default off. | Yes |
| `tgOffset` | int | `0` | Telegram long-poll offset | Yes (internal) |

### Cloud access / relay (`src/agent/store.cpp`)

Reaching the device from anywhere through CumuloNimbus. Orchestrator-only; ships
dark (off). See [Cloud access](../cloud-relay.md) for the pairing flow and the
security model.

| Key | Type | Default | Holds | Read back? |
|---|---|---|---|---|
| `cloudOptIn` | bool | `false` | Cloud relay enabled. | Yes |
| `cloudDevId` | string | `""` | Cloud device id assigned at pairing. | Yes |
| `cloudCred` | string | `""` | Cloud-minted device credential (bearer); wiped on unpair. | No - write-only |
| `cloudHost` | string | `app.cumulo-nimbus.ai` | Relay host. | Yes |
| `cloudName` | string | `""` | Paired device display name for the web status line. | Yes |

### OTA / update engine (`src/agent/store.cpp`)

Persisted across the install reboot so the device can validate a fresh image and
roll back if it fails. See [OTA updates](../ota.md).

| Key | Type | Default | Holds | Read back? |
|---|---|---|---|---|
| `otaPend` | bool | `false` | A fresh image is awaiting first-boot validation. | Yes (internal) |
| `otaBoots` | int | `0` | Boot attempts since the slot flip. | Yes (internal) |
| `otaPrev` | string | `""` | Previous app slot label (`app0`/`app1`) for rollback. | Yes (internal) |
| `otaLast` | string | `""` | Last OTA outcome (`ok vX` / `rollback vX` / ...). | Yes |
| `autoUpd` | bool | `false` | Auto-install a pending update in an idle window. | Yes |
| `otaNotif` | string | `""` | Last version already Telegram-notified (no re-nag). | Yes (internal) |
| `otaType` | string | board-derived | Typed-OTA device slug (`nimbus-tft`, `freenove-28`, ...) so a board is only offered a matching image. | Yes |
| `otaNotes` | string | `""` | `"ver\|notes"` carried across the install reboot. | Yes (internal) |

### Voice, LED, and misc (`src/agent/store.cpp`)

| Key | Type | Default | Holds | Read back? |
|---|---|---|---|---|
| `sttProv` | string | `"mistral"` (Voxtral) | STT provider (`mistral\|openai`). **Freely changeable.** | Yes |
| `ttsProv` | string | `"mistral"` | TTS provider (`mistral\|openai`). **Freely changeable.** | Yes |
| `ttsVoice` | string | `""` | TTS voice id/slug; `""` = provider default | Yes |
| `tftFlip` | bool | `false` | Display flip: rotate the color panel 180 degrees for an upside-down mount (touch panel only). Applies live; touch mapping follows. The stored value is a delta from the board's correct orientation, which the firmware sets per board, so a fresh unit is upright out of the box on every board (the all-in-one panel is mounted 180 from the hand-built board). | Yes |
| `ttsEnabled` | bool | `false` | Spoken-reply enable | Yes |
| `theme` | string | `"teal"` | LED color theme slug | Yes |
| `ledBright` | int | `128` | LED brightness (`ledBright` NVS key) | Yes |
| `sfxLvlN` | int | `0` (Off) | Sound-effect level in Notifier mode: `0` Off / `1` Low / `2` Medium / `3` High. | Yes |
| `sfxLvlO` | int | `2` (Medium) | Sound-effect level in Orchestrator mode (same scale). | Yes |
| `sfxTheme` | string | `"pulse"` | Sound theme slug. `pulse` is the only shipping theme; an unknown value is coerced back to `pulse` on write. | Yes |
| `sfxVol` | int | `50` | Master speaker volume, `0..100`. | Yes |
| `saverMin` | int | `-1` (mode default) | Screensaver idle threshold in minutes; `0` = off, unset (`-1`) uses the battery-mode default (5 min). Set from Settings > Display on the web (the Screen rest field) or the Screensaver row on the device menu; the web control applies live. | Yes |
| `webTok` | string | gen on first use | Per-device web/MCP auth token behind the LAN surface. | No - printed only over UART (`WEBTOK?`) |
| `apPass` | string | gen on first use | Per-device setup-network passphrase, shown on the setup screen. | Yes (on-device only) |
| `codeSbx` | bool | `false` | Code sandbox toggle (Assistant > Tools). | Yes |
| `orchPromptV2` | bool | `false` | Use the simplified v2 system prompt (A/B flag). | Yes |
| `fetchPol` | int | `1` (ask) | Download trust for `files.fetch`: `0` off / `1` ask per link / `2` scan then keep / `3` full trust (Assistant > Safety > Downloads). | Yes |
| `onbrded` | bool | `false` | First-run onboarding completed. Plain NVS bool (survives a reboot with no SD), not the profile override blob. | Yes |
| `scrModel` | string | `"tft"` | Display type (`eink\|tft`), boot-applied. Exempt from Revert to Defaults (hardware identity). `"tft"` is the only supported value and the default: a fresh or NVS-erased device comes up on the color panel silently. Only an **explicit** stored `"eink"` (a real e-ink migration) boots the unsupported-display notice - an absent key never does. On all-in-one boards it is **fixed to `tft`** and the selector is locked - see the note below. | Yes |
| `devTz` | string | `""` (= UTC) | POSIX timezone for daily/weekly routines + the device clock display (Settings → Mode & identity). Applies immediately; wall-clock routines rebase budget-neutrally. | Yes |
| `dreamScrHash` | string | `""` | fnv64-hex of the scratchpad after the last dream - the quiet-night skip baseline. Device-managed. | Yes |
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
| `battCells` | int | `0` (board) | Series-cell count override (`1` or `2`); `0`/absent uses the board default (1S Freenove, 2S Solide). Pack mV / cells = per-cell mV. | Yes |
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
| `orchLoopRnds` | int | `12` | Max tool-dispatch rounds per turn. Clamped `1..32`. | Yes |
| `orchLoopDlS` | int | `600` | Wall-clock budget for the loop, in seconds. Clamped `30..3600`. | Yes |
| `orchLoopRCap` | int | `0` (auto) | Per-tool-result byte clamp. `0` = auto; else `512..65536`. | Yes |
| `orchLoopTCap` | int | `0` (auto) | Cumulative tool-output byte budget. `0` = auto; else `2048..1048576`. | Yes |

### Local Loops governor caps (`src/agent/store.cpp`)

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
| `embProv` | string | `"openai"` | Embedding provider (`openai\|mistral`) |
| `embModel` | string | `"text-embedding-3-small"` | Embedding model |
| `embDims` | int | `256` | Embedding width; `0` = provider-native |
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

**2. `lib/core` - the deny policy (portable security core).** The model may
tune exactly two config keys: `ledBrightness` (0..255) and `priority` (a routing
**preference**, i.e. the sub-session list - not a host). Everything else is
ignored (unknown) or **BLOCKED**. The protected list
(`lib/core/src/orch_device_actions.cpp`, nullptr-terminated) is:

| Key | Why blocked |
|---|---|
| `password`, `token`, `connector` | Secrets - an LLM-issued chat action must never set a credential |
| `allowlist` | The device's own auth gate - the model must not grant itself access |
| `orchHost`, `fabric` | Provider / sub-agent routing - the model must not redirect its own brain / sub-agent backends |

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
`kConfigStoreVersion = 1`): little-endian, magic `'N' 'C'`, then version,
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
(`putBlob`), **with no SD card the overrides do not survive reboot** - only
`nimbus_mode` (an NVS int) and the other NVS keys persist. This is a known open
bug (**F10**), flagged in P4 review and not yet fixed. The device
`test_persist_across_reboot` / `R_F10_persist_no_sd` test encodes the fix and
**xfails** until the blob is moved to a store that survives without SD.

See [Tools & Commands](../tools-and-commands.md) for provisioning over
serial (`[env:provision]` or the test console `WIFI` / `MODE` commands).

## Set-once embed-config invariant

Vectors embedded under different `provider`/`model`/`dims` are **incomparable**,
so the embed config is **set-once**: `embLocked` flips `true` the first time any
vector is embedded, and after that changing the config invalidates the whole
vector DB (VDB).

`store::setEmbedConfig()` itself is a plain accessor - the reset/lock **policy**
lives with the caller. The server-side guard is `POST /api/mem/embedcfg`
(`src/net/web_memory.cpp`): while locked, a change is refused with **HTTP 409**
unless `reset=1` is passed. `reset=1` is the destructive path - it
**flushes and wipes the VDB** (`flushAll` + `persistVectors`), clears
`embLocked`, then writes the new config and reconfigures the engine to the new
dims. A change can therefore never silently strand incomparable vectors; the web
UI must warn and require the explicit reset. `provider` must be `openai` or
`mistral` and `model` is required (else HTTP 400).
