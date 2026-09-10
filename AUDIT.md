# Action-button feedback audit (CUM-385)

Every control in the device web UI that performs an action should confirm success
and surface failure. This audits every action-performing control in
`include/web/ui_*.h` plus its handler in `include/web/ui_js.h`, records the feedback
state found, and lists the fix where one was needed. Pure navigation, help (`?`)
toggles, copy-to-clipboard, and read-only displays are out of scope (they perform no
action).

Feedback vocabulary (all in `ui_js.h`):

- `toast(msg)` - transient confirmation, sentence case, verb-led.
- `failToast(e)` - "Sign in required" on 401, else "Couldn't save - try again".
- `jok(r)` - throws on a non-2xx so `.catch` fires instead of a false success.
- `run({status,btn,pending,ok,error})` - the CUM-31 feedback-state driver: renders
  pending then exactly one of ok / none / error into a `[data-fb]` element.
- `apply()` / `orchApply()` - POST helpers that `toast('Saved')` + reload on success
  and `failToast` on error.

## The two owner-named examples

- **`#cxSetupAp`** (`ui_device.h:248`) is **not an action control**. It is a
  display-toggle info block (setup Wi-Fi network name + password), shown only while
  the setup AP is up (`ui_js.h` visibility toggle). The control that actually starts
  the setup network is **`#wifiAp`** ("Publish Setup Network"), which already toasts
  "Setup network published" and surfaces errors into `#wifiApMsg`. No silent action.
- **`#factoryReset`** (`ui_device.h:352`) already confirms: a type-to-confirm modal,
  then a full-page "Device is resetting..." interstitial on success and
  `toast('Reset failed - try again')` on error (`ui_js.h` factory-reset handler).

Both owner reports predate the current modal/interstitial and toast wiring; verified
still-good and left unchanged.

## Fixes applied in this lane

