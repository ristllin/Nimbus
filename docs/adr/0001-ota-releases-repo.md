# ADR 0001: Typed OTA (v2) reuses the existing releases repo

- Status: accepted
- Date: 2026-08-23
- Deciders: N0 lane (boards, flashing, typed OTA)

## Context

OTA moves from an untyped manifest (schema 1, variants keyed on the build tag
`esp32s3`/`cyd`) to a typed manifest (schema 2, variants keyed on the device
**type**: `nimbus-tft`, `freenove-28`, `freenove-35`, `freenove-40`). Existing
devices carry no type yet, and e-ink devices are frozen: they must never receive
another update.

The device already fetches
`https://github.com/ristllin/nimbus-fw-releases/releases/latest/download/manifest.json`
over unauthenticated HTTPS, trusting the ECDSA signature rather than the transport
or the host. The question: publish the v2 manifests into the existing
`ristllin/nimbus-fw-releases` repo, or stand up a new releases repo for the typed
scheme?

## Decision

**Reuse `ristllin/nimbus-fw-releases`.** Each release publishes a single
`manifest.json` whose `variants` map is keyed on the device type. The transition
release additionally publishes a schema-1 `manifest.json` under the tag existing
firmware still polls, so fielded devices can install it and cross into the typed
scheme.

## Rationale

- **The delivery URL is baked into fielded firmware.** `OTA_MANIFEST_URL`
  (`releases/latest/download/manifest.json`) is compiled into every device
  already in the field. A new repo would strand every existing device on the old
  URL, which is exactly the population the transition release exists to migrate.
  Reuse keeps one stable, forever-pollable URL.
- **Signature, not location, is the trust root.** The ECDSA anchors in
  `include/ota_pubkey.h` gate what a device installs; the repo is just a public
  file host. A second repo adds a second thing to secure and rotate for zero
  trust benefit.
- **Common convention.** Assets-only public release repos that accrete releases
  over time are the norm (the SFX assets follow the same pattern). Versioning is
  carried by the release tag and the manifest `version`/`schema` fields, not by
  splitting repos.
- **Schema is self-describing.** `schema: 1` vs `schema: 2` in the same file
  namespace lets old firmware reject a v2 manifest cleanly and new firmware
  reject a v1 one, so the two can coexist during the transition without a repo
  boundary.

## Alternatives considered

- **New repo `nimbus-fw-releases-v2`.** Rejected: strands fielded devices on the
  old URL (defeating the transition), duplicates release automation and secrets,
  and buys no isolation the `schema` field does not already provide.
- **Per-type manifest files (`manifest-nimbus-tft.json`, ...).** Rejected: the
  device parser already selects its variant out of one `variants` map, so N files
  would be N times the publish surface and N chances for a partial release, with
  no gain. One manifest, many variants, stays atomic.

## Consequences

- `tools/make_manifest.py` emits schema 2 by default and `--schema 1` for the
  transition manifest; both are byte-locked to `nimbus::ota::buildSigMessage`
  (`nimbus-ota-v<schema>\n<version>\n<type>\n<sha256-hex>\n`).
- The release workflow must build and sign every typed variant (`nimbus-tft`,
  `freenove-28/35/40`) into the one manifest. A type with no image in a given
  release simply has no entry, and devices of that type report "no update".
- Untyped and e-ink devices match no variant and never install again; this is
  the intended freeze, not an error.
