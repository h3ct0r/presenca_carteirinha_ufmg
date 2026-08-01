# Deploying the Config Builder

The Config Builder is a **static single-page app**: one `index.html` plus the ES
modules under `src/` and the `fixtures/` sample. There is **no build step, no
backend, and no network calls** — deploying it just means serving these files as
static content. Everything (validation, tar generation, the `config.tar`
download) happens in the visitor's browser; nothing is uploaded to the server.

> **Privacy note:** the generated `config.tar` — which contains the teachers'
> passwords — is built and downloaded entirely client-side. It never touches the
> web server, so hosting this tool exposes no configuration data. The only reason
> to restrict access is to control *who may author* configs (see
> [access control](#optional-restrict-access) below).

Files to serve (the whole `tools/config-builder/` directory):

```
index.html
src/*.js
fixtures/example.model.json
```

The `test/`, `hooks/`, and `*.md` files are not needed at runtime; you can ship
just the three items above if you want a minimal bundle.

---

## GitHub Pages (canonical public host)

The **Deploy site** workflow publishes this tool at:

**https://verlab.github.io/presenca_carteirinha_ufmg/config/**

alongside the firmware installer (`/firmware/`) and a small hub at `/`.
See [`docs/site/README.md`](../../docs/site/README.md). Config still builds
entirely in the browser — the Pages host only serves static files.

## Development — `python3 http.server`

From this directory:

```bash
cd tools/config-builder
python3 -m http.server 8000
```

Then open <http://localhost:8000>. Use a server (not `file://`) so the browser
can `fetch()` the example roster and load the ES modules under the same origin.
(Site-nav links to `../firmware/` only resolve in the assembled Pages tree.)

### Cache gotcha while iterating

`python3 -m http.server` sends **no cache headers**, and browsers cache ES
modules aggressively — so after you edit `src/app.js` a plain reload may still
run the old code. Two ways around it:

- **Hard reload** the page (DevTools open → right-click reload → *Empty Cache and
  Hard Reload*), or
- run a tiny **no-store** variant so every response revalidates. Save this as
  `devserver.py` and run `python3 devserver.py`:

  ```python
  import http.server, socketserver

  class NoCache(http.server.SimpleHTTPRequestHandler):
      def end_headers(self):
          self.send_header("Cache-Control", "no-store, max-age=0")
          super().end_headers()

  socketserver.TCPServer.allow_reuse_address = True
  socketserver.TCPServer(("127.0.0.1", 8000), NoCache).serve_forever()
  ```

You can also point any other static dev server at this directory
(`npx serve`, `php -S localhost:8000`, etc.) — they are all equivalent.

---

## Production — nginx

Copy the app to the server and point an nginx `server` block at it.

```bash
sudo mkdir -p /var/www/config-builder
sudo rsync -a --delete tools/config-builder/ /var/www/config-builder/
# (or ship only index.html, src/, and fixtures/)
```

`/etc/nginx/sites-available/config-builder.conf`:

```nginx
server {
    listen 80;
    server_name config.example.edu;          # your hostname

    root /var/www/config-builder;
    index index.html;

    # Static SPA: serve the file if it exists, else fall back to index.html.
    location / {
        try_files $uri $uri/ /index.html;
    }

    # ES modules must be served with a JavaScript MIME type or the browser
    # refuses to run them. nginx's bundled mime.types maps .js correctly on
    # current releases; this makes it explicit and future-proof.
    types { text/javascript js mjs; }        # inside http{} or here
    default_type application/octet-stream;

    # Small text assets — compress them.
    gzip on;
    gzip_types text/javascript application/javascript application/json text/css text/html;

    # Let the browser revalidate instead of serving stale JS after a redeploy.
    # nginx already sends Last-Modified + ETag; this forces a conditional check.
    location ~* \.(js|mjs|json|html)$ {
        add_header Cache-Control "no-cache";
    }
}
```

Enable it and reload:

```bash
sudo ln -s /etc/nginx/sites-available/config-builder.conf /etc/nginx/sites-enabled/
sudo nginx -t && sudo systemctl reload nginx
```

### Stamping the build hash

The header shows a build id in its top-right corner — `v0.2.0` from
`src/version.js`, which mirrors the firmware's `APP_VERSION` (a test enforces
that the two match). Because there is no build step, the git hash is **not**
committed: a value checked into the repo would describe whoever last edited that
line, not the deployment.

Stamp it on the **deployed copy** instead, right after the rsync, and the header
reads `v0.2.0+ff64e14` — the same format the device shows on its idle and About
screens, so a support conversation can compare the two directly:

```bash
sudo sed -i.bak "s/BUILD_SHA = ''/BUILD_SHA = '$(git rev-parse --short=7 HEAD)'/" \
  /var/www/config-builder/src/version.js && sudo rm -f /var/www/config-builder/src/version.js.bak
```

Run it from a checkout of this repo (that is where `git rev-parse` reads from).
Skipping it is fine — the header just shows the plain version.

### HTTPS (recommended if reachable over a network)

The tool works fine over plain HTTP on a trusted LAN, but if it's reachable
beyond one machine, terminate TLS. The simplest route is Let's Encrypt:

```bash
sudo apt install certbot python3-certbot-nginx
sudo certbot --nginx -d config.example.edu
```

certbot rewrites the `server` block to listen on 443 and redirect 80 → 443.

### Optional: restrict access

Authoring a config isn't sensitive (nothing leaves the browser), but if you want
to limit who can reach the page, add HTTP Basic Auth:

```bash
sudo apt install apache2-utils
sudo htpasswd -c /etc/nginx/.htpasswd staff
```

```nginx
location / {
    auth_basic "Config Builder";
    auth_basic_user_file /etc/nginx/.htpasswd;
    try_files $uri $uri/ /index.html;
}
```

For internal-only use, prefer binding to a private interface or a VPN over
exposing it publicly.

---

## Sanity check after deploying

1. Load the page — it should show the example roster and a green **Ready to
   export** status, with the build id in the header's top-right corner (see
   [Stamping the build hash](#stamping-the-build-hash)). A blank corner there
   means `src/version.js` did not ship or the module failed to load.
2. Open the browser console — there should be **no errors** (a MIME-type error on
   `src/app.js` means the `.js` type mapping above is missing).
3. Click **Download config.tar** and confirm a `config.tar` downloads.
4. *(optional)* `tar tf config.tar` locally to confirm it contains
   `config.json`, `students/students.json`, and `classes/<CODE>/class.json`.
