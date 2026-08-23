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
  // Reach info the Home info line + connectivity render.
  apSsid: 'Nimbus-setup', apIp: '192.168.4.1', ip: '192.168.1.42',
  mdns: 'nimbus.local', running: true, scrModel: 'tft',
  // OTA (N5 contract stub): installed/latest/notes + result state
  ota: 'idle', otaLatest: '', otaNotes: '', otaPct: -1, otaErr: '',
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
  cust: { base: '', conv: '', model: '', hasKey: false },
  orchHost: 'anthropic',
  provPrio: 'anthropic,cumulo,zai', subPrio: 'cumulo,anthropic',
  sttProv: 'openai', ttsOn: false, ttsProv: 'openai', ttsVoice: 'alloy',
  theme: 'nimbus', hasTav: false, hasTg: false,
  tgAllow: '', tgBot: '', tgLive: false, tgVerify: 'unk', tgVts: 0,
  tavVerify: 'unk', tavVts: 0,
  fetchPol: 'ask', compactKB: 0, loopRounds: 0, loopDeadline: '', orchLoop: false,
  orchTrace: false, midFail: 0, tlsSlots: 1, tlsVerify: 'unk',
  sfxLvlN: 2, sfxLvlO: 2, sfxTheme: 'terran', sfxVol: 60, sfxTier: 'basic', sfxSync: 'idle',
  usage: {
    sessIn: 12000, sessOut: 4300, turns: 8, lastIn: 900, lastOut: 300,
    byProvider: [{ provider: 'anthropic', in: 12000, out: 4300, calls: 8, limit: 0 }],
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

// N5 safety stub: three moderation gates + a cost note.
export const SAFETY = {
  input: false, output: false, media: false,
  costNote: 'Each gate that is on adds a small provider moderation call per item.',
};

export const HEALTH = { ok: true, checks: [{ name: 'wifi', ok: true }, { name: 'sd', ok: true }] };

export const CONNECT = {
  // token is what the sign-in QR encodes; masked-safe fake for the harness
  token: 'HARNESSTOKEN123456',
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

export const MEM_STATS = { blobs: 12, vectors: 340, episodic: 88, scratchpadBytes: 2048 };
export const MEM_SCRATCHPAD = { text: 'remember: ring shows amber only for money/attention' };
export const MEM_CONFIG = { longTerm: true, embedProvider: 'anthropic' };
export const MEM_EMBEDCFG = { provider: 'anthropic', model: 'text-embed-3', present: true };

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
export const SKILLS = { skills: [{ id: 'summarize', name: 'Summarize', enabled: true }] };
export const LOOPS = { loops: [] };
export const FETCHQ = { queue: [], downloads: [] };
export const USAGE_HISTORY = { buckets: [{ t: '2026-08-20', usd: 0.8 }, { t: '2026-08-21', usd: 1.1 }] };
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

// N5 OTA check result stub - "installed / latest / notes / result states".
export const OTA_CHECK = {
  result: 'available', installed: 'v4.3.0-pre', latest: 'v4.3.0',
  notes: 'Web app revamp + feedback states.', battOk: true,
};

// Map endpoint path -> default response. GET-shaped; POST/actions fall back to OK.
export const DEFAULTS = {
  '/api/state': STATE,
  '/api/orch': ORCH,
  '/api/models': MODELS,
  '/api/fallbacks': FALLBACKS,
  '/api/wakeups': WAKEUPS,
  '/api/safety': SAFETY,
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
