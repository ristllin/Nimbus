// @ts-check
// Docusaurus 3 config for the Nimbus firmware documentation site.
// Docs-only mode: the docs plugin is mounted at the site root (routeBasePath '/')
// and the blog is disabled, so there is no separate landing page - `docs/intro.md`
// (slug: '/') is the home page. Content is sourced from the repo's top-level
// `docs/` tree (migrated into `website/docs/` by website/scripts/migrate-docs.mjs).

const {themes} = require('prism-react-renderer');
const lightCodeTheme = themes.github;
const darkCodeTheme = themes.dracula;

// organizationName/projectName drive the GitHub deploy target and the
// "edit this page" links (github.com/<org>/<repo>). The site is served from
// the custom domain docs.cumulo-nimbus.ai (a grey-cloud CNAME to GitHub
// Pages, set in website/static/CNAME), so url is that domain and baseUrl is
// '/' (not '/Nimbus/'). GitHub 301-redirects the old ristllin.github.io/Nimbus
// path to the custom domain.
const organizationName = 'ristllin';
const projectName = 'Nimbus';

/** @type {import('@docusaurus/types').Config} */
const config = {
  title: 'Nimbus',
  tagline: 'Firmware for a battery-capable ESP32-S3 desk device - Notifier + Orchestrator',
  favicon: 'img/favicon.svg',

  // Served from the custom domain (see website/static/CNAME).
  url: 'https://docs.cumulo-nimbus.ai',
  // Root of the custom domain, not a project subpath.
  baseUrl: '/',

  organizationName, // GitHub org/user.
  projectName, // GitHub repo name.
  trailingSlash: false,

  // Every internal link must resolve - the migrate script de-links anything
  // that does not publish, so a broken link here is a real regression.
  onBrokenLinks: 'throw',
  // (onBrokenMarkdownLinks defaults to 'warn', which is what we want.)

  // Parse .md as CommonMark (not strict MDX): the migrated firmware docs are full
  // of prose `<name>` / `{reply,...}` fragments that would otherwise trip the MDX
  // JSX parser. .mdx files (none today) would still get full MDX.
  markdown: {
    format: 'detect',
    mermaid: true,   // render ```mermaid fences as diagrams (architecture, hardware, flashing…)
  },

  themes: ['@docusaurus/theme-mermaid'],

  i18n: {
    defaultLocale: 'en',
    locales: ['en'],
  },

  presets: [
    [
      'classic',
      /** @type {import('@docusaurus/preset-classic').Options} */
      ({
        docs: {
          sidebarPath: require.resolve('./sidebars.js'),
          // Docs-only mode: serve docs at the site root.
          routeBasePath: '/',
          // "Edit this page": migrated pages carry a per-page custom_edit_url
          // pointing at their canonical source under docs/ (written by
          // scripts/migrate-docs.mjs). This base covers only the hand-written
          // pages (intro.md, getting-started/), whose source IS website/docs.
          editUrl: `https://github.com/${organizationName}/${projectName}/tree/main/website/`,
        },
        blog: false,
        theme: {
          customCss: require.resolve('./src/css/custom.css'),
        },
      }),
    ],
  ],

  themeConfig:
    /** @type {import('@docusaurus/preset-classic').ThemeConfig} */
    ({
      image: 'img/social-card.png',
      colorMode: {
        defaultMode: 'dark',
        respectPrefersColorScheme: true,
      },
      navbar: {
        title: 'Nimbus',
        logo: {
          alt: 'Nimbus',
          src: 'img/logo.svg',
        },
        items: [
          {
            type: 'docSidebar',
            sidebarId: 'docsSidebar',
            position: 'left',
            label: 'Docs',
          },
          {
            to: '/quick-start/what-you-need',
            label: 'Get started',
            position: 'left',
          },
          {
            to: '/flash',
            label: 'Flash',
            position: 'left',
          },
          {
            href: `https://github.com/${organizationName}/${projectName}`,
            label: 'GitHub',
            position: 'right',
          },
        ],
      },
      footer: {
        style: 'dark',
        links: [
          {
            title: 'Docs',
            items: [
              {label: 'Quick Start', to: '/quick-start/what-you-need'},
              {label: 'Bill of materials', to: '/guides/hardware-bom'},
              {label: 'Web UI reference', to: '/getting-started/webui-reference'},
            ],
          },
          {
            title: 'Reference',
            items: [
              {label: 'Config & NVS', to: '/reference/config-and-nvs'},
              {label: 'Tool catalog', to: '/reference/tool-catalog'},
              {label: 'Turn contract', to: '/reference/turn-contract'},
              {label: 'Changelog', to: '/reference/changelog'},
            ],
          },
          {
            title: 'More',
            items: [
              {label: 'GitHub', href: `https://github.com/${organizationName}/${projectName}`},
            ],
          },
        ],
        copyright: `Nimbus - source-available, free for noncommercial use (PolyForm-NC / CC BY-NC-SA). Built with Docusaurus.`,
      },
      prism: {
        theme: lightCodeTheme,
        darkTheme: darkCodeTheme,
        additionalLanguages: ['bash', 'json', 'cpp', 'python', 'ini'],
      },
    }),
};

module.exports = config;
