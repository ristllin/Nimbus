// /flash - browser flasher for Nimbus, powered by ESP Web Tools (vendored under
// static/vendor/esp-web-tools - no CDN/runtime third-party dependency).
//
// One merged image PER BOARD VARIANT lives at a FIXED per-variant path on the
// `webflash` branch of the public ristllin/nimbus-fw-releases repo, published by
// every release run of .github/workflows/release.yml. Each image carries an NVS
// seed (scrModel/tftFlip/mode/type) so the board comes up on the RIGHT panel and
// already knows its update type - it never boots to a blank screen. Fetched via
// raw.githubusercontent.com because GitHub release-asset downloads send no
// Access-Control-Allow-Origin header - the raw host does.
import React, {useEffect, useState} from 'react';
import Layout from '@theme/Layout';
import Link from '@docusaurus/Link';
import useBaseUrl from '@docusaurus/useBaseUrl';

// Board/size variants. `slug` is the typed-OTA device type and the per-variant
// webflash path segment; the two must match what release.yml publishes.
const VARIANTS = [
  {slug: 'nimbus-tft', label: 'Nimbus board (TFT + ring)'},
  {slug: 'freenove-28', label: 'Freenove CYD - 2.8 inch'},
  {slug: 'freenove-35', label: 'Freenove CYD - 3.5 inch'},
  {slug: 'freenove-40', label: 'Freenove CYD - 4.0 inch'},
];

const manifestUrlFor = (slug) =>
  `https://raw.githubusercontent.com/ristllin/nimbus-fw-releases/webflash/latest/${slug}/manifest.json`;

const styles = {
  main: {maxWidth: 760, margin: '0 auto', padding: '2rem 1rem 4rem'},
  box: {
    border: '1px solid var(--ifm-color-emphasis-300)',
    borderRadius: 8,
    padding: '1rem 1.25rem',
    margin: '1.25rem 0',
  },
  warn: {
    borderLeft: '4px solid var(--ifm-color-warning)',
    background: 'var(--ifm-color-warning-contrast-background)',
    borderRadius: 6,
    padding: '1rem 1.25rem',
    margin: '1.25rem 0',
  },
  installRow: {margin: '1.5rem 0', textAlign: 'center'},
};

