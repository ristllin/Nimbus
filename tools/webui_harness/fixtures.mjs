// fixtures.mjs - default JSON responses for the web-app's /api/* surface, good
// enough for the page to render and the happy-path flows to run. Individual
// Playwright specs override specific endpoints (empty / error / slow) with
// page.route() to assert feedback states. Shapes mirror what ui_js.h consumes.
//
// Contracts consumed from other lanes are STUBS here (TASK.md): /api/models +
// /api/fallbacks (N3), /api/ota/check + files payloads (N5), /api/wakeups (N2).

export const STATE = {
  fw: 'v4.3.0-pre', build: 'n1-harness', mode: 1, jobs: 2,
  heap: 120 * 1024, heapTotal: 300 * 1024, heapMin: 90 * 1024,
  psramFree: 6.2 * 1048576, psramTotal: 8 * 1048576,
  batt: {
    valid: true, percent: 82, millivolts: 8040, mvTrue: 8040,
    minsToEmpty: 240, onExtPower: false, charging: false, dieTempC: 41,
  },
  storeSD: true, sdLost: false,
  // Reach info the Home info line + connectivity render. rssi is the current station
  // signal the Wi-Fi panel badges on the in-use saved network (proposal §3).
  sta: true, rssi: -58,
  // The device /api/state emits the station address as `staIp` (webui.cpp), which the
  // Home info line renders ("sta <staIp> (<rssi> dBm)"). A prior fixture named it `ip`,
  // a field /api/state never sends, so the info line read "sta undefined" - a rendered
  // lie the audit must not show. Match the device contract.
  apSsid: 'Nimbus-setup', apIp: '192.168.4.1', staIp: '192.168.1.42',
  mdns: 'nimbus.local', running: true, scrModel: 'tft',
  // Battery mode: the device /api/state sends profile / effectiveProfile (ints) and
  // effectiveProfileName (webui.cpp). Omitting them left the Device pane reading
  // "effective: undefined" and no battery-mode radio checked. 1 = Balanced.
  profile: 1, effectiveProfile: 1, effectiveProfileName: 'Balanced',
  // A configured device reports a synced clock; without it the Usage spend chart and
  // routine schedules render their "clock not set yet" placeholder. Mirror the device.
  clock: { synced: true, epoch: Math.floor(Date.now() / 1000), local: '2026-09-01 12:00', tz: 'UTC' },
  // OTA (N5 contract stub): installed/latest/notes + result state. otaResult is
  // the definitive, poll-safe check outcome the panel + Check button read
  // (pending/up-to-date/new-version/unreachable/failed) - CUM-249.
  ota: 'idle', otaResult: 'pending', otaLatest: '', otaNotes: '', otaPct: -1, otaErr: '',
  lastOta: '-', autoUpd: false, otaBattOk: true, otaBattMsg: '',
  // Cloud pairing (Pairing + cloud card) - in the pairing state so the card,
  // code, and QR render for the harness + screenshots.
  cloud: { line: 'Pairing: enter the Cloud link code at app.cumulo-nimbus.ai.', state: 'pairing', paired: false, optIn: true, code: 'CN-4821-QK' },
  // Storage / files quota surface (N5 files payload stub)
  card: { sizeMB: 30436, freeMB: 28112, usedMB: 2324, quotaMB: 4096, quotaUsedMB: 512 },
};

