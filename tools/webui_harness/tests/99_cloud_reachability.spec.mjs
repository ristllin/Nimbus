// Cloud-access reachability line: the device UI must tell the honest truth about the
// tunnel (CUM-174, the device-UI half of the CUM-173 tunnel-502 field day).
//
// When the tunnel is DOWN, the device serves its web UI locally just fine, so the
// page renders - but the cloud card must say the tunnel is not connecting, NOT claim
// "Cloud access is on and reachable." A page that says cloud is up while a browser
// coming through d.cumulo-nimbus.ai gets a 502 is exactly the lie that day shipped.
// ui_js.h keys the line on d.cloud.online (paired && online -> reachable; paired &&
// !online -> not connecting), so this is fully simulable by stubbing /api/state.
//
// Reads the visible DOM, so it also runs against a real device over LAN
// (TARGET=device) unchanged.
import { test, expect } from '@playwright/test';
import { seedToken, openApp } from './_helpers.mjs';
import { STATE } from '../fixtures.mjs';

// Stub /api/state with a cloud object merged over the defaults. Must be routed
// before the app's first fetch (openApp navigates and loadState fires on load).
function cloudState(page, cloud) {
  return page.route('**/api/state', (r) =>
    r.fulfill({
      status: 200,
      contentType: 'application/json',
      body: JSON.stringify({ ...STATE, cloud }),
    }),
  );
}

// Open the Device pane and expand the Cloud access group so the line is on screen -
// a hidden group could hide a wrong render (AGENTS.md: a snapshot must show the state
// under test, not a collapsed group).
async function openCloudCard(page) {
  await openApp(page);
  await page.locator('.tab[data-p=device]').click();
  const group = page.locator('#pane-set > details.setgroup', {
    has: page.locator('> summary', { hasText: 'Cloud access' }),
  });
  if (!(await group.evaluate((el) => el.open))) await group.locator('> summary').click();
  await expect(group.locator('> .setbody'), 'the Cloud access body did not render').toBeVisible();
  return group;
}

test('paired but tunnel down: the line says it is not connecting, never "reachable"', async ({ page }) => {
  await seedToken(page);
  // Paired to the cloud, but the tunnel is not currently connecting (relay dropped,
  // server restart, the loopback-502 class - the device cannot reach out).
  await cloudState(page, { paired: true, online: false, optIn: true, line: '' });
  await openCloudCard(page);
  const line = page.locator('#cloudLine');
  await expect(line).toContainText('not connecting');
  // The honesty guard: it must NOT tell the owner cloud is reachable while it is down.
  await expect(line).not.toContainText('reachable');
});

test('paired and reachable: the line says cloud is on and reachable', async ({ page }) => {
  await seedToken(page);
  await cloudState(page, { paired: true, online: true, optIn: true, line: '' });
  await openCloudCard(page);
  await expect(page.locator('#cloudLine')).toContainText('on and reachable');
});

// The unpair/pair controls must track reachability sensibly: a paired device offers
// Unpair (not Pair) whether or not the tunnel is up, so the owner can always recover a
// stuck link. Pair is hidden once paired.
test('a paired-but-offline device still offers Unpair, not Pair', async ({ page }) => {
  await seedToken(page);
  await cloudState(page, { paired: true, online: false, optIn: true, line: '' });
  await openCloudCard(page);
  await expect(page.locator('#cloudUnpair')).toBeVisible();
  await expect(page.locator('#cloudPair')).toBeHidden();
});
