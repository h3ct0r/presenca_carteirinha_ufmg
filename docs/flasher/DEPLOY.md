# Deploying the web firmware installer

Static page on GitHub Pages. Firmware binaries come from **GitHub Releases**
(not committed to git).

## Setup

1. Repository should be **public** (`h3ct0r/presenca_carteirinha_ufmg`).
2. **Settings → Pages → Source: GitHub Actions**.
3. URL after first deploy:
   **https://h3ct0r.github.io/presenca_carteirinha_ufmg/**

## How downloads work

Browsers cannot `fetch()` release files directly from `github.com` (CORS). The
**Deploy flasher** workflow copies each release `.bin` into `bins/` on the Pages
site at build time. The installer then downloads from the same origin
(`./bins/rfid-attendance-<tag>-firmware.bin`).

After a new `v*` tag, Pages redeploys when the **Release** workflow finishes (or
on push to `main`). Manual: **Actions → Deploy flasher → Run workflow**.

## Browser requirements

Chrome or Edge on desktop (Web Serial). Close PlatformIO Serial Monitor / other
serial tools before flashing.
