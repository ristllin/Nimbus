// Playwright config for the Nimbus web-app suite (lane N1).
//
// Default target = the host harness server (T4 sim-e2e). To run the SAME specs
// against a real device over LAN (T5/HIL), set BASE_URL=http://<device-ip> and
// TARGET=device, and skip the webServer (the device is already serving). A valid
// token is supplied to device runs via NIMBUS_TOKEN (seeded into localStorage).
import { defineConfig, devices } from '@playwright/test';

const BASE_URL = process.env.BASE_URL || 'http://localhost:8790';
const ON_DEVICE = process.env.TARGET === 'device';

export default defineConfig({
  testDir: './tests',
  timeout: 30_000,
  expect: { timeout: 5_000 },
  fullyParallel: false,
  retries: 0,
  reporter: [['list'], ['html', { open: 'never', outputFolder: 'playwright-report' }]],
  outputDir: 'test-results',
  use: {
    baseURL: BASE_URL,
    trace: 'retain-on-failure',
    screenshot: 'only-on-failure',
  },
  projects: [
    { name: 'desktop', use: { ...devices['Desktop Chrome'], viewport: { width: 1280, height: 900 } } },
    { name: 'phone', use: { ...devices['Pixel 7'] } },
  ],
  webServer: ON_DEVICE
    ? undefined
    : {
        command: 'node server.mjs',
        url: 'http://localhost:8790/healthz',
        reuseExistingServer: !process.env.CI,
        stdout: 'ignore',
        stderr: 'pipe',
      },
});
