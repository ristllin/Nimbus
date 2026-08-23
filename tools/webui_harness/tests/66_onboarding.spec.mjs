// CUM-66: onboarding v2 - display flip step, provider step gains Cumulo Nimbus +
// Z.ai, e-ink branches removed, finishes into Home with a "what next" card.
import { test, expect } from '@playwright/test';
import { seedToken } from './_helpers.mjs';
import { STATE } from '../fixtures.mjs';

// Serve /api/state with needsOnboarding so the wizard opens.
async function onboardingState(page, extra = {}) {
  await page.route('**/api/state', (route) =>
    route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify({ ...STATE, needsOnboarding: true, ...extra }) }));
}

test('the wizard opens on a fresh device and has no e-ink strings', async ({ page }) => {
  await seedToken(page);
  await onboardingState(page);
  await page.goto('/');
  await expect(page.locator('#onbov')).toBeVisible();
  const overlay = await page.locator('#onbov').innerText();
  expect(overlay).not.toMatch(/e-?ink/i);
  expect(overlay).not.toMatch(/knob/i);
});

test('the display step is a flip step, not an e-ink/touch chooser', async ({ page }) => {
  await seedToken(page);
  await onboardingState(page);
  await page.goto('/');
  await page.locator('#onbNext').click(); // welcome -> display
  await expect(page.locator('#onb_flip')).toBeVisible();
  const body = await page.locator('#onbBody').innerText();
  expect(body).toMatch(/flip/i);
  expect(body).not.toMatch(/e-?ink/i);
});

test('the provider step offers Cumulo Nimbus and Z.ai', async ({ page }) => {
  await seedToken(page);
  await onboardingState(page);
  await page.goto('/?onboard=provider'); // handoff deep-links straight to the provider step
  await expect(page.locator('#onb_prov')).toBeVisible();
  const opts = await page.locator('#onb_prov option').allInnerTexts();
  expect(opts).toContain('Cumulo Nimbus');
  expect(opts).toContain('Z.ai');
  expect(opts).toContain('Anthropic');
});

test('Home shows a "what next" card right after onboarding finishes', async ({ page }) => {
  await seedToken(page);
  await page.goto('/');
  await expect(page.locator('#whatNext')).toBeHidden(); // no flag yet
  // Simulate the one-shot flag finish() sets, then reload as the wizard does.
  await page.evaluate(() => localStorage.setItem('nimbusJustOnboarded', '1'));
  await page.reload();
  await expect(page.locator('#whatNext')).toBeVisible();
  await expect(page.locator('#whatNext')).toContainText('What next');
  // Dismiss hides it, and it does not return on reload (the flag was cleared once).
  await page.locator('#whatNextDismiss').click();
  await expect(page.locator('#whatNext')).toBeHidden();
  await page.reload();
  await expect(page.locator('#whatNext')).toBeHidden();
});
