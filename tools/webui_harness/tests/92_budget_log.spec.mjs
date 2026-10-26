// The limit changes made with /budget (lane BUD, owner request 26 Oct 2026): the Usage
// page lists the newest three under the spending ceiling, with when and from where; the
// device's text is escaped; nothing shows while there are none; a change whose time the
// device did not know says so.
import { test, expect } from '@playwright/test';
import { seedToken, openApp } from './_helpers.mjs';
import { ORCH, CEILING } from '../fixtures.mjs';

async function openUsage(page, changes) {
  const body = { ...ORCH, usage: { ...ORCH.usage, ceiling: { ...CEILING, changes } } };
  await page.route('**/api/orch', (route) =>
    route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify(body) }));
  await seedToken(page);
  await openApp(page);
  await page.locator('.tab[data-p=assistant]').click();
  await page.locator('.subtab[data-sp=usage]').click();
  await expect(page.locator('#ceilMeter')).toContainText('this month');
}

test('the newest three changes by command are listed with when and from where', async ({ page }) => {
  await openUsage(page, [
    { t: 1793124120, src: 'telegram', text: 'Anthropic monthly limit changed from $250.00 to $400.00' },
    { t: 0, src: 'web', text: 'Overall ceiling changed from off to $30.00' },
    { t: 1793120000, src: 'serial', text: 'OpenAI monthly limit of $75.00 removed' },
    { t: 1793110000, src: 'telegram', text: 'the fourth, never shown' },
  ]);
  const log = page.locator('#budLog');
  await expect(log).toContainText('Changed by command:');
  await expect(log).toContainText('(Telegram): Anthropic monthly limit changed from $250.00 to $400.00');
  await expect(log).toContainText('Time not known (web chat): Overall ceiling changed from off to $30.00');
  await expect(log).toContainText('(serial console): OpenAI monthly limit of $75.00 removed');
  await expect(log).not.toContainText('the fourth');
});

test('the device text is shown as text, never as markup', async ({ page }) => {
  await openUsage(page, [{ t: 0, src: 'telegram', text: '<img src=x onerror=alert(1)>' }]);
  await expect(page.locator('#budLog')).toContainText('<img src=x onerror=alert(1)>');
  await expect(page.locator('#budLog img')).toHaveCount(0);
});

test('nothing is listed while there are no changes', async ({ page }) => {
  await openUsage(page, []);
  await expect(page.locator('#budLog')).toBeEmpty();
});
