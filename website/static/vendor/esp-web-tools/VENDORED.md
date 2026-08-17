# Vendored: esp-web-tools

- Package: [`esp-web-tools`](https://www.npmjs.com/package/esp-web-tools) v10.4.0
  (Apache-2.0 - see LICENSE alongside this file)
- Contents: the `dist/web/` browser bundle, copied verbatim. `install-button.js`
  is the entry module; the hashed chunks are lazy-loaded by relative path, so the
  directory must be copied as a whole.
- Consumed by: `website/src/pages/flash.jsx` (the /flash browser-flasher page).
- Vendored (not CDN) so the flasher keeps working with no third-party runtime
  dependency.

To update: `npm pack esp-web-tools` (or `npm install esp-web-tools` in a scratch
dir), replace every file here with the new `dist/web/*`, update the version above,
and re-test /flash against a real board.
