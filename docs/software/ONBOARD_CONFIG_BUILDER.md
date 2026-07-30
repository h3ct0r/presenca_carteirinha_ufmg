# On-device config-builder — Design & Ideation

Hosting the browser-only config-builder (`tools/config-builder/`) **on the ESP32
itself**, served over the existing soft-AP, so a professor can author config by
connecting to the device instead of needing the tool on a laptop.

**Status: ideation only — nothing built.** No code, no firmware change, no
endpoint. This document is the design record from the 2026-07-28 session so the
work can be picked up later. Numbers below were measured; the security analysis
is against the code as it exists today.

Related: [`CONFIG_IMPORT.md`](CONFIG_IMPORT.md) (the tar contract this would
reuse), [`STUDENT_PHOTOS.md`](STUDENT_PHOTOS.md), the tool itself in
[`../../tools/config-builder/`](../../tools/config-builder/).

---

## 1. Why it makes sense

The decisive property: **the config-builder does zero server-side work.** CSV
parsing, name→id photo matching, canvas re-encode, and tar building all run in
the professor's browser. The ESP32 would be a dumb static-file host, so none of
this project's usual constraints apply — no LVGL heap pressure (128 KB), no CPU
contention with the camera, no new FreeRTOS task.

Three payoffs, in order of real value:

1. **Schema parity, permanently.** The tool and the firmware validator would ship
   in the same image, so they can never drift. Today that drift is managed by a
   manual ritual (contract → firmware → tool; see
   [`tools/config-builder/CLAUDE.md`](../../tools/config-builder/CLAUDE.md)
   "Keeping in sync"). Shipping them together deletes the whole bug class.
2. **Pre-fill from the live config.** A device-hosted tool can read the current
   `students.json` / `class.json` and open with the **real roster already
   loaded**, so the professor *edits* instead of re-authoring. This is impossible
   with the laptop tool and is probably the most useful single feature here.
3. **No laptop setup.** No repo checkout, no `python3 -m http.server`, no
   internet. Matches the tool's existing "offline, no CDN, everything vendored"
   rule — it was practically designed for this.

## 2. Measured facts (2026-07-28)

| Fact | Value |
|---|---|
| Tool size (`index.html` + `src/*.js`) | **79,884 bytes** (~80 KB) |
| Same, gzipped (`gzip -9`) | **23,335 bytes** (~23 KB) |
| `app0` partition | 6 MB (`0x600000`) |
| Flash used by firmware | 3,929,494 bytes (**62.5%**) → **~2.36 MB free** |
| `spiffs` partition | 9.3 MB (`0x8E0000`) — **entirely unused**, nothing in `src/` mounts it |
| Staged-tar cap | 16 MB (`MAX_TAR`, raised for bundled photos) |

The tool costs ~1% of remaining flash headroom. Storage is a non-issue by two
orders of magnitude.

## 3. What already exists (nothing here is greenfield)

| Piece | Where |
|---|---|
| Soft-AP, WPA2, per-boot credentials | [`src/services/wifi_ap.cpp`](../../src/services/wifi_ap.cpp) |
| HTTP server + list/read/download/save/rename/delete/upload | [`src/services/file_server.cpp`](../../src/services/file_server.cpp) |
| Precedent: an embedded web app in PROGMEM | the file-manager page itself (`INDEX_HTML`) |
| Validated tar import with backup + rollback | [`src/services/import_service.cpp`](../../src/services/import_service.cpp) |
| On-device import confirm dialog | [`src/ui/screens/scr_admin.cpp:618`](../../src/ui/screens/scr_admin.cpp) |

## 4. Hosting: where the files live

**Recommendation: embed in flash, gzipped**, served with
`Content-Encoding: gzip`.

| Option | Cost | Verdict |
|---|---|---|
| **Flash (PROGMEM/rodata), gzipped** | ~23 KB | ✅ **Recommended.** Can't be deleted; guaranteed to match the firmware — which is payoff #1. |
| SD card (`/www/`) | 0 flash | ❌ Reintroduces "files missing/stale/deleted" — the exact problem being solved. Would also need an importer whitelist carve-out. |
| SPIFFS partition (9.3 MB free) | 0 flash | ⚠️ Middle ground, but needs separate provisioning, so it inherits the staleness problem. |

**Tension to resolve honestly:** `tools/config-builder/CLAUDE.md` forbids a build
step ("No build step, no framework, no bundler"). Embedding needs a gzip+embed
step. Resolution: that is a **firmware packaging step, not a tool build step** —
the tool still opens from `file://` and still tests with `node --test`. This
should be written into that CLAUDE.md rather than quietly violated.

## 5. Security model

### 5.1 What protects the device today

There is **no HTTP auth at all** — `file_server.h` states it outright: *"Full
access, on purpose — this is a debug tool."* No token, no cookie, no rate limit.

The real controls are structural, and they are stronger than they look:

| Control | Where |
|---|---|
| AP is **off by default**; requires a physical tap to start | [`scr_wifi_editor.cpp:77`](../../src/ui/screens/scr_wifi_editor.cpp) |
| HTTP server exists **only while the WiFi screen is open** (`on_show`/`on_hide`) | [`scr_wifi_editor.cpp:26`](../../src/ui/screens/scr_wifi_editor.cpp) |
| Bottom nav **locked** while the AP is up — the professor is trapped on the screen, watching | [`scr_wifi_editor.cpp:58`](../../src/ui/screens/scr_wifi_editor.cpp) |
| WPA2, 8 random digits, **regenerated every boot** | [`wifi_ap.cpp:22`](../../src/services/wifi_ap.cpp) |
| Applying config requires a **physical tap** on the Admin confirm dialog | [`scr_admin.cpp:601`](../../src/ui/screens/scr_admin.cpp) |

