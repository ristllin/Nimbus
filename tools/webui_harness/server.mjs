// server.mjs - a tiny host-side stand-in for the device web server. Serves the
// reconstructed page (GET /), the logo (GET /logo.svg), and default JSON for the
// /api/* surface so the web app renders and happy-path flows run without a
// device. Playwright specs override individual endpoints with page.route() to
// exercise feedback states (pending / empty / error / slow).
//
// This is the T4 (sim-e2e) tier for the web app. The real-device-over-LAN tier
// (T5/HIL) is documented in tools/webui_harness/README.md and runs the same
// specs against BASE_URL=http://<device-ip>.
import { createServer } from 'node:http';
import { buildPage, logoSvg } from './concat.mjs';
import { DEFAULTS, OK } from './fixtures.mjs';

const PORT = Number(process.env.PORT || 8790);

function send(res, code, type, body) {
  res.writeHead(code, { 'content-type': type, 'cache-control': 'no-store' });
  res.end(body);
}

const server = createServer((req, res) => {
  const url = new URL(req.url, `http://localhost:${PORT}`);
  const path = url.pathname;

  if (path === '/' || path === '/index.html') {
    return send(res, 200, 'text/html; charset=utf-8', buildPage());
  }
  if (path === '/logo.svg') {
    return send(res, 200, 'image/svg+xml', logoSvg());
  }
  if (path === '/healthz') {
    return send(res, 200, 'text/plain', 'ok');
  }
  if (path.startsWith('/api/')) {
    // Drain any request body (uploads/forms) so the socket closes cleanly.
    req.on('data', () => {});
    req.on('end', () => {
      const fixture = DEFAULTS[path] ?? OK;
      send(res, 200, 'application/json', JSON.stringify(fixture));
    });
    return;
  }
  return send(res, 404, 'text/plain', 'not found');
});

server.listen(PORT, () => {
  // eslint-disable-next-line no-console
  console.log(`webui harness on http://localhost:${PORT}`);
});
