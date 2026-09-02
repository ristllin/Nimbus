# Wi-Fi resilience: saved networks and automatic failover

Orchestrator mode keeps the device online the way a phone does. It remembers several
Wi-Fi networks, and when the one it is using goes away it scans and joins the next
reachable saved network on its own, with no setup step and no restart. This page
describes what the device stores, how it chooses, and how that cooperates with the
first-run setup hotspot.

Notifier mode has no Wi-Fi: its transport is Bluetooth and the radio stays off. None
of this applies there.

## What is stored

The device keeps a bounded list of up to **five** saved networks. Each entry is an
SSID, its password (an open network stores an empty password, which is a real value,
not "unset"), an auto-join flag, and the day it last connected.

Five is a deliberate cap, not a technical limit. It comfortably covers a home
network, an office, a phone hotspot, and a couple of travel or guest networks, while
staying small enough to hold in a single NVS record that is read on the very first
boot step, long before any filesystem is mounted. When the list is full, adding a
genuinely new network evicts the least recently joined entry, and the network you are
currently connected to is never the one dropped. The web page refuses to silently
evict: at capacity it asks you to forget one first.

The list lives in NVS under a new key and never touches the older single-network
keys except to read them once: on the first boot after upgrading, the legacy single
credential is folded in as the first saved network, and the list head is mirrored
back to those legacy keys so that an OTA rollback to an older image still finds a
working network. A factory reset (`nvs_flash_erase`) wipes the whole list, exactly as
the danger-zone copy promises.

## How the device chooses

Selection is **scan-then-match**, not retry-one-forever. When the link is down the
device scans, intersects what the radio can actually see with the saved list, and
tries them **strongest signal first**. Saved order is only a tie-break between
networks of equal strength, so it acts as a light priority hint. A successful join
promotes that network to the top of the list, so the order also tracks what has
worked recently.

If a join fails, the reason decides what happens next:

- **Network not found** or **wrong password**: move to the next candidate at once.
  Retrying either is exactly the airtime waste this design removes.
- **A transient error**: one more attempt on the same network, then move on.
- **Our own deliberate disconnect**: never counted as a failure.

When every reachable saved network has been tried and none worked, the device enters
a slow-retry state: it keeps looking on a backoff schedule (about 30 seconds growing
to a 5-minute ceiling, roughly one percent radio duty) rather than scanning
continuously. Continuous scanning would starve the setup hotspot's beacons on the
single shared 2.4 GHz radio and lock you out, so the slow schedule is a hard rule
with its own test.

## Failover and the setup hotspot

During first-time setup the "…-setup" hotspot is the only way in, and the firmware
protects it (see [security](./security.md) and the setup-AP recovery logic). Multi-
network failover is designed to cooperate with that, not fight it:

- Failover only engages once **onboarding is finished** and there are at least **two**
  saved networks. A fresh device, or one with a single network, uses the same paths
  it always has.
- While the device is actively working through its saved networks, "trying the next
  network" counts as **progress, not churn**. The setup hotspot is not brought back
  during that window.
- The setup hotspot returns only once failover is **exhausted** (every saved network
  tried, none reachable). That is the point at which a person genuinely needs it.

Because failover only engages after a short grace once the link is lost, and never
while someone is connected to the setup hotspot or a manual join is in flight, the
common cases (a brief router blip, a normal single-network reconnect, first-run
setup) behave exactly as before.

That hold on failover is always temporary. If you join a network by hand (say a phone
hotspot) and it then disappears, the device does not stay silent waiting for a network
that is gone: once that hand-picked attempt has had its short window to connect and has
not, the hold releases and the device goes back to trying your other saved networks on
its own, with no restart. The same is true if a person was on the setup hotspot during
the outage and then leaves. You asked for that network, not for the radio to go quiet
until the next power cycle.

## What you see

The device screen tells the truth about what the radio is doing. When it is failing
over across more than one reachable saved network, the status reads for example
`Joining Office 2/3...`, so you can see it working through the list rather than
appearing stuck on one name. A single candidate shows the plain `Joining <name>...`.

## Where you manage it

The saved-network list is managed on the web page under **Settings → Connectivity**:
add a network, forget one, reorder priority, and see which is connected. A password
is never shown back; joining a saved network reuses the stored one. This is the
canonical home for the list.

The device screen's **Settings → Wi-Fi** is the second surface. It shows the live
status and a scan-and-join picker for getting online, but not the full list editor.
Both surfaces write into the one shared list.

## Where the logic lives

If this page disagrees with the code, the code wins.

| Thing | File |
|---|---|
| Saved-network model (store, dump/load, ranking, migration) | `lib/core/src/wifi_known_networks.cpp` |
| Selection / failover state machine | `lib/core/src/wifi_policy.cpp` |
| Supervisor engage / bow-out / re-begin decision | `lib/core/src/wifi_supervise.cpp` (`decideSupervise`) |
| Setup-AP recovery decision | `lib/core/src/setup_ap.cpp` (`decideSetupAp`) |
| Status copy ("Joining X 2/3...") | `lib/core/src/wifi_copy.cpp` |
| Device seam wiring the machine to the radio | `src/net/wifi_link.cpp` |
| NVS glue for the list | `src/net/wifi_store.cpp` |
| Web management endpoints (`/api/wifi`) | `src/net/webui.cpp` |

Host tests: `test/test_wifi_known`, `test/test_wifi_policy`, `test/test_wifi_recovery`,
`test/test_wifi_supervise`, `test/test_wifi_copy`. The two-access-point migration is a
bench (hardware) test,
`tests/hil/test_l33_multi_network_failover.py`, marked manual because it needs a
person to power one access point down.
