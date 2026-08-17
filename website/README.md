# Nimbus documentation site

A [Docusaurus 3](https://docusaurus.io) (classic preset, **docs-only** mode) site
for the Nimbus ESP32-S3 firmware. Published to GitHub Pages.

## Where the content comes from

Most pages are a **published view of the repo's top-level `docs/`** Markdown -
they are *migrated*, not hand-maintained here. The originals stay canonical; this
site never moves or deletes them.

- `scripts/migrate-docs.mjs` copies each prose doc from `../docs` into
  `docs/**`, adds Docusaurus front-matter, and fixes links that don't resolve
  inside the site (cross-repo source links, moved paths, binary/CAD assets). It's
  idempotent - re-run it whenever the source docs change.
- Two pages are **written from scratch and live only here** (the script never
  touches them):
  - `docs/getting-started/first-time-setup.md`
  - `docs/getting-started/webui-reference.md`
- `docs/intro.md` is the site home (`slug: /`).

To regenerate the migrated pages:

```bash
node scripts/migrate-docs.mjs
```

## Run locally

```bash
npm install
npm start          # dev server with hot reload at http://localhost:3000/Nimbus/
```

Build a static site and preview the production output:

```bash
npm run build
npm run serve
```

Requires Node 18+.

## How the CI deploy works

`.github/workflows/docs.yml` builds and publishes on every push to `main` that
touches `website/**`, `docs/**`, or the workflow itself:

1. `npm ci` (falling back to `npm install`) in `website/`,
2. `node scripts/migrate-docs.mjs` to refresh the migrated pages from `docs/`,
3. `npm run build`,
4. upload `website/build` and deploy via `actions/deploy-pages`.

The site expects to be served under `/<projectName>/` - `baseUrl` is `/Nimbus/`
and `organizationName`/`projectName` are `ristllin`/`Nimbus` in
`docusaurus.config.js`. Adjust those and `url` if the repo moves. Enable Pages
for the repo with **Settings → Pages → Source: GitHub Actions**.

## Layout

```
website/
  docusaurus.config.js   site config (docs-only, baseUrl /Nimbus/)
  sidebars.js            curated sidebar (Getting started / Guides / Reference)
  package.json           Docusaurus 3 classic
  scripts/migrate-docs.mjs
  src/css/custom.css     teal-on-dark theme
  static/img/            logo, favicon, social card
  docs/
    intro.md
    getting-started/     first-time-setup, webui-reference (hand-written)
    guides/              migrated from docs/*.md
    reference/           migrated from docs/reference/*.md
```
