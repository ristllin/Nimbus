#!/usr/bin/env node
// migrate-docs.mjs - copy the repo's canonical `docs/**` prose into
// `website/docs/**`, adding Docusaurus front-matter and fixing links so they
// resolve inside the site. Idempotent: re-run it any time the source docs
// change (it overwrites the generated pages). The hand-written pages under
// `website/docs/intro.md` and `website/docs/getting-started/` are NOT touched.
//
//   node scripts/migrate-docs.mjs
//
// Sourced from ../docs so the site stays a *view* of the canonical docs; the
// originals are never moved or deleted. Each generated page carries a
// `custom_edit_url` pointing at its canonical source under docs/, so "Edit
// this page" edits the right file.

import {readFileSync, writeFileSync, mkdirSync, copyFileSync, existsSync} from 'node:fs';
import {dirname, resolve, basename, relative, sep} from 'node:path';
import {fileURLToPath} from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const repo = resolve(here, '..', '..'); // website/scripts -> repo root
const srcRoot = resolve(repo, 'docs');
const dstRoot = resolve(here, '..', 'docs');
// Local diagrams/screenshots are copied into the site's static dir and embedded,
// so they actually render on the published site (served at <baseUrl>/hw/<name>).
const imgRoot = resolve(here, '..', 'static', 'hw');

// source (relative to docs/)  ->  { out (relative to website/docs/), title, label }
// Only canonical published docs are listed. Maintainer-only docs and the folder
// READMEs stay in the repo and are not published - see docs/README.md for the
// docs-tree map.
const PAGES = [
  // Quick Start
  ['quick-start/what-you-need.md', 'quick-start/what-you-need.md', 'What you need', 'What you need'],
  ['quick-start/flash.md', 'quick-start/flash.md', 'Flash the firmware', 'Flash the firmware'],
  ['quick-start/setup-wizard.md', 'quick-start/setup-wizard.md', 'Set up the device', 'Set up the device'],
  ['quick-start/first-conversation.md', 'quick-start/first-conversation.md', 'Your first conversation', 'First conversation'],
  ['quick-start/notifier-quick-start.md', 'quick-start/notifier-quick-start.md', 'Notifier quick start', 'Notifier quick start'],
  // Hardware Build
  ['hardware.md', 'guides/hardware.md', 'Hardware reference', 'Hardware overview'],
  ['hardware/bom.md', 'guides/hardware-bom.md', 'Bill of materials', 'Bill of materials'],
  ['hardware/build-tft.md', 'guides/hardware-build-tft.md', 'Build Guide - Touch TFT', 'Build: Touch TFT'],
  ['hardware/touch-tft.md', 'guides/hardware-touch-tft.md', 'Configuration A - Touch TFT', 'HW: Touch TFT'],
  ['hardware/all-in-one-cyd.md', 'guides/hardware-all-in-one-cyd.md', 'Configuration B - all-in-one (Freenove CYD)', 'HW: All-in-one (CYD)'],
  // Using Nimbus
  ['modes-and-signals.md', 'guides/modes-and-signals.md', 'Modes & signals', 'Modes & signals'],
  ['led-ux.md', 'guides/led-ux.md', 'LED experience - motion & color', 'LED experience'],
  ['sfx-map.md', 'guides/sfx-map.md', 'SFX - sound state language', 'SFX map'],
  ['connectors.md', 'guides/connectors.md', 'Connectors - external tools per provider', 'Connectors'],
  ['people-and-privacy.md', 'guides/people-and-privacy.md', 'People and privacy - roles, quotas, what stays private', 'People & privacy'],
  ['notifier-status-language.md', 'guides/notifier-status-language.md', 'Notifier status language', 'Status language'],
  // How It Works
  ['architecture.md', 'guides/architecture.md', 'Nimbus Architecture', 'Architecture'],
  ['turn-anatomy.md', 'guides/turn-anatomy.md', 'Turn anatomy - what the model sees', 'Turn anatomy'],
  ['architecture/orchestrator-live-turn.md', 'guides/orchestrator-live-turn.md', 'Orchestrator live turn', 'Live turn'],
  ['orchestrator-world.md', 'guides/orchestrator-world.md', 'Orchestrator World - memory & control surface', 'Orchestrator World'],
  ['memory.md', 'guides/memory.md', 'Memory - RAM pools, turn budget, and compaction', 'Memory'],
  ['orchestrator-storage.md', 'guides/orchestrator-storage.md', 'Orchestrator storage tiering', 'Storage tiering'],
  ['sub-sessions.md', 'guides/sub-sessions.md', 'Sub-sessions - background agents end to end', 'Sub-sessions'],
  ['harness.md', 'guides/harness.md', 'The agentic harness', 'Harness'],
  ['provider-wire.md', 'guides/provider-wire.md', 'Provider wire - structured outputs & tool loops', 'Provider wire'],
  ['security.md', 'guides/security.md', 'Security posture & open TODOs', 'Security'],
  // Reference
  ['reference/config-and-nvs.md', 'reference/config-and-nvs.md', 'Config & NVS reference', 'Config & NVS'],
  ['reference/tool-catalog.md', 'reference/tool-catalog.md', 'Tool catalog', 'Tool catalog'],
  ['reference/turn-contract.md', 'reference/turn-contract.md', 'Turn contract', 'Turn contract'],
  ['reference/capabilities-matrix.md', 'reference/capabilities-matrix.md', 'Provider capability matrix', 'Capability matrix'],
  ['tools-and-commands.md', 'reference/tools-and-commands.md', 'Tools & Commands', 'Tools & commands'],
  ['changelog.md', 'reference/changelog.md', 'Changelog', 'Changelog'],
  // Forking & Contributing
  ['development.md', 'contributing/development.md', 'Development guide', 'Development'],
  ['self-hosted-ota.md', 'contributing/self-hosted-ota.md', 'Self-hosted OTA', 'Self-hosted OTA'],
  ['ota.md', 'guides/ota.md', 'OTA Firmware Updates', 'OTA updates'],
  ['ota-operations.md', 'guides/ota-operations.md', 'OTA Operations & Maintenance', 'OTA operations'],
];