In short: **security = physical presence + a short, supervised window.** That is
a legitimate model for a classroom device, and it is the property to preserve.

### 5.2 Threat model

- **Assets:** attendance records (the crown jewel — the entire point of the
  device), the authored config, teacher credentials, availability during class.
- **Realistic adversary:** a student in the room with a phone.
- **Realistic attack:** *not* cracking WPA2 — **photographing the AP password**,
  which the screen displays large and centered while the professor works.

### 5.3 Pre-existing holes (NOT created by this feature)

Both already ship today and are riskier than the feature under discussion:

1. **`/api/upload` writes anywhere on the SD, unauthenticated.** A browser on the
   AP can *already* drop `/config.tar` onto the card.
2. **`config.json` holds teacher passwords in plaintext** (digits-only,
   [`CONFIG_IMPORT.md`](CONFIG_IMPORT.md) §3.1), and
   `GET /api/read?path=/config.json` will serve them. Those same passwords unlock
   the device's admin UI.

**⚠️ DEFERRED BY DECISION (2026-07-28):** the user chose to **trust WPA2 alone
for now** and to **load passwords as usual** (no redaction, no merge rule). This
is an accepted, known risk — recorded here so it is not forgotten, to be revisited
when the feature is actually built. See §8.

## 6. Direct-apply design

### 6.1 The principle

> **Keep all authority on the LVGL screen. Give the HTTP endpoint none.**

The browser may **stage** a `config.tar`; **only a physical tap on the device
applies it.** Because staging already exists unauthenticated (§5.3), a
direct-apply built this way adds **zero new authority** — it only improves the UX
of the half that already exists.

### 6.2 Flow

1. Browser builds the tar exactly as today → `POST` to the upload path → lands in
   a fixed staging slot.
2. Device raises an LVGL modal: *"Apply config from 192.168.4.2? — 3 teachers,
   240 students, 5 classes. This replaces the current configuration.
   [Apply] [Reject]"*
3. The professor — already standing there, trapped on the screen by the nav lock
   — taps **Apply**.
4. The existing `import_service_run()` runs unchanged: whitelist → backup →
   validate → apply → rollback on failure.

The **summary line matters**: it is the professor's chance to notice *"240
students? I have 30."* — a cheap human integrity check on the payload.

Benefits: no tokens, no session state, no password over HTTP (so no brute-force
surface on a 4-digit secret), no new crypto. The trust boundary stays exactly
where it is today, and the confirm modal is a near-copy of the existing one.

### 6.3 Bounding the staging endpoint

Even without authority, unauthenticated upload allows nuisance. Cheap mitigations:
- enforce the 16 MB size cap **before** writing;
- a **single fixed staging slot** (overwrite, never accumulate);
- reject non-tar payloads early (magic check).

### 6.4 Options considered and rejected

- **Teacher password over HTTP** — reuses the device-unlock secret on a network
  channel; digits-only means brute-forceable in seconds without careful lockout.
  Adds a rate-limiting subsystem to buy what one physical tap gives for free.
- **Pairing code + session token** (TV-pairing pattern) — real state management
  (issue/expire/bind) for *less* security than the on-device tap. Revisit only if
  unattended or repeated applies are ever wanted.

## 7. Phased plan

1. **Serve the tool read-only from flash.** Gzip+embed script, one route, a link
   from the file-manager page. Biggest win, near-zero risk, **no new attack
   surface**. Worth shipping alone.
2. **Pre-fill from the live config.** `GET` the current authoring model so the
   professor edits the real roster. The feature that makes the on-device version
   *better* than the laptop one. (Passwords loaded as-is — see §5.3 deferral.)
3. **On-device-confirmed direct apply.** Removes the download/re-upload dance,
   which is especially painful on phones. Design in §6.
4. **Streaming unpack.** `import_service_run()` still `malloc`s the whole tar; at
   the 16 MB cap with bundled photos this is the likeliest thing to break. Direct
   apply will exercise it hardest. (Already tracked in `STUDENT_PHOTOS.md` §3.)

Step 1 is genuinely small and independently useful.

## 8. Open questions / deferred

- **Teacher-password exposure (§5.3.2)** — deferred by decision. When revisited,
  the options are: redact passwords from the pre-fill view (which then needs an
  "omitted password ⇒ preserve existing" merge rule on apply — a contract
  change), **or** keep teachers device-managed and let the tool pre-fill only
  students/classes (simpler, avoids the contract change entirely).
- **`/api/read` path denylist** — worth doing independently of this feature.
- **Sync `WebServer` blocking** — it is pumped from an LVGL timer every 5 ms;
  multi-MB uploads on that thread are a hardware-only unknown, like most of this
  project.
- **Merge or separate pages** — the file manager and the builder would be two
  embedded web apps; decide whether they share nav.
- **Does this belong before hardware validation?** Honest counterargument: this
  adds a second maintained surface while *nothing camera- or WiFi-related has
  been validated on hardware*. If the priority is proving face detection works on
  device, this is a pleasant detour from the actual blocker.

## Changelog

- **2026-07-28** — Document created from an ideation session. Measured the tool
  size / flash headroom / unused SPIFFS partition; audited the current security
  posture (no HTTP auth; physical-presence model) and found two pre-existing
  holes (unauthenticated SD upload; plaintext teacher passwords readable via
  `/api/read`). Recommended flash-embedded hosting and a staging-only apply
  endpoint whose authority stays on the LVGL confirm modal. **Decisions taken:**
  trust WPA2 alone for now; load passwords as usual; address password exposure
  later. **Nothing implemented.**
