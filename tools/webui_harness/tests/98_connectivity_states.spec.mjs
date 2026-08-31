// Connectivity-tab render matrix for the CUM-207 Part 2 redesign proposal. Captures
// every Connectivity + Cloud-access view/state into screenshots/ so the proposal can
// reference real renders (connected, disconnected/setup-AP, the saved-networks list,
// the setup-hotspot password panel, and the cloud/tunnel card). Regenerable QA
// artifacts, not committed. Run: npx playwright test 98_connectivity_states
import { test } from '@playwright/test';
import { seedToken, openApp } from './_helpers.mjs';

async function toDevice(page) {
  await seedToken(page);
  await openApp(page);
  await page.locator('.tab[data-p=device]').click();
  await page.waitForTimeout(400);
}

async function expand(page, name) {
  const group = page.locator('details.setgroup', {
    has: page.locator('summary', { hasText: name }),
  });
  const open = await group.evaluate((el) => el.open);
  if (!open) await group.locator('summary').click();
  await page.waitForTimeout(300);
  return group;
}

// The default fixtures: connected (sta=true, staIp set), setup hotspot up, three saved
// networks, cloud in the pairing state. This single render already exercises the reach
// table, the setup Wi-Fi password panel (CUM-200), and the saved-networks list.
test('connectivity connected', async ({ page }, testInfo) => {
  await toDevice(page);
  const group = await expand(page, 'Connectivity');
  const proj = testInfo.project.name;
  await group.screenshot({ path: `screenshots/${proj}-connectivity-connected.png` });
});

test('cloud access card', async ({ page }, testInfo) => {
  await toDevice(page);
  const group = await expand(page, 'Cloud access');
  const proj = testInfo.project.name;
  await group.screenshot({ path: `screenshots/${proj}-cloud-access.png` });
});

// Disconnected / recovery: no station link, the setup hotspot is the way in, and the
// saved list is empty (a fresh device). Overrides land before the page's first fetch.
test('connectivity disconnected setup-ap', async ({ page }, testInfo) => {
  await page.route('**/api/wifi**', (r) =>
    r.fulfill({
      contentType: 'application/json',
      body: JSON.stringify({
        max: 5, count: 0, networks: [], sta: false, staIp: '',
        apUp: true, apSsid: 'Nimbus-setup', apIp: '192.168.4.1',
      }),
    }),
  );
  await toDevice(page);
  const group = await expand(page, 'Connectivity');
  const proj = testInfo.project.name;
  await group.screenshot({ path: `screenshots/${proj}-connectivity-disconnected.png` });
});
