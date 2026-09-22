// An enabled, keyed connector whose provider is NOT the assistant's current head
// is reachable through a spawned sub-session on that provider (core rule:
// connectorScope -> SubsessionsOnly, orch_connectors_wire.cpp). The Connectors-tab
// badge must say so for EVERY provider, including Mistral, which previously read a
// misleading "idle - set host to Mistral" and pushed the owner to switch head for
// no reason. This locks the badge to the core capability model and to its OpenAI
// sibling, and keeps the cross-surface story consistent with the Capabilities table.
import { test, expect } from '@playwright/test';
import { seedToken, openApp } from './_helpers.mjs';

const KNOWN = [{ id: 'gcal', name: 'Google Calendar', providers: 'openai,mistral', kind: 'connector', cid: '', cred: 'Google', desc: 'Calendar.', docs: '' }];

function connectors(host, keyed) {
  return {
    configured: [{ type: 'gcal', name: 'Google Calendar', prov: 'mistral', kind: 'connector', en: 1 }],
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

test('off-host Mistral connector reads "via sessions", never "idle"', async ({ page }) => {
  await seedToken(page);
  await openConnectorsWith(page, connectors('anthropic', { mistral: true, anthropic: true }));
  const card = page.locator('#conncards', { hasText: 'Google Calendar' });
  await expect(card).toContainText('via sessions');
  await expect(card).not.toContainText('idle');
  await expect(card).not.toContainText('set host to Mistral');
});

test('on-host Mistral connector reads "live on your turns"', async ({ page }) => {
  await seedToken(page);
  await openConnectorsWith(page, connectors('mistral', { mistral: true }));
  const card = page.locator('#conncards', { hasText: 'Google Calendar' });
  await expect(card).toContainText('live on your turns');
});

test('Mistral connector with no Mistral key still reads "key missing", not "idle"', async ({ page }) => {
  await seedToken(page);
  await openConnectorsWith(page, connectors('anthropic', { anthropic: true }));
  const card = page.locator('#conncards', { hasText: 'Google Calendar' });
  await expect(card).toContainText('key missing');
  await expect(card).not.toContainText('idle');
});
