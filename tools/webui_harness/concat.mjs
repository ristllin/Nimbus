// concat.mjs - reconstruct the served web-app page host-side, exactly as the
// device's chunked "/" handler does: the ordered concatenation of the PROGMEM
// raw-string fragments listed in include/web_pages.h. Mirrors the logic in
// tools/webui_concat_check.py so the harness always tests the CURRENT source,
// not a stale snapshot.
//
// Lane N1 (Web app revamp). Owns include/web/* and the webui snapshot; this
// host harness lives beside the snapshot it exercises.
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const HERE = dirname(fileURLToPath(import.meta.url));
export const ROOT = join(HERE, '..', '..');            // repo root (nimbus/)
const WEB = join(ROOT, 'include', 'web');
const MANIFEST = join(ROOT, 'include', 'web_pages.h');

const RAWSTR = /R"=====\(([\s\S]*)\)====="/;

function fragmentPayload(name) {
  const txt = readFileSync(join(WEB, `${name}.h`), 'utf8');
  const m = RAWSTR.exec(txt);
  if (!m) throw new Error(`no raw-string payload in ${name}.h`);
  return m[1];
}

// Fragment include order, taken from web_pages.h (single source of truth).
export function fragmentOrder() {
  const manifest = readFileSync(MANIFEST, 'utf8');
  const order = [...manifest.matchAll(/#include "web\/(ui_\w+)\.h"/g)].map((m) => m[1]);
  if (!order.length) throw new Error('no fragment includes in web_pages.h');
  return order;
}

// The full page, byte-identical to what the device serves.
export function buildPage() {
  return fragmentOrder().map(fragmentPayload).join('');
}

// The dotted-ring logo served at GET /logo.svg (a C string literal, not a raw
// string) - unescape the \" so the browser gets valid SVG.
export function logoSvg() {
  const txt = readFileSync(join(WEB, 'ui_logo.h'), 'utf8');
  const m = /UI_LOGO_SVG\[\]\s*PROGMEM\s*=\s*"([\s\S]*?)";/.exec(txt);
  if (!m) throw new Error('no UI_LOGO_SVG literal in ui_logo.h');
  return m[1].replace(/\\"/g, '"').replace(/\\n/g, '\n');
}

if (process.argv[1] === fileURLToPath(import.meta.url)) {
  process.stdout.write(buildPage());
}
