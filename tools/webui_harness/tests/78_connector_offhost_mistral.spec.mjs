// An enabled, keyed Mistral Studio connector authenticates in the owner's Mistral
// account, not on the device. The Connectors-tab badge must tell the honest story:
//   - not connected in Mistral yet (auth==2)        -> "connect it in Mistral"
//   - connected, and Mistral is NOT the head (auth 1) -> "via sessions" (spawned sub)
//   - connected, and Mistral IS the head (auth 1)     -> "live on your turns"
// It must never read the old misleading "idle - set host to Mistral", and never imply
// readiness ("via sessions") for a connector the owner has not connected in Mistral.
import { test, expect } from '@playwright/test';
import { seedToken, openApp } from './_helpers.mjs';

const KNOWN = [{ id: 'gcal', name: 'Google Calendar', providers: 'openai,mistral', kind: 'connector', cid: '', cred: 'Google', desc: 'Calendar.', docs: '' }];

// auth: -1 n/a, 1 present/connected, 0 sign-in failed, 2 missing / not connected in Mistral.
function connectors(host, keyed, auth) {
  return {
    configured: [{ type: 'gcal', name: 'Google Calendar', prov: 'mistral', kind: 'connector', en: 1, auth }],
    known: KNOWN, keyed, host,
  };
}

async function openConnectorsWith(page, payload) {
  await page.route('**/api/connectors**', (r) =>
    r.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify(payload) }));
  await openApp(page);
  await page.locator('.tab[data-p=assistant]').click();
  await page.locator('.subtab[data-sp=connectors]').click();
  await expect(page.locator('#conncards')).toBeVisible();
}

test('not connected in Mistral (auth=2) reads "connect it in Mistral", never "via sessions" or "idle"', async ({ page }) => {
  await seedToken(page);
  await openConnectorsWith(page, connectors('anthropic', { mistral: true, anthropic: true }, 2));
  const card = page.locator('#conncards', { hasText: 'Google Calendar' });
  await expect(card).toContainText('connect it in Mistral');
  await expect(card).not.toContainText('via sessions');
  await expect(card).not.toContainText('idle');
  await expect(card).not.toContainText('set host to Mistral');
});

test('connected off-host (auth=1) reads "via sessions"', async ({ page }) => {
  await seedToken(page);
  await openConnectorsWith(page, connectors('anthropic', { mistral: true, anthropic: true }, 1));
  const card = page.locator('#conncards', { hasText: 'Google Calendar' });
  await expect(card).toContainText('via sessions');
  await expect(card).not.toContainText('idle');
});

test('connected on-host (auth=1) reads "live on your turns"', async ({ page }) => {
  await seedToken(page);
  await openConnectorsWith(page, connectors('mistral', { mistral: true }, 1));
  const card = page.locator('#conncards', { hasText: 'Google Calendar' });
  await expect(card).toContainText('live on your turns');
});

test('no Mistral key still reads "key missing", not "idle"', async ({ page }) => {
  await seedToken(page);
  await openConnectorsWith(page, connectors('anthropic', { anthropic: true }, 2));
  const card = page.locator('#conncards', { hasText: 'Google Calendar' });
  await expect(card).toContainText('key missing');
  await expect(card).not.toContainText('idle');
});