// applyOrch consumes /api/orch as a rich object: providers is keyed BY NAME,
// cust is always present, and jobs is the sessions array (tag/backend/model/
// category/state). This mirrors the device closely enough that applyOrch runs
// clean (a thin stub throws on d.cust.base and never reaches the jobs block).
const provider = (present, verify, model) => ({ present, hasKey: present, verify, model, models: [model] });
export const ORCH = {
  running: true,
  providers: {
    anthropic: provider(true, 'ok', 'claude-sonnet-5'),
    openai: provider(false, 'unk', ''),
    cumulo: provider(true, 'ok', 'nimbus-1'),
    zai: provider(false, 'unk', ''),
  },
  // The custom-endpoint wire selector defaults to "openai" on the device (store.cpp),
  // never empty; an empty conv left the <select> blank in the audit render.
  cust: { base: '', conv: 'openai', model: '', hasKey: false },
  orchHost: 'anthropic',
  provPrio: 'anthropic,cumulo,zai', subPrio: 'cumulo,anthropic',
  sttProv: 'openai', ttsOn: false, ttsProv: 'openai', ttsVoice: 'alloy',
  theme: 'nimbus', hasTav: false, hasTg: false,
  tgAllow: '', tgBot: '', tgLive: false, tgVerify: 'unk', tgVts: 0,
  tavVerify: 'unk', tavVts: 0,
  fetchPol: 1, compactKB: 0, loopRounds: 0, loopDeadline: '', orchLoop: false,
  orchTrace: false, midFail: 0, tlsSlots: 1, tlsVerify: 'unk',
  modInbound: 0, modOutbound: 0, modInjection: 0,
  sfxLvlN: 2, sfxLvlO: 2, sfxTheme: 'terran', sfxVol: 60, sfxTier: 'basic', sfxSync: 'idle',
  usage: {
    sessIn: 12000, sessOut: 4300, turns: 8, lastIn: 900, lastOut: 300,
    // Per-provider ledger row. Field names mirror the device's providerUsageJson()
    // (store.cpp): prov / tokens / calls / tok(In|Out) / tot(In|Out|Calls) / rate*
    // / (token|call|cents)Limit / estCents. A prior fixture used provider/in/out/limit,
    // which the Budgets renderer does not read - it printed the provider as "undefined".
    byProvider: [{
      prov: 'anthropic', tokens: 16300, calls: 8,
      tokenLimit: 0, callLimit: 0, resetDay: 1, over: false,
      tokIn: 12000, tokOut: 4300, totIn: 12000, totOut: 4300, totCalls: 8,
      // estCents is server-computed from tok(In|Out) x rate(In|Out): 12000/1e6*300 +
      // 4300/1e6*1500 = 10 cents, kept consistent with the All-time tile's own estimate.
      rateIn: 300, rateOut: 1500, rateCall: 0, centsLimit: 0, estCents: 10, tags: [],
    }],
  },
  budget: { dailyUsd: 5, spentUsd: 1.24, cap: 'auto', effective: 5 },
  // Session rows as applyOrch renders them.
  jobs: [
    { tag: 'sess-1', backend: 'anthropic', model: 'claude-sonnet-5', category: 'chat', state: 'running' },
    { tag: 'sess-2', backend: 'cumulo', model: 'nimbus-1', category: 'routine', state: 'running' },
  ],
};

// N3 contract stubs
export const MODELS = {
  providers: [
    { id: 'anthropic', models: ['claude-opus-5', 'claude-sonnet-5', 'claude-haiku-4-5'] },
    { id: 'cumulo', models: ['nimbus-1'] },
    { id: 'zai', models: ['glm-4'] },
  ],
};
export const FALLBACKS = { chain: ['anthropic', 'cumulo', 'zai'] };

// N2 contract stub
export const WAKEUPS = {
  policy: 'silent-allow',
  items: [
    { id: 'w1', when: '07:30', label: 'Morning brief', enabled: true },
    { id: 'w2', when: '18:00', label: 'Evening recap', enabled: false },
  ],
  pending: null, // when a wake-up needs approval, an object; single card, never a loop
};

export const HEALTH = { ok: true, checks: [{ name: 'wifi', ok: true }, { name: 'sd', ok: true }] };

export const CONNECT = {
  // token is what the sign-in QR encodes; masked-safe fake for the harness
  token: 'HARNESSTOKEN123456',
  // Reach identity for the "On your network" row (proposal §1): the mDNS name and IP
  // each link to themselves with a one-time sign-in code, and Copy grabs the plain name.
  name: 'Nimbus', mdns: 'nimbus.local', apSsid: 'Nimbus-setup',
  mdnsUrl: 'http://nimbus.local/?c=HARNESSSIGNCODE', url: 'http://192.168.1.42/?c=HARNESSSIGNCODE',
  ip: '192.168.1.42', apPass: 'setup-pass',
  cloudCode: 'CN-4821', // "Cloud link code" (code 2)
  deviceCode: '82 41 07', // "Device sign-in code" (code 1)
};

