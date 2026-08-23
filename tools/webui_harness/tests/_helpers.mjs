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
