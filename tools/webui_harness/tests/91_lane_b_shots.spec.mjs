// Visual evidence for the web page batch (the Code sandbox switch and its status
// line, the connector cards, the settings-store banner, the Health rows, the
// history pane edges, the Telegram archive view), at desktop and phone widths.
// Against the host server it fills each surface from fixtures; with TARGET=device
// (BASE_URL=http://<device>, NIMBUS_TOKEN set) it shoots the real device page as it
// is. Screenshots land in screenshots/ (not committed).
import { test, expect } from '@playwright/test';
import { seedToken, openApp } from './_helpers.mjs';
import { STATE, ORCH } from '../fixtures.mjs';

const ON_DEVICE = process.env.TARGET === 'device';
const shot = (page, testInfo, name) =>
  page.screenshot({ path: `screenshots/laneB-${name}-${testInfo.project.name}.png`, fullPage: true });

async function fixtures(page) {
  if (ON_DEVICE) return;
  const json = (body) => (r) => r.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify(body) });
  await page.route('**/api/state', json({ ...STATE, nvs: { ok: true, used: 20160, free: 0, total: 20160, low: true, fails: 266, lastFail: 'usgLedger', recentFails: [] }, nvsReserve: { entries: 16, refused: 12, evicted: 3, starved: true } }));
  await page.route('**/api/health', json({ ok: 9, degraded: 3, absent: 1, components: [
    { key: 'wifi', label: 'Wi-Fi', state: 'ok', detail: 'connected -58 dBm' },
    { key: 'nvs', label: 'Settings storage', state: 'degraded', detail: "266 saves failed since start, last usgLedger, 0 entries free (under the reserve of 16); the reserve cannot be restored. Remove connectors you don't use to free space." },
    { key: 'provider_openai', label: 'Provider openai', state: 'degraded', detail: 'out of credit (HTTP 429) for 90 min; turns skip it until it answers again' },
    { key: 'provider_anthropic', label: 'Provider anthropic', state: 'degraded', detail: 'out of credit (HTTP 400) for 3 h; turns skip it until it answers again' }] }));
  await page.route('**/api/orch', json({ ...ORCH, codeSbx: 1, pdfRoute: 'openai' }));
  await page.route('**/api/connectors', json({
    configured: [{ name: 'gcal', type: 'gcal', prov: 'mistral', kind: 'connector', en: 1, auth: 2, ws: 'unchecked' },
                 { name: 'notion', type: 'notion', prov: 'mistral', kind: 'connector', en: 1, auth: 2, ws: 'unlisted' }],
    known: [{ id: 'gcal', name: 'Google Calendar', providers: 'openai,mistral', kind: 'connector', cid: 'connector_googlecalendar', mcid: 'google_calendar', cred: 'OAuth', desc: 'Read and manage calendar events.', docs: '' },
            { id: 'notion', name: 'Notion', providers: 'mistral,openai,anthropic', kind: 'mcp', cid: '', mcid: 'notion', cred: 'Token', desc: 'Pages and databases.', docs: '' }],
    keyed: { mistral: true, openai: true }, host: 'openai', wsChecking: false,
    wsMiss: 'memory', wsMissAgeS: 60, wsRecheckS: 1500, mistralTools: 4, mistralToolsAdvised: 2 }));
  await page.route('**/api/mem/episodic**', json({ messages: [
    { id: 'm2', session: 'web', kind: 'message', role: 'assistant', text: 'Here is the summary you asked for.', tags: '' },
    { id: 'm1', session: 'web', kind: 'message', role: 'user', text: 'Summarize this morning.', tags: '' }],
    total: 1234, indexComplete: false, olderExists: true, nextBefore: 'm00000100', searchedTo: '2026-10-19' }));
  await page.route('**/api/tg/archive**', (r) => {
    const u = new URL(r.request().url());
    if (!u.searchParams.get('chat')) return json({ stats: {}, chats: [{ chat: '123456789', days: 2, newest: '2026-10-21' }] })(r);
    if (!u.searchParams.get('day')) return json({ chat: '123456789', days: ['2026-10-21', '2026-10-20'], more: false })(r);
    return json({ chat: '123456789', day: '2026-10-21', rows: [
      { dir: 'in', date: 1792990800, kind: 'photo', raw: '1001.json', file: '1001.jpg' },
      { dir: 'in', date: 1792990860, kind: 'text', raw: '1002.json' },
      { dir: 'out', date: 1792990900, kind: 'document', raw: 'o3.json', file: 'o3.pdf' }], more: false })(r);
  });
}

test('Home: settings-store banner and the Health rows', async ({ page }, testInfo) => {
  await seedToken(page);
  await fixtures(page);
  await openApp(page);
  if (!ON_DEVICE) await expect(page.locator('#nvsBanner')).toBeVisible();
  await page.waitForTimeout(800);
  await shot(page, testInfo, 'home');
});

test('Tools: the one Code sandbox switch and its status line', async ({ page }, testInfo) => {
  await seedToken(page);
  await fixtures(page);
  await openApp(page);
  await page.locator('.tab[data-p=assistant]').click();
  await page.locator('.subtab[data-sp=tools]').click();
  const grp = page.locator('#subpane-tools details.setgroup', { hasText: 'Tool use' }).first();
  if (!(await grp.evaluate((d) => d.open))) await grp.locator('summary').click();
  await page.locator('#codeSbxRow button.qh').click();
  if (!ON_DEVICE) await expect(page.locator('#codeSbxStat')).toHaveText('Ready. PDFs go through OpenAI.');
  await page.waitForTimeout(400);
  await shot(page, testInfo, 'tools');
});

test('Connectors: Mistral cards say why they are unconfirmed, with Check Now', async ({ page }, testInfo) => {
  await seedToken(page);
  await fixtures(page);
  await openApp(page);
  await page.locator('.tab[data-p=assistant]').click();
  await page.locator('.subtab[data-sp=connectors]').click();
  if (!ON_DEVICE) await expect(page.locator('#cc_gcal_ws')).toContainText('Check Now');
  await page.waitForTimeout(400);
  await shot(page, testInfo, 'connectors');
});

test('Chat: the indexing divider and Load Older', async ({ page }, testInfo) => {
  await seedToken(page);
  await fixtures(page);
  await openApp(page);
  await page.locator('.tab[data-p=chat]').click();
  if (!ON_DEVICE) await expect(page.locator('#chatIndexing')).toBeVisible();
  await page.waitForTimeout(400);
  await shot(page, testInfo, 'chat');
});

test('Memory: the Telegram archive view', async ({ page }, testInfo) => {
  await seedToken(page);
  await fixtures(page);
  await openApp(page);
  await page.locator('.tab[data-p=memory]').click();
  await page.locator('#tgArcGroup summary').click();
  if (!ON_DEVICE) await expect(page.locator('.tgArcRow')).toHaveCount(3);
  await page.waitForTimeout(600);
  await page.locator('#tgArcGroup').scrollIntoViewIfNeeded();
  await shot(page, testInfo, 'archive');
});
