#pragma once

// OTA release-signing TRUST ANCHORS. The device accepts a firmware update only
// when the release's ECDSA P-256 signature (over the canonical message
// "nimbus-ota-v1\n<version>\n<variant>\n<sha256-hex>\n" - nimbus::ota::
// buildSigMessage) verifies against one of these public keys. The private key
// lives ONLY in the GitHub Actions secret OTA_SIGNING_KEY + the owner's offline
// backup - never in this repo.
//
// Rotation = append the new PEM here, ship a release signed with the OLD key
// (so fielded devices learn the new anchor), then re-key CI; remove the old
// entry a release later. See docs/ota.md.
static const char* const kOtaPubKeys[] = {
    // key #1 - generated 2026-07-18 (CI: OTA_SIGNING_KEY)
    R"PEM(-----BEGIN PUBLIC KEY-----
MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAE0wQ36qVOmNUDbXurABy3gzEzdm0D
iOMoR65DVT987FCCnVg7PLKpYY5ml7HZjBckputCTSGrQ2SEl8gGRBq/JQ==
-----END PUBLIC KEY-----
)PEM",
#ifdef NIMBUS_TEST
    // TEST-ONLY anchor (private half is COMMITTED at test/ota_test_key.pem so
    // the HIL suite can sign fixtures). Production esp32s3 builds never trust
    // it - and never flash env:test on a production unit (AGENTS.md).
    R"PEM(-----BEGIN PUBLIC KEY-----
MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAElr8hokxcrZ+Zqs4POi6nZRTRgDMr
knXVM5BhR8UJf3f4RmGzYb3BNtZeMhtR6EukEH4YagwspSLWIpM29OmZBQ==
-----END PUBLIC KEY-----
)PEM",
#endif
};
static const int kOtaPubKeyCount = sizeof(kOtaPubKeys) / sizeof(kOtaPubKeys[0]);
