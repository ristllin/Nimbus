# Security policy

## Reporting a vulnerability

Please report vulnerabilities privately via
**GitHub Security Advisories**: on the repository page, Security → Report a
vulnerability. Do not open a public issue for anything exploitable.

You can expect an acknowledgement within a week. Fixes ship as a signed OTA
release; credit is given unless you prefer otherwise.

## Scope notes

Nimbus is a hobbyist desk device, designed for a trusted home LAN:

- Web/API access is gated by a per-device random access token (carried in the
  sign-in QR). Treat the QR like a password.
- OTA updates are ECDSA-signed and verified on-device before install; `test`
  builds accept an additional publicly-known test key and therefore never poll
  for updates on their own - don't daily-drive a `test` build.
- The device's threat model, accepted risks, and open hardening items are
  documented honestly in `docs/security.md`. Items listed there as accepted
  risks are known; reports that add new impact or a practical exploit path for
  them are still very welcome.