export const THEMES = {
  active: 'nimbus',
  roles: [
    { role: 'primary', hex: '#5ad6c4', name: 'teal' },
    { role: 'money', hex: '#f0b45a', name: 'amber' },
  ],
  themes: [{ id: 'nimbus', name: 'Nimbus' }, { id: 'dusk', name: 'Dusk' }],
};

// Memory-pane payloads mirror the device handlers in web_memory.cpp. Prior fixtures
// used invented field names (blobs/episodic/scratchpadBytes, text, embedProvider), none
// of which the pane reads - so the badge printed "undefined scratch (cap 0)", a false
// "No SD card" banner showed over a present card, the recall-tuning inputs stayed blank,
// and the scratchpad read "(empty)". These match the real /api/mem/* contracts.
// GET /api/mem/stats (handleStats): counts + storage tier + embed object.
export const MEM_STATS = {
  vectors: 340, scratchItems: 1, episodicMsgs: 88, epiTruncated: false, epiFloor: '',
  embedAvailable: true, embedLocked: false,
  sdPresent: true, flashFull: false, maxVectors: 5000, store: 'SD /mem',
  embed: { provider: 'openai', model: 'text-embedding-3-small', dims: 1536 },
};
// GET /api/mem/scratchpad (handleScratchGet): active task + short/mid/long item lists.
export const MEM_SCRATCHPAD = {
  active: 'Draft the weekly status note',
  short: ['ring shows amber only for money/attention'], mid: [], long: [],
};
// GET /api/mem/config (handleConfigGet): recall-tuning knobs.
export const MEM_CONFIG = {
  retrieval_count: 8, relevance_threshold: 0.35, decay_factor: 0.98,
  max_context_bytes: 4096, max_vectors: 5000,
  recency_half_life_hours: 168, mmr_lambda: 0.5,
};
// GET /api/mem/embedcfg (handleEmbedCfgGet): embed provider must be openai|mistral.
export const MEM_EMBEDCFG = {
  provider: 'openai', model: 'text-embedding-3-small', dims: 1536, locked: false, vectors: 340,
};

export const FILES_LIST = {
  present: true, count: 2, bytes: 221640, freeBytes: 29478158336,
  card: { sizeMB: 30436, freeMB: 28112 },
  quota: { limitMB: 4096, usedMB: 512 },
  files: [
    { name: 'notes.txt', bytes: 1240, kind: 'text', project: 'default', mtime: 1690000000 },
    { name: 'photo.jpg', bytes: 220400, kind: 'image', project: 'default', mtime: 1690000500 },
  ],
};

// Shape loadConnectors expects: {configured, known, keyed, host}. Includes a
// device-dialed MCP server pending owner approval (CUM-33), so the approval card
// renders in the harness.
export const CONNECTORS = {
  host: 'anthropic',
  keyed: { anthropic: true },
  known: [
    { id: 'github', name: 'GitHub MCP', providers: 'anthropic', kind: 'mcp', cred: 'GitHub token', desc: 'Repos and issues over MCP.', docs: 'github' },
  ],
  configured: [
    { name: 'devtools', type: 'devtools', prov: 'anthropic', kind: 'mcp', url: 'https://mcp.example.dev/', en: 1, dev: 1, appr: 0, hasTok: true },
  ],
};
export const TOOLS = { tools: [{ id: 'web', name: 'Web fetch', enabled: true }], sandbox: { enabled: false } };
// GET /api/skills/list (web_skills.cpp): {sd, skills:[{id,title,version?,source,origin,pending?}]}.
// A prior fixture used {name,enabled}, neither of which the pane reads, and omitted `sd`,
// so the pane always claimed "No SD card - built-in skills only" and dropped the title.
export const SKILLS = {
  sd: true,
  skills: [{ id: 'summarize', title: 'Summarize', source: 'builtin', origin: 'builtin' }],
};
export const LOOPS = { loops: [] };
export const FETCHQ = { queue: [], downloads: [] };
// Daily usage buckets for the Usage spend chart. The device serves
// {today:<dayKey>,days:[{d,prov,in,out,calls}]} (store.cpp usageHistoryJsonPs), where
// a dayKey is whole days since the Unix epoch. A prior fixture served {buckets:[{t,usd}]},
// a shape the chart does not read, so it always fell back to "clock not set yet" and no
// chart ever rendered in the audit. Match the device contract.
const _dayKey = Math.floor(Date.now() / 86400000);
export const USAGE_HISTORY = {
  today: _dayKey,
  days: [
    { d: _dayKey - 3, prov: 'anthropic', in: 2600, out: 800, calls: 2 },
    { d: _dayKey - 2, prov: 'anthropic', in: 3200, out: 900, calls: 2 },
    { d: _dayKey - 1, prov: 'anthropic', in: 5100, out: 1600, calls: 3 },
    { d: _dayKey - 1, prov: 'cumulo', in: 2000, out: 700, calls: 2 },
    { d: _dayKey, prov: 'anthropic', in: 1100, out: 1000, calls: 1 },
  ],
};
export const TELEGRAM = { enabled: false, allowlist: [], pending: [], public: false };
export const TENANT = { name: 'default', tenants: ['default'] };
export const TRACE = { turns: [] };
export const VOICES = { voices: ['alloy', 'verse'] };

