// Shared spec helpers for the Nimbus web-app suite.
import { expect } from '@playwright/test';

// Seed a token into localStorage BEFORE any script runs, so the auth gate does
// not block the app. On a real device run, pass NIMBUS_TOKEN; the harness token
// is accepted by the mock server (which ignores auth).
export async function seedToken(page, token) {
  const tok = token || process.env.NIMBUS_TOKEN || 'HARNESSTOKEN123456';
  await page.addInitScript((t) => {
    try { localStorage.setItem('nimbusTok', t); } catch (e) {}
  }, tok);
}

// Attach collectors for console errors and uncaught page errors. Returns an
// object with .errors (array of strings). Filter out benign favicon/network
// noise at the call site if needed.
export function collectErrors(page) {
  const errors = [];
  page.on('console', (msg) => { if (msg.type() === 'error') errors.push(`console: ${msg.text()}`); });
  page.on('pageerror', (err) => { errors.push(`pageerror: ${err.message}`); });
  return { errors };
}

// Navigate to the app and wait for the shell to be interactive.
export async function openApp(page) {
  await page.goto('/');
  await expect(page.locator('nav.tabs')).toBeVisible();
  // The auth gate, if present, would cover everything - assert it is absent.
  await expect(page.locator('#authgate')).toHaveCount(0);
}

// A content element distinctive to each top destination's pane. It is present in the
// static HTML but only VISIBLE once goDest() un-hides that pane - so asserting it
// visible proves the RIGHT pane actually painted. This is the guard against the
// "capture never inspected" class (AGENTS.md): a screenshot of the wrong pane - the
// white-screen symptom, where dead page JS leaves Home showing under a highlighted
// tab - must FAIL here instead of being archived as if it were correct.
export const PANE_ANCHOR = {
  home: '#pane-dash h2:has-text("Active sessions")',
  chat: '#chatInput',
  memory: '#pane-mem',
  assistant: '.subtab',            // subtabs exist only on the Assistant pane
  device: '#fwsec',                // the Software update group, Device-only
};

// Assert the given top destination's pane is the one on screen. Fails if the target
// pane never rendered, or if Home is still showing under a non-home tab (dead JS).
export async function assertPane(page, dest) {
  const sel = PANE_ANCHOR[dest];
  if (!sel) throw new Error(`no pane anchor for destination "${dest}"`);
  await expect(page.locator(sel).first(), `${dest} pane did not render`).toBeVisible();
  if (dest !== 'home') {
    await expect(page.locator('#pane-dash'), `${dest} switch left Home visible`).toBeHidden();
  }
}

// Open the global search palette the way the current viewport allows: the sidebar
// button on desktop, the "/" shortcut on phone (where the sidebar - and its search
// button - are replaced by the bottom tab bar and the button is display:none).
export async function openSearch(page) {
  const btn = page.locator('#globalSearchBtn');
  if (await btn.isVisible()) await btn.click();
  else await page.keyboard.press('/');
  await expect(page.locator('#searchOverlay')).toBeVisible();
}

// Drive the app's styled modal (CUM-266 replaced native confirm/prompt/alert). For a
// type-to-confirm prompt, pass the exact word; OK stays disabled until it matches.
export async function confirmModal(page, word) {
  await expect(page.locator('#modalOv.show')).toBeVisible();
  if (word !== undefined) await page.locator('#modalInput').fill(word);
  await page.locator('#modalOk').click();
}

// Assert the styled modal is open, then Cancel it. (A type-to-confirm prompt's OK-gating
// is asserted inline by the caller before this - see 68_updates_storage's erase test.)
export async function dismissModal(page) {
  await expect(page.locator('#modalOv.show')).toBeVisible();
  await page.locator('#modalCancel').click();
}