export default function FlashPage() {
  const moduleUrl = useBaseUrl('/vendor/esp-web-tools/install-button.js');
  const [variant, setVariant] = useState(VARIANTS[0].slug);

  useEffect(() => {
    // The web component ships as an ES module with hashed sibling chunks it
    // lazy-imports by relative path, so it is loaded (once) as a module script
    // rather than bundled by Docusaurus.
    if (document.querySelector(`script[src="${moduleUrl}"]`)) return;
    const s = document.createElement('script');
    s.type = 'module';
    s.src = moduleUrl;
    document.body.appendChild(s);
  }, [moduleUrl]);

  return (
    <Layout
      title="Flash Nimbus from the browser"
      description="Install the Nimbus firmware on an ESP32-S3 board straight from Chrome or Edge - no toolchain needed.">
      <main style={styles.main}>
        <h1>Flash Nimbus from the browser</h1>
        <p>
          Install the latest Nimbus firmware release on an ESP32-S3-DevKitC-1
          board straight from this page - no toolchain, no downloads. The full
          walkthrough (including the command-line path) is in{' '}
          <Link to="/quick-start/flash">Quick Start &rarr; Flash the firmware</Link>.
        </p>

        <div style={styles.box}>
          <strong>You need</strong>
          <ul style={{marginBottom: 0}}>
            <li>
              <strong>Chrome or Edge on a computer</strong> - the flasher uses
              Web Serial, which other browsers and phones don&apos;t support.
            </li>
            <li>
              A <strong>data-capable USB cable</strong> - many USB cables are
              charge-only and never show a serial port.
            </li>
          </ul>
        </div>

        {variant === 'nimbus-tft' ? (
          <div style={styles.warn}>
            <strong>Nimbus board: use the port labeled UART.</strong> The
            DevKitC-1 has two USB-C ports and only one can flash a fresh board.
            Plug into the port silkscreened <code>UART</code>, not{' '}
            <code>USB</code> - on a factory-fresh board the native{' '}
            <code>USB</code> port has no path into download mode until Nimbus
            owns it. A board that &quot;won&apos;t flash&quot; on that port is
            not broken; it is on the wrong port.
          </div>
        ) : (
          <div style={styles.warn}>
            <strong>Freenove CYD: use its single USB-C port.</strong> The
            all-in-one board has one USB-C port and no separate UART bridge -
            just connect a data-capable USB-C cable. There is no wrong port to
            avoid.
          </div>
        )}

        <div style={styles.box}>
          <label htmlFor="variant" style={{fontWeight: 600, display: 'block', marginBottom: '0.5rem'}}>
            Which board do you have?
          </label>
          <select
            id="variant"
            value={variant}
            onChange={(e) => setVariant(e.target.value)}
            style={{padding: '0.4rem 0.6rem', fontSize: '1rem', width: '100%', maxWidth: 420}}>
            {VARIANTS.map((v) => (
              <option key={v.slug} value={v.slug}>
                {v.label}
              </option>
            ))}
          </select>
          <p style={{margin: '0.5rem 0 0'}}>
            <small>
              The image is matched to your board and screen size, so it comes up
              on the right display and already knows which updates it should get.
            </small>
          </p>
        </div>

        <div style={styles.installRow}>
          {/* key by variant so the button re-mounts with the new manifest */}
          <esp-web-install-button key={variant} manifest={manifestUrlFor(variant)}>
            <button
              slot="activate"
              className="button button--primary button--lg">
              Install Nimbus
            </button>
            <span slot="unsupported">
              This browser can&apos;t flash devices - use Chrome or Edge on a
              computer.
            </span>
            <span slot="not-allowed">
              Flashing needs a secure (HTTPS) page - reload this page over
              HTTPS.
            </span>
          </esp-web-install-button>
          <p style={{marginTop: '0.75rem'}}>
            <small>
              Pick the board&apos;s serial port when the browser asks. Choosing
              &quot;Erase device&quot; is fine on a new board; on a board
              already running Nimbus it wipes its saved settings.
            </small>
          </p>
        </div>

        <h2>After flashing</h2>
        <ul>
          <li>
            <strong>Ignore the flasher&apos;s Wi-Fi prompts.</strong> If the
            dialog offers to connect the device to Wi-Fi after installing,
            skip/close it - Nimbus provisions itself through its own setup
            network, not through the flasher.
          </li>
          <li>
            On your phone or computer, join the Wi-Fi network named{' '}
            <strong>Nimbus-setup</strong> (the password is shown on the device screen,
            unique to your device - or just scan the on-screen QR to join)
            and the setup wizard opens in your browser. Step-by-step:{' '}
            <Link to="/quick-start/setup-wizard">Set up the device</Link>.
          </li>
          <li>
            <strong>The screen is ready right away.</strong> The image you
            picked already tells the board which display it has, so it comes up
            on the correct panel - there is no blank-screen step and no display
            question in the wizard.
          </li>
        </ul>

        <h2>If no serial port shows up</h2>
        <ul>
          <li>Swap in a known-good data cable (the most common cause).</li>
          <li>
            Confirm the cable is in the <code>UART</code> port and try another
            USB port on the computer.
          </li>
          <li>
            Recovery and port details:{' '}
            <Link to="/quick-start/flash">Flash the firmware</Link>.
          </li>
        </ul>

        <h2>Verifying the image (optional)</h2>
        <p>
          First-time flashing writes the image as-is - like any firmware
          installer before secure boot, it is not signed. If you want to
          confirm the bytes before flashing, the image's SHA-256 is published
          next to it at{' '}
          <code>
            raw.githubusercontent.com/ristllin/nimbus-fw-releases/webflash/latest/nimbus-webflash.bin.sha256
          </code>
          . Over-the-air updates after setup are ECDSA-signed and verified on
          the device.
        </p>
      </main>
    </Layout>
  );
}