| Control (#id) | Was | Now |
|---|---|---|
| Battery hardware Save `#protSave` | Wired **inside** the Calibrate handler, so Save did nothing until Calibrate was pressed first; POST had no `.catch` (silent on failure). Also gated on `bt.valid`. | Wired unconditionally; `toast('Battery settings saved')` on success, `failToast` on error. (CUM-386 restructure.) |
| Deduplicate memories `#memdedupe` | Silent on success and failure (`memVecOp` had no toast, no `.catch`). | `toast('Duplicates removed')`; `failToast` on error. |
| Delete Temporary `#memflushnp` | Confirm modal, then silent. | `toast('Temporary memories deleted')`; `failToast`. |
| Delete All memories `#memflushall` | Typed confirm, then silent. | `toast('All memories deleted')`; `failToast`. |
| Per-memory delete / pin / unpin | List re-render only, no toast, no `.catch`. | `toast('Memory deleted' / 'Memory pinned' / 'Memory unpinned')`; `failToast`. |
| Automatic updates `#autoUpd` | Success toast only; POST had no `.catch` (silent on failure). | `jok` + `failToast` so a failed save is surfaced. |
| Add Telegram member `#tgAddBtn` | List refresh only, no success toast (error via `failToast`). | `toast('Member added')`. |
| Open access `#tgPublic` | List refresh only, no success toast. | `toast('Open access on' / 'Open access off')`. |
| Mic Meter `#micBtn` | Poll `.catch(()=>{})` swallowed a transport failure; the meter kept polling in silence. | On failure it stops the poll and shows "Mic meter stopped - device unreachable." in `#audiomsg`. |

`memVecOp` gained an optional success-message argument and a `.catch(failToast)`, so
every memory vector operation (dedupe, flush, per-item delete/pin) now confirms and
surfaces errors from one place.

## Full inventory

Verdict key: OK = confirms success and surfaces error; FIXED = corrected in this lane
(see table above); N/A = not an action control.

### Dashboard / Home
| Control | Feedback | Verdict |
|---|---|---|
| Restart `#homeRestart` | interstitial + `.catch` toast | OK |
| Power off `#homePowerOff` | interstitial + `.catch` toast | OK |
| Dismiss `#whatNextDismiss` | local UI toggle | N/A |
| Open Chat / Add a file / Providers / Check for updates | navigation | N/A |

### Mode & identity
| Control | Feedback | Verdict |
|---|---|---|
| Mode `#mode` | restart-interstitial flow; reverts on cancel | OK |
| Device name Save `#devNameSave` | `apply` toast + `failToast` | OK |
| Timezone Save `#devTzSave` | `apply` toast + `failToast` | OK |
| Sync now `#clockSyncBtn` / `#govSync` | `apply` toast + `failToast` | OK |
| Device sign-in code `#idToken` | copy only | N/A |

### Display
| Control | Feedback | Verdict |
|---|---|---|
| Display flip `#scrFlip` | state toast + `.catch(failToast)` | OK |
| Screen rest `#saverMin` | state toast + `.catch(failToast)` | OK |
| Touch calibration Save `#tchCalSave` | validation toasts + `orchApply` | OK |
| Touch orientation `#tSwap`/`#tFlipX`/`#tFlipY` | toast + `.catch(failToast)` | OK |
| Theme chips `#themeChips` | `orchApply` toast | OK |
| Demo on Device `#prevBtn` | toast + `.catch(failToast)` | OK |

### Battery mode
| Control | Feedback | Verdict |
|---|---|---|
| Battery mode radios `input[name=profile]` | `apply` toast + `failToast` | OK |
| Low-battery light `#lbRing` | state-aware toast + `.catch(failToast)` | OK |
| Save power when low `#lbSaver` | state-aware toast + `.catch(failToast)` | OK |
| Monitor the battery `#battMon` | toast + reveals restart row + `.catch(failToast)` | OK |
| Restart now `#battRestart` | restart interstitial | OK |

### Customize battery mode
| Control | Feedback | Verdict |
|---|---|---|
| Revert to Defaults `#revertProf` | success line `#revertMsg`; error via `apply`/`failToast` toast | OK |

### Sound
| Control | Feedback | Verdict |
|---|---|---|
| Mic Meter `#micBtn` | (see fixes) | FIXED |
| Speaker Tone `#beepBtn` | `#audiomsg` verdict + `.catch` | OK |
| Loopback Test `#lbBtn` | `#audiomsg` verdict + `.catch` | OK |
| SFX levels `#sfxLvlO`/`#sfxLvlN`, theme `#sfxTheme`, volume `#sfxVol` | `orchApply` toast | OK |
| Play `#sfxPlay` | toast + `.catch` | OK |

### Battery (hardware)
| Control | Feedback | Verdict |
|---|---|---|
| Calibrate Full Charge `#battcalBtn` | `run({status:'battcalMsg'})`; disabled with an honest tooltip when there is no reading | OK |
| Save `#protSave` | (see fixes) | FIXED |

### Software update
| Control | Feedback | Verdict |
|---|---|---|
| Check for Updates `#fwCheck` | `run` ok/none/error | OK |
| Install Update `#fwInstall` | `run` + battery-gate confirm | OK |
| Automatic updates `#autoUpd` | (see fixes) | FIXED |

### Connectivity
| Control | Feedback | Verdict |
|---|---|---|
| Generate New Code `#regenTok` | toast + `.catch` toast | OK |
| Scan Networks `#scan` | `#msg` countdown + result / error | OK |
| Save hidden network `#savewifi` | `#msg` result / error | OK |
| Publish Setup Network `#wifiAp` | toast + `#wifiApMsg` error | OK |
| Resume Joining `#wifiResume` | toast + `#wifiApMsg` error | OK |
| Forget Paired Devices `#btForget` | toast + `.catch` toast | OK |
| Saved-network Forget / Reorder / Connect | toasts / `#msg` | OK |
| `#cxSetupAp` info block, `#cxLanCopy` copy | display toggle / copy | N/A |

### Cloud access
| Control | Feedback | Verdict |
|---|---|---|
| Pair / Unpair / Turn off `#cloudPair`/`#cloudUnpair`/`#cloudOff` | `run({status:'cloudMsg'})` | OK |
| Copy code `#cloudCopy` | copy only | N/A |

### Power
| Control | Feedback | Verdict |
|---|---|---|
| Restart... `#deviceRestart` | interstitial + `.catch` toast | OK |
| Power Off... `#powerOff` | interstitial + `.catch` toast | OK |

### Danger zone
| Control | Feedback | Verdict |
|---|---|---|
| Erase Storage... `#sdReset` | toast + `.catch` toast | OK |
| Format Card... `#sdFormat` | toast + error toast | OK |
| Factory Reset... `#factoryReset` | interstitial + `.catch` toast | OK |

### Assistant pane
| Control | Feedback | Verdict |
|---|---|---|
| Save Changes (Models) `#orchsave` | `saveVerdict` persistent line | OK |
| Save Changes (Tool use) `#toolusesave` | `#toolusemsg` line | OK |
| Connectors Save JSON / per-card Save / Remove | toast + `.catch(failToast)` | OK |
| Provider key Save & Verify `saveAndVerify` | per-provider `#pmsg_*` verify status | OK |
| Telegram token Save / Clear | `orchApply` toast | OK |
| Add member `#tgAddBtn`, Open access `#tgPublic` | (see fixes) | FIXED |
| Voice replies `#ttsOn` | `orchApply` toast | OK |
| Tavily Save / Clear, Custom endpoint Clear | `orchApply` toast | OK |
| Skill Load / Save / Delete / Approve | `#skMsg` lines / toast | OK |

### Memory pane
| Control | Feedback | Verdict |
|---|---|---|
| Directive Save `#savedir` / Revert `#revertdir` | `saveVerdict` / `#dirmsg` line; error via `failToast` toast | OK |
| Clear assistant memory `#clearmem` | `orchApply` toast | OK |
| Clear conversation `#clearconv` | toast | OK |
| Deduplicate `#memdedupe` | (see fixes) | FIXED |
| Delete Temporary `#memflushnp`, Delete All `#memflushall` | (see fixes) | FIXED |
| Per-memory delete / pin | (see fixes) | FIXED |
| Recall tuning Save `#cfgsave` | toast + `.catch(failToast)` | OK |
| Embedding model Save `#embsave` | `#embmsg` verify line + toast | OK |
| Delete folder `#filesRmProj` | toast + `.catch` toast | OK |
| Upload `#upBtn` | `run({status:'upMsg'})` | OK |

### Chat pane
| Control | Feedback | Verdict |
|---|---|---|
| Send `#chatSend` | reply bubble + `#chatMsg`; error bubble | OK |
| Attach `#chatAttach` | `run({status:'chatUpMsg'})` | OK |

### Usage pane
| Control | Feedback | Verdict |
|---|---|---|
| Save Rates `#ratesave` / Save Budgets `#budsave` | `#ratemsg`/`#budmsg` line on success; error via `failToast` toast | OK |

### Loops / Safety pane
| Control | Feedback | Verdict |
|---|---|---|
| Create Routine `#lpCreate` | `#lpMsg` result / error | OK |
| Wake-up policy `#wkPolicy`, Approve `#wkApprove`, Deny `#wkDeny` | `run` feedback state | OK |
| Save Downloads policy `#fetchpolsave` / Guest moderation `#modSave` | `#fetchpolmsg`/`#modmsg` line on success; error via `failToast` toast | OK |

### Onboard wizard
| Control | Feedback | Verdict |
|---|---|---|
| Back / Skip `#onbBack`/`#onbSkip` | step navigation | N/A |
| Next / Finish `#onbNext` | `#onbMsg` progress + completion / gate messages | OK |
| Save & Verify provider (embedded) | `#onb_provmsg` status | OK |

## On-device menu (Settings) action feedback

The physical Settings menu routes action rows (forget pairings, rescan SD, self-test,
factory reset, etc.) through the on-device action-feedback triple (sound + ring cue +
a screen line) added in 438e5f7 (`lib/core` action_feedback + `main.cpp`
`emitMenuActionFeedback`). That path is unchanged by this lane; the web audit above is
the surface this lane owns.
