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
| `sysPrompt` | string | `""` | The user directive - **immutable by the model** | Yes |

All three hosts (OpenAI, Anthropic, Mistral) are in the default `provPrio` /
`subPrio` lists; list order is the failover order, and an explicit `orchHost`
overrides it.

Default per-provider models: `OPENAI_MODEL` = `gpt-5.5`, `ANT_MODEL` =
`claude-sonnet-4-6`, `MISTRAL_MODEL` = `mistral-large-latest`; custom →
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
| `tgOffset` | int | `0` | Telegram long-poll offset | Yes (internal) |

### Voice, LED, and misc (`src/agent/store.cpp`)

| Key | Type | Default | Holds | Read back? |
|---|---|---|---|---|
| `sttProv` | string | `"mistral"` (Voxtral) | STT provider (`mistral\|openai`). **Freely changeable.** | Yes |
| `ttsProv` | string | `"mistral"` | TTS provider (`mistral\|openai`). **Freely changeable.** | Yes |
| `ttsVoice` | string | `""` | TTS voice id/slug; `""` = provider default | Yes |
| `ttsEnabled` | bool | `false` | Spoken-reply enable | Yes |
| `theme` | string | `"teal"` | LED color theme slug | Yes |
| `ledBright` | int | `128` | LED brightness (`ledBright` NVS key) | Yes |
| `scrModel` | string | `"eink"` | Display type (`eink\|tft`), boot-applied. Exempt from Revert to Defaults (hardware identity). On all-in-one boards it is **fixed to `tft`** and the selector is locked - see the note below. | Yes |
| `devTz` | string | `""` (= UTC) | POSIX timezone for daily/weekly routines + the device clock display (Settings → Mode & identity). Applies immediately; wall-clock routines rebase budget-neutrally. | Yes |
| `dreamScrHash` | string | `""` | fnv64-hex of the scratchpad after the last dream - the quiet-night skip baseline. Device-managed. | Yes |
| `tchCal` | string | `""` | Touch-panel calibration (XPT2046); `""` = identity/default | Yes |
| `lbRing` | bool | `false` | Low-battery ring cue (owner opt-in; off by default) | Yes |
| `lbSaver` | bool | `true` | Auto-drop to a lower battery mode on low battery | Yes |

**`scrModel` vs. the board pinout.** `scrModel` picks the display and input
**family** (renderer + touch/knob) on a hand-built Solide S3 board, where one
firmware image serves both the e-ink and TFT builds. The **board pinout** is a
separate, coarser identity fixed at **compile time** by `SOLIDE_BOARD`
(`solide_s3` or `freenove_s3`): a Freenove all-in-one runs its own firmware image,
so its pinout is baked in, its `scrModel` is fixed to `tft`, and the web display
selector is locked (driven by board id, not by `scrModel`). See the
[hardware reference](../hardware.md#board-configurations).

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
