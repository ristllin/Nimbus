// CUM-31: the feedback-state system. One helper (run/fbState) drives every async
// action through pending -> result (ok | none | error); nothing disappears, and an
// error names the next step. This spec pins the helper's state machine and proves
// a converted real action (file upload) renders pending then a result state.
import { test, expect } from '@playwright/test';
import { seedToken, openApp } from './_helpers.mjs';

test('fbState/run render pending then ok, none, and error', async ({ page }) => {
  await seedToken(page);
  await openApp(page);

  // Success path: pending -> ok.
  const okState = await page.evaluate(async () => {
    const el = document.createElement('div'); el.id = '__fb_ok'; document.body.appendChild(el);
    const p = run({ status: el, work: () => new Promise((r) => setTimeout(() => r(1), 30)), ok: () => 'Saved.' });
    const during = el.getAttribute('data-fb');
    await p;
    return { during, after: el.getAttribute('data-fb'), text: el.textContent };
  });
  expect(okState.during).toBe('pending');
  expect(okState.after).toBe('ok');
  expect(okState.text).toContain('Saved.');

  // Empty path: work resolves but ok() flags {none:true}.
  const noneState = await page.evaluate(async () => {
    const el = document.createElement('div'); document.body.appendChild(el);
    await run({ status: el, work: () => Promise.resolve([]), ok: (r) => (r.length ? 'Found' : { none: true, msg: 'Nothing found.' }) });
    return { fb: el.getAttribute('data-fb'), text: el.textContent };
  });
  expect(noneState.fb).toBe('none');
  expect(noneState.text).toContain('Nothing found.');

  // Error path: work rejects -> error state whose message names a next step.
  const errState = await page.evaluate(async () => {
    const el = document.createElement('div'); document.body.appendChild(el);
    try { await run({ status: el, work: () => Promise.reject(500), error: () => 'Could not save - try again.' }); } catch (e) {}
    return { fb: el.getAttribute('data-fb'), text: el.textContent };
  });
  expect(errState.fb).toBe('error');
  expect(errState.text).toMatch(/try again/i);
});

test('the button is disabled during pending and re-enabled after', async ({ page }) => {
  await seedToken(page);
  await openApp(page);
  const res = await page.evaluate(async () => {
    const btn = document.createElement('button'); document.body.appendChild(btn);
    const el = document.createElement('div'); document.body.appendChild(el);
    const p = run({ status: el, btn, work: () => new Promise((r) => setTimeout(r, 30)), ok: () => 'ok' });
    const during = btn.disabled;
    await p;
    return { during, after: btn.disabled };
  });
  expect(res.during).toBe(true);
  expect(res.after).toBe(false);
});

test('file upload (converted action) shows pending then a success result', async ({ page }) => {
  await seedToken(page);
  await openApp(page);
  await page.locator('nav.tabs .tab[data-p=memory]').click();
  await page.locator('#pane-mem details summary', { hasText: 'Files' }).click();
  await page.locator('#upFile').setInputFiles({ name: 'hello.txt', mimeType: 'text/plain', buffer: Buffer.from('hi') });
  await page.locator('#upBtn').click();
  const msg = page.locator('#upMsg');
  await expect(msg).toHaveAttribute('data-fb', 'ok');
  await expect(msg).toContainText('Uploaded');
});

test('file upload error names the next step', async ({ page }) => {
  await seedToken(page);
  await page.route('**/api/files/upload**', (route) => route.fulfill({ status: 500, body: 'err' }));
  await openApp(page);
  await page.locator('nav.tabs .tab[data-p=memory]').click();
  await page.locator('#pane-mem details summary', { hasText: 'Files' }).click();
  await page.locator('#upFile').setInputFiles({ name: 'bad.txt', mimeType: 'text/plain', buffer: Buffer.from('x') });
  await page.locator('#upBtn').click();
  const msg = page.locator('#upMsg');
  await expect(msg).toHaveAttribute('data-fb', 'error');
  await expect(msg).toContainText('try again');
});
