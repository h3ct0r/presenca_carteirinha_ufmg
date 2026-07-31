# GitHub Pages site (hub)

Static hub assembled by `.github/workflows/pages.yml` into the published site:

| Path | Source |
|------|--------|
| `/` | `docs/site/` (this folder) |
| `/config/` | `tools/config-builder/` (runtime files) |
| `/firmware/` | `docs/flasher/` + release bins |

Published URL: https://verlab.github.io/presenca_carteirinha_ufmg/

No `gh-pages` branch — deploy is **GitHub Actions** only.

Local preview of the assembled layout:

```bash
# from repo root (requires gh + existing releases for bins, optional)
mkdir -p /tmp/presenca-site
cp docs/site/index.html docs/site/hub.css /tmp/presenca-site/
mkdir -p /tmp/presenca-site/config/src /tmp/presenca-site/config/fixtures
cp tools/config-builder/index.html /tmp/presenca-site/config/
cp tools/config-builder/src/*.js /tmp/presenca-site/config/src/
cp tools/config-builder/fixtures/example.model.json /tmp/presenca-site/config/fixtures/
mkdir -p /tmp/presenca-site/firmware
cp -a docs/flasher/. /tmp/presenca-site/firmware/
cd /tmp/presenca-site && python3 -m http.server 8000
```

Config-builder alone (unchanged): `cd tools/config-builder && python3 -m http.server 8000`.
