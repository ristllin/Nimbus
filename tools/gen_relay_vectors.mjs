// Generate cloud relay protocol test vectors from the single source of truth:
// cumulo-nimbus/packages/shared/src/protocol.ts. Emits test/test_relay_codec/
// relay_vectors.h, consumed by the native Unity suite. Mirrors the discipline of
// tools/gen_nsn_vectors.py: import the reference impl, fail loudly on drift.
//
// Run whenever protocol.ts changes (needs the cumulo repo's node_modules for zod/tsx):
//   cd ../cumulo-nimbus && npx tsx ../Nimbus/tools/gen_relay_vectors.mjs
//
// Two directions, both checked here:
//   RELAY -> DEVICE: encodeFrame() output the C++ parser must decode (the byte-locked
//                    artifact written to the .h).
//   DEVICE -> RELAY: the firmware's intended hello/res/ping shapes are run through the
//                    reference parseDeviceFrame() (zod). If zod rejects, generation
//                    THROWS, so a wire drift can never ship silently.
import { fileURLToPath } from "node:url";
import { dirname, resolve } from "node:path";
import { existsSync } from "node:fs";
import { writeFileSync } from "node:fs";

const here = dirname(fileURLToPath(import.meta.url));

// Resolve the single source of truth robustly across layouts: the normal sibling
// checkout (cumulo-nimbus), a fleet-lane worktree (the cloud repo checked out as
// `cumulo`), or an explicit CUMULO_PROTOCOL override. Prefer the built dist (plain
// ESM, no tsx needed); fall back to the .ts source when a TS loader is present.
function resolveProto() {
  if (process.env.CUMULO_PROTOCOL) return process.env.CUMULO_PROTOCOL;
  const roots = ["../../cumulo-nimbus", "../../cumulo", "../cumulo", "../cumulo-nimbus"];
  const rels = ["packages/shared/dist/protocol.js", "packages/shared/src/protocol.ts"];
  for (const root of roots) {
    for (const rel of rels) {
      const p = resolve(here, root, rel);
      if (existsSync(p)) return p;
    }
  }
  throw new Error("could not find cumulo protocol module (set CUMULO_PROTOCOL)");
}
const protoPath = resolveProto();
const proto = await import(protoPath);
const { encodeFrame, parseDeviceFrame, PROTOCOL_VERSION } = proto;

// --- RELAY -> DEVICE vectors (the C++ parser must decode these exactly) --------
const relayVectors = [
  { name: "welcome", frame: { t: "welcome", v: PROTOCOL_VERSION, heartbeatMs: 30000 } },
  {
    name: "req_get_no_body",
    frame: {
      t: "req", id: "r1", method: "GET", path: "/api/state?t=abc",
      headers: { "user-agent": "Mozilla/5.0", accept: "application/json" },
    },
  },
  {
    name: "req_post_body",
    frame: {
      t: "req", id: "r2", method: "POST", path: "/api/config",
      headers: { "content-type": "application/json" },
      bodyB64: Buffer.from('{"devName":"Nimbus"}').toString("base64"),
    },
  },
  { name: "pong", frame: { t: "pong", ts: 1786900000000 } },
  { name: "bye_unpaired", frame: { t: "bye", reason: "unpaired" } },
  { name: "bye_revoked", frame: { t: "bye", reason: "revoked" } },
  // Chunked upload (protocol 2).
  {
    name: "ubegin",
    frame: {
      t: "ubegin", id: "u1", method: "POST", path: "/api/music/upload?name=song.mp3",
      headers: { "content-type": "multipart/form-data; boundary=X" }, totalLen: 5000000,
    },
  },
  {
    name: "uchunk",
    frame: {
      t: "uchunk", id: "u1", seq: 0, off: 0,
      bodyB64: Buffer.from("hello chunk").toString("base64"),
    },
  },
  { name: "uend", frame: { t: "uend", id: "u1" } },
  { name: "uabort", frame: { t: "uabort", id: "u1", reason: "client_abort" } },
];

// --- DEVICE -> RELAY shapes (validated through zod at generation time) ----------
const deviceFrames = [
  { t: "hello", v: PROTOCOL_VERSION, deviceId: "dev_abc123", connectToken: "tok.xyz", fw: "v4.2.0" },
  {
    t: "res", id: "r1", status: 200,
    headers: { "content-type": "application/json" },
    bodyB64: Buffer.from('{"ok":true}').toString("base64"),
  },
  { t: "res", id: "r3", status: 204, headers: {} },
  { t: "ping", ts: 1786900000001 },
  { t: "uack", id: "u1", off: 8192 },
];
for (const f of deviceFrames) {
  const r = parseDeviceFrame(encodeFrame(f));
  if (!r.ok) throw new Error(`device->relay drift: ${f.t} rejected by parseDeviceFrame: ${r.error}`);
}

// --- emit the header -----------------------------------------------------------
const TYPE = { welcome: 1, req: 2, pong: 3, bye: 4, ubegin: 5, uchunk: 6, uend: 7, uabort: 8 };
const esc = (s) => s.replace(/\\/g, "\\\\").replace(/"/g, '\\"');
const cstr = (s) => (s === undefined || s === null ? "nullptr" : `"${esc(String(s))}"`);

let rows = "";
for (const v of relayVectors) {
  const f = v.frame;
  const json = encodeFrame(f);
  const hdrKey = f.headers ? Object.keys(f.headers)[0] : null;
  const hdrVal = hdrKey ? f.headers[hdrKey] : null;
  // `reason` covers both bye and uabort. totalLen/seq/off are upload-only (0 otherwise).
  rows +=
    `  {${cstr(v.name)}, ${cstr(json)}, ${TYPE[f.t]}, ${f.heartbeatMs ?? 0}, ` +
    `${cstr(f.id)}, ${cstr(f.method)}, ${cstr(f.path)}, ${cstr(f.bodyB64)}, ` +
    `${cstr(f.reason)}, ${cstr(hdrKey)}, ${cstr(hdrVal)}, ` +
    `${f.totalLen ?? 0}, ${f.seq ?? 0}, ${f.off ?? 0}},\n`;
}

const header = `// GENERATED by tools/gen_relay_vectors.mjs from cumulo-nimbus protocol.ts.
// Do NOT hand-edit. Regenerate on any protocol.ts change. PROTOCOL_VERSION=${PROTOCOL_VERSION}.
#pragma once

struct RelayVec {
  const char* name;
  const char* json;
  int type;  // 1=welcome 2=req 3=pong 4=bye 5=ubegin 6=uchunk 7=uend 8=uabort
  unsigned heartbeatMs;
  const char* id;
  const char* method;
  const char* path;
  const char* bodyB64;
  const char* byeReason;  // bye + uabort reason
  const char* hdrKey;
  const char* hdrVal;
  unsigned totalLen;  // ubegin
  unsigned seq;       // uchunk
  unsigned off;       // uchunk
};

static const RelayVec kRelayVectors[] = {
${rows}};
static const int kRelayVectorCount = ${relayVectors.length};
`;

const out = resolve(here, "../test/test_relay_codec/relay_vectors.h");
writeFileSync(out, header);
console.log(`wrote ${out} (${relayVectors.length} relay->device vectors; ${deviceFrames.length} device->relay shapes validated)`);
