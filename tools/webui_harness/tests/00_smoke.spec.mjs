// Smoke: the page loads clean, the shell renders, and switching every nav
// destination throws no JS errors. This is the floor every other spec builds on.
import { test, expect } from '@playwright/test';
import { seedToken, collectErrors, openApp } from './_helpers.mjs';

test('page loads with no console/page errors and shows the nav', async ({ page }) => {
  await seedToken(page);
  const { errors } = collectErrors(page);
  await openApp(page);
  // Let the initial polls (state/orch/health) settle.
  await page.waitForTimeout(800);
  const nav = page.locator('nav.tabs .tab');
  expect(await nav.count()).toBeGreaterThanOrEqual(5);
  expect(errors, `unexpected JS errors:\n${errors.join('\n')}`).toEqual([]);
});

test('every nav destination switches its pane with no errors', async ({ page }) => {
  await seedToken(page);
  const { errors } = collectErrors(page);
  await openApp(page);
  const tabs = page.locator('nav.tabs .tab');
  const n = await tabs.count();
  for (let i = 0; i < n; i++) {
    await tabs.nth(i).click();
    await page.waitForTimeout(150);
    const pane = tabs.nth(i);
    await expect(pane).toHaveClass(/on/);
  }
  expect(errors, `errors while switching panes:\n${errors.join('\n')}`).toEqual([]);
});