// absolute source path -> output path (relative to website/docs/)
const srcToOut = new Map(PAGES.map(([src, out]) => [resolve(srcRoot, src), out]));

function toPosix(p) {
  return p.split(sep).join('/');
}

function transform(md, srcRel, outRel) {
  const srcDir = dirname(resolve(srcRoot, srcRel));
  const outDir = dirname(outRel);

  // 1) Image embeds -> copy the asset into the site's static dir and embed it, so
  //    the diagram actually renders. Fall back to a note if the file is missing.
  md = md.replace(/!\[([^\]]*)\]\(([^)]+)\)/g, (m, alt, url) => {
    if (/^https?:/.test(url)) return m; // remote image - leave it
    const abs = resolve(srcDir, url.trim());
    if (existsSync(abs)) {
      const base = basename(abs);
      mkdirSync(imgRoot, {recursive: true});
      copyFileSync(abs, resolve(imgRoot, base));
      return `![${alt}](/hw/${base})`;   // served at <baseUrl>/hw/<base>
    }
    const label = alt || url;
    return `\n:::note\nDiagram **${label}** lives in the source repository (\`${url}\`) - not embedded here.\n:::\n`;
  });

  // 2) Ordinary links: resolve each relative target against the PAGES map. A
  //    target that migrates gets a correct relative link to its published
  //    location; anything else (source files, internal docs, cross-repo paths)
  //    is de-linked - the words stay, the dead link goes.
  md = md.replace(/(?<!!)\[([^\]]+)\]\(([^)]+)\)/g, (m, text, url) => {
    const trimmed = url.trim();
    if (/^(https?:|mailto:|#)/.test(trimmed)) return m; // external / same-page anchor
    if (/\s/.test(trimmed)) return text; // not a real path (prose in parens)
    const match = trimmed.match(/^([^#?]*)([#?].*)?$/);
    const target = match[1];
    const suffix = match[2] || '';
    if (!target) return m;
    const abs = resolve(srcDir, target);
    const out = srcToOut.get(abs);
    if (!out) return text; // not published - drop the link, keep the words
    let rel = toPosix(relative(outDir === '.' ? '' : outDir, out));
    // Docusaurus only file-resolves EXPLICITLY relative .md links.
    if (!rel.startsWith('.')) rel = './' + rel;
    return `[${text}](${rel}${suffix})`;
  });

  return md;
}

function yamlEscape(s) {
  return s.replace(/"/g, '\\"');
}

let count = 0;
for (const [src, out, title, label] of PAGES) {
  const raw = readFileSync(resolve(srcRoot, src), 'utf8');
  const body = transform(raw, src, out);
  const front =
    `---\n` +
    `title: "${yamlEscape(title)}"\n` +
    `sidebar_label: "${yamlEscape(label)}"\n` +
    `description: "Migrated from docs/${src} - the canonical source lives in the repo."\n` +
    `custom_edit_url: "https://github.com/ristllin/Nimbus/blob/main/docs/${src}"\n` +
    `---\n\n` +
    `> Sourced from \`docs/${src}\` in the firmware repo.\n\n`;
  const dst = resolve(dstRoot, out);
  mkdirSync(dirname(dst), {recursive: true});
  writeFileSync(dst, front + body);
  count++;
  console.log(`  ${src}  ->  website/docs/${out}`);
}
console.log(`migrate-docs: wrote ${count} pages`);
