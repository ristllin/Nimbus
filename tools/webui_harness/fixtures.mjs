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
  // OTA (N5 contract stub): installed/latest/notes + result state
  ota: 'idle', otaLatest: '', otaNotes: '', otaPct: -1, otaErr: '',
  lastOta: '-', autoUpd: false,
  // Cloud pairing (Pairing + cloud card)
  cloud: { line: 'Cloud access is off.', state: 'off', paired: false, optIn: false, code: '' },
  // Storage / files quota surface (N5 files payload stub)
  card: { sizeMB: 30436, freeMB: 28112, usedMB: 2324, quotaMB: 4096, quotaUsedMB: 512 },
};

export const ORCH = {
  providers: [
    { id: 'anthropic', name: 'Anthropic', present: true, verify: 'ok', model: 'claude-sonnet-5' },
    { id: 'openai', name: 'OpenAI', present: false, verify: 'unk', model: '' },
    { id: 'cumulo', name: 'Cumulo Nimbus', present: true, verify: 'ok', model: 'nimbus-1' },
    { id: 'zai', name: 'Z.ai', present: false, verify: 'unk', model: '' },
  ],
  budget: { dailyUsd: 5, spentUsd: 1.24, cap: 'auto', effective: 5 },
  jobs: [
    { id: 'sess-1', title: 'Refactor webui', model: 'claude-sonnet-5', state: 'running' },
    { id: 'sess-2', title: 'Draft release notes', model: 'nimbus-1', state: 'running' },
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
  card: { sizeMB: 30436, freeMB: 28112 },
  files: [
    { name: 'notes.txt', size: 1240, project: 'default', mtime: 1690000000 },
    { name: 'photo.jpg', size: 220400, project: 'default', mtime: 1690000500 },
  ],
};

export const CONNECTORS = { connectors: [{ id: 'telegram', name: 'Telegram', state: 'on' }], mcp: [] };
export const TOOLS = { tools: [{ id: 'web', name: 'Web fetch', enabled: true }], sandbox: { enabled: false } };
export const SKILLS = { skills: [{ id: 'summarize', name: 'Summarize', enabled: true }] };
export const LOOPS = { loops: [] };
export const FETCHQ = { queue: [], downloads: [] };
export const USAGE_HISTORY = { buckets: [{ t: '2026-08-20', usd: 0.8 }, { t: '2026-08-21', usd: 1.1 }] };
export const TELEGRAM = { enabled: false, allowlist: [], pending: [], public: false };
export const TENANT = { name: 'default', tenants: ['default'] };
export const TRACE = { turns: [] };
export const VOICES = { voices: ['alloy', 'verse'] };

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
};