// Global search sources (CUM-62).
export const DOCS_SEARCH = {
  results: [
    { id: 'ota#battery-gate', title: 'Updates and the battery gate', snippet: 'An update waits until the battery is high enough, then downloads and verifies.' },
    { id: 'memory#recall', title: 'How recall works', snippet: 'The assistant searches memories by meaning and injects the top matches.' },
  ],
};
export const MEM_VECTOR = {
  items: [
    { text: 'Amber is reserved for money and attention signals only.' },
    { text: 'The ring shows one arc per active session.' },
  ],
};

// Generic OK for POST/action endpoints.
export const OK = { ok: true };

// POST /api/ota/check only ACCEPTS the check (the device runs it async and 202s
// {ok:true}); the verdict is read afterwards by polling /api/state's otaResult,
// never from this accept body (CUM-249).
export const OTA_CHECK = { ok: true };

// Saved Wi-Fi networks (CUM-207): GET /api/wifi shape the Connectivity Wi-Fi panel
// reads. A password is never present in this payload. `current` marks the one in use;
// list order is priority order. Enough entries to exercise the reorder "Up" affordance.
export const WIFI = {
  max: 5,
  count: 3,
  networks: [
    { ssid: 'Home 5th Floor', open: false, auto: true, current: true },
    { ssid: 'Cafe-Guest', open: true, auto: true, current: false },
    { ssid: 'Pixel Hotspot', open: false, auto: true, current: false },
  ],
  sta: true,
  staIp: '192.168.1.42',
  apUp: true,
  apSsid: 'Nimbus-setup',
  apIp: '192.168.4.1',
};

// Map endpoint path -> default response. GET-shaped; POST/actions fall back to OK.
export const DEFAULTS = {
  '/api/state': STATE,
  '/api/wifi': WIFI,
  '/api/orch': ORCH,
  '/api/models': MODELS,
  '/api/fallbacks': FALLBACKS,
  '/api/wakeups': WAKEUPS,
  '/api/health': HEALTH,
  '/api/connect': CONNECT,
  '/api/themes': THEMES,
  '/api/mem/stats': MEM_STATS,
  '/api/mem/scratchpad': MEM_SCRATCHPAD,
  '/api/mem/config': MEM_CONFIG,
  '/api/mem/embedcfg': MEM_EMBEDCFG,
  '/api/files/list': FILES_LIST,
  '/api/connectors': CONNECTORS,
  '/api/tools': TOOLS,
  '/api/skills/list': SKILLS,
  '/api/loops': LOOPS,
  '/api/fetchq': FETCHQ,
  '/api/usage/history': USAGE_HISTORY,
  '/api/telegram': TELEGRAM,
  '/api/tenant': TENANT,
  '/api/trace': TRACE,
  '/api/voices': VOICES,
  '/api/ota/check': OTA_CHECK,
  '/api/docs/search': DOCS_SEARCH,
  '/api/mem/vector': MEM_VECTOR,
};
