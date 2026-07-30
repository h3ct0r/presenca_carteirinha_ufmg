/**
 * RFID Attendance — USB installer (Web Serial + esptool-js).
 * Release metadata from GitHub API; .bin served from ./bins/ (same origin).
 */

/** Resolve owner/repo from GitHub Pages host so forks work without edits. */
function detectRepo() {
  const host = location.hostname || "";
  const m = /^([\w-]+)\.github\.io$/i.exec(host);
  if (m) {
    const segs = location.pathname.split("/").filter(Boolean);
    if (segs[0]) return `${m[1]}/${segs[0]}`;
  }
  return "h3ct0r/presenca_carteirinha_ufmg";
}

const REPO = detectRepo();
const BIN_PREFIX = "rfid-attendance-";
const BIN_SUFFIX = "-firmware.bin";
const FLASH_ADDR = 0x10000;
const MONITOR_BAUD = 115200;
const DEFAULT_FLASH_BAUD = 115200;
const CONNECT_TIMEOUT_MS = 28000;
const USB_JTAG_SERIAL_PID = 0x1001;

/** Prefer Espressif USB-JTAG/Serial and common USB-UART bridges. */
const USB_PORT_FILTERS = [
  { usbVendorId: 0x303a }, // Espressif
  { usbVendorId: 0x1a86 }, // WCH CH340/CH341
  { usbVendorId: 0x10c4 }, // Silicon Labs CP210x
  { usbVendorId: 0x0403 }, // FTDI
];

const USB_ADAPTER_NAMES = {
  "303a:1001": "Espressif USB JTAG/serial",
  "303a:4001": "Espressif USB serial",
  "1a86:7523": "WCH CH340",
  "1a86:5523": "WCH CH341 serial",
  "10c4:ea60": "Silicon Labs CP210x",
  "0403:6001": "FTDI FT232",
};

let selectedPort = null;
let releases = [];
let deviceVersion = null;
let esptoolModule = null;

const $ = (id) => document.getElementById(id);
const logEl = $("log");
const progressWrap = $("progressWrap");
const progressBar = $("progressBar");

function log(msg) {
  const t = new Date().toLocaleTimeString();
  logEl.textContent += `[${t}] ${msg}\n`;
  logEl.scrollTop = logEl.scrollHeight;
}

function setStatus(msg, kind = "") {
  const el = $("statusLine");
  if (!el) return;
  el.textContent = msg || "";
  el.className = kind ? `status-line ${kind}` : "status-line";
}

function setProgress(pct) {
  progressWrap.classList.add("visible");
  progressBar.style.width = `${Math.min(100, Math.max(0, pct))}%`;
}

function hideProgress() {
  progressWrap.classList.remove("visible");
  progressBar.style.width = "0%";
}

function sleep(ms) {
  return new Promise((r) => setTimeout(r, ms));
}

function flashBaud() {
  const sel = $("flashBaud");
  return sel ? parseInt(sel.value, 10) || DEFAULT_FLASH_BAUD : DEFAULT_FLASH_BAUD;
}

function usbAdapterName(port) {
  const info = port.getInfo();
  if (!info.usbVendorId) return "USB serial";
  const key =
    info.usbVendorId.toString(16) +
    ":" +
    (info.usbProductId || 0).toString(16);
  return USB_ADAPTER_NAMES[key] || `USB serial (${key})`;
}

function configureLoaderBaud(loader, baud) {
  loader.baudrate = baud;
  loader.romBaudrate = baud;
}

async function ensurePortClosed(port) {
  try {
    await port.close();
  } catch (_) {}
}

/** Set DTR/RTS one at a time — combined setSignals breaks on some Chromium builds */
async function setLineSignals(port, dtr, rts) {
  if (dtr !== undefined) {
    await port.setSignals({ dataTerminalReady: dtr });
  }
  if (rts !== undefined) {
    await port.setSignals({ requestToSend: rts });
  }
}

async function probeSerialControl(port) {
  await ensurePortClosed(port);
  try {
    await port.open({ baudRate: 115200, bufferSize: 8192 });
    await sleep(80);
    await setLineSignals(port, false, false);
    await sleep(40);
    await setLineSignals(port, false, true);
    await sleep(40);
    await setLineSignals(port, false, false);
    await ensurePortClosed(port);
    await sleep(150);
    return { ok: true };
  } catch (e) {
    await ensurePortClosed(port);
    const msg = e.message || String(e);
    return {
      ok: false,
      error: `USB control signals failed (${msg}). Enter download mode manually (BOOT+RST).`,
    };
  }
}

/** esptool.py classic reset — D0|R1|W100|D1|R0|W400|D0 */
async function classicBootloaderReset(port, baud = 115200) {
  await ensurePortClosed(port);
  await port.open({ baudRate: baud, bufferSize: 8192 });
  await sleep(80);
  await setLineSignals(port, false, true);
  await sleep(100);
  await setLineSignals(port, true, false);
  await sleep(400);
  await setLineSignals(port, false, undefined);
  await sleep(200);
  await ensurePortClosed(port);
  await sleep(250);
}

/** Pulse EN (RTS) so the chip runs firmware — Web Serial often fails flashDeflFinish(reboot). */
async function hardResetEsp(port, baud = 115200) {
  await ensurePortClosed(port);
  await port.open({ baudRate: baud, bufferSize: 8192 });
  await sleep(80);
  await setLineSignals(port, false, false);
  await sleep(100);
  await setLineSignals(port, undefined, true);
  await sleep(80);
  await setLineSignals(port, undefined, false);
  await sleep(300);
  await ensurePortClosed(port);
}

async function rebootAfterFlash(port, loader, transport, baud) {
  log("Resetting ESP32-P4…");
  try {
    if (loader.IS_STUB) await loader.flashDeflFinish(true);
  } catch (e) {
    log(`Stub reboot: ${e.message || e} — trying hardware reset.`);
  }
  await sleep(300);
  try {
    await loader.after("hard_reset");
  } catch (e) {
    log(`hard_reset: ${e.message || e}`);
  }
  try {
    await transport.disconnect();
  } catch (_) {}
  await ensurePortClosed(port);
  await sleep(200);
  try {
    await hardResetEsp(port, baud);
    log("Hardware reset sent.");
  } catch (e) {
    log(`Hardware reset: ${e.message || e}`);
  }
}

function withTimeout(promise, ms, label) {
  return Promise.race([
    promise,
    sleep(ms).then(() => {
      throw new Error(`${label} (timeout ${ms / 1000}s)`);
    }),
  ]);
}

function parseTagVer(tag) {
  const m = /^v?(\d+)\.(\d+)\.(\d+)/i.exec(tag || "");
  if (!m) return null;
  return { major: +m[1], minor: +m[2], patch: +m[3], raw: tag };
}

function compareVer(a, b) {
  if (!a || !b) return 0;
  if (a.major !== b.major) return a.major - b.major;
  if (a.minor !== b.minor) return a.minor - b.minor;
  return a.patch - b.patch;
}

function normalizeDeviceVer(s) {
  if (!s) return null;
  const m = /v?(\d+\.\d+\.\d+)/i.exec(s);
  return m ? parseTagVer(m[1]) : parseTagVer(s);
}

function isFirmwareAsset(name) {
  return (
    typeof name === "string" &&
    name.startsWith(BIN_PREFIX) &&
    name.endsWith(BIN_SUFFIX)
  );
}

function updateUI() {
  const rel = releases.find((r) => r.tag === $("releaseSelect").value);
  const cmp = $("versionCompare");
  const dv = normalizeDeviceVer(deviceVersion);

  $("deviceVersion").textContent = deviceVersion || "—";

  if (rel && dv) {
    const rv = parseTagVer(rel.tag);
    if (rv) {
      const c = compareVer(rv, dv);
      if (c > 0)
        cmp.innerHTML =
          '<span class="compare-newer">Newer release available.</span>';
      else if (c < 0)
        cmp.innerHTML =
          '<span class="compare-older">Device is newer than selected build.</span>';
      else cmp.textContent = "Device matches selected release.";
      return;
    }
  }
  cmp.textContent = "";

  const canInstall =
    releases.length > 0 &&
    $("releaseSelect").value &&
    $("ackFlash").checked &&
    "serial" in navigator;
  $("btnInstall").disabled = !canInstall;
}

function localBinUrl(fileName) {
  return new URL(`bins/${fileName}`, window.location.href).href;
}

async function fetchReleases() {
  const sel = $("releaseSelect");
  sel.innerHTML = '<option value="">Loading…</option>';
  sel.disabled = true;
  setStatus("Loading releases…");
  releases = [];

  const res = await fetch(`https://api.github.com/repos/${REPO}/releases`, {
    headers: { Accept: "application/vnd.github+json" },
  });

  if (res.status === 404) {
    sel.innerHTML = '<option value="">Repository not accessible</option>';
    setStatus("Cannot load releases — check that the repo is public.", "err");
    updateUI();
    return;
  }

  if (!res.ok) {
    throw new Error(`GitHub API HTTP ${res.status}`);
  }

  const data = await res.json();
  for (const rel of data) {
    if (rel.draft) continue;
    const tag = rel.tag_name || rel.name;
    const asset = (rel.assets || []).find((a) => isFirmwareAsset(a.name));
    if (!asset) continue;
    releases.push({
      tag,
      name: rel.name || tag,
      fileName: asset.name,
      url: localBinUrl(asset.name),
      size: asset.size,
      prerelease: !!rel.prerelease,
    });
  }

  sel.innerHTML = "";
  if (!releases.length) {
    sel.innerHTML =
      '<option value="">No rfid-attendance-*-firmware.bin on Releases yet</option>';
    setStatus(
      "No firmware on Releases yet. Tag v*, wait for Release + Pages deploy.",
      "err"
    );
    updateUI();
    return;
  }

  for (const r of releases) {
    const opt = document.createElement("option");
    opt.value = r.tag;
    const kb = (r.size / 1024).toFixed(0);
    opt.textContent = r.prerelease
      ? `${r.tag} (pre · ${kb} KB)`
      : `${r.tag} (${kb} KB)`;
    sel.appendChild(opt);
  }
  sel.disabled = false;
  setStatus(`${releases.length} release(s) ready.`, "ok");
  log(`Loaded ${releases.length} release(s).`);
  updateUI();
}

/**
 * Reset the board and parse the USB CDC boot banner:
 *   presenca-carteirinha-ufmg <APP_VERSION_FULL> starting
 */
async function readVersionFromPort(port) {
  let reader;
  try {
    await hardResetEsp(port, MONITOR_BAUD);
    await port.open({ baudRate: MONITOR_BAUD, bufferSize: 8192 });
    await sleep(200);
    reader = port.readable.getReader();
    const dec = new TextDecoder();
    let buf = "";
    const deadline = Date.now() + 5000;
    while (Date.now() < deadline) {
      const { value, done } = await reader.read();
      if (done) break;
      buf += dec.decode(value, { stream: true });
      const m = /presenca-carteirinha-ufmg\s+(\S+)\s+starting/i.exec(buf);
      if (m) return m[1].trim();
    }
    return null;
  } finally {
    try {
      if (reader) await reader.releaseLock();
    } catch (_) {}
    try {
      await port.close();
    } catch (_) {}
  }
}

async function requestPort() {
  setStatus("Choose the USB serial port…");
  try {
    selectedPort = await navigator.serial.requestPort({
      filters: USB_PORT_FILTERS,
    });
  } catch (e) {
    if (e.name === "NotFoundError") throw e;
    selectedPort = await navigator.serial.requestPort();
  }
  return selectedPort;
}

async function readInstalledVersion() {
  if (!("serial" in navigator)) return;

  try {
    const port = await requestPort();
    setStatus("Reading boot banner…");
    deviceVersion = await readVersionFromPort(port);
    selectedPort = null;
    if (deviceVersion) {
      log(`Installed: ${deviceVersion}`);
      setStatus(`Installed firmware: ${deviceVersion}`, "ok");
    } else {
      log("No boot banner seen (wrong port, baud, or device busy).");
      setStatus("Version not read — you can still install.", "ok");
    }
    updateUI();
  } catch (e) {
    selectedPort = null;
    if (e.name !== "NotFoundError") {
      log(`Read version: ${e.message || e}`);
      setStatus(e.message || String(e), "err");
    }
    updateUI();
  }
}

async function loadEsptool() {
  if (!esptoolModule) {
    log("Loading esptool-js…");
    esptoolModule = await import(
      "https://cdn.jsdelivr.net/npm/esptool-js@0.6.0/+esm"
    );
  }
  return esptoolModule;
}

async function downloadFirmware(rel) {
  log(`Downloading ${rel.fileName}…`);
  const res = await fetch(rel.url);
  if (!res.ok) {
    throw new Error(
      `Firmware file not on this site (HTTP ${res.status}). ` +
        "Re-deploy Pages after a new Release, or wait a few minutes."
    );
  }
  return new Uint8Array(await res.arrayBuffer());
}

function portPid(port) {
  try {
    return port.getInfo()?.usbProductId || 0;
  } catch (_) {
    return 0;
  }
}

async function connectLoader(port, baud, terminal) {
  const { ESPLoader, Transport, ClassicReset, UsbJtagSerialReset } =
    await loadEsptool();

  const pid = portPid(port);
  const isUsbJtag = pid === USB_JTAG_SERIAL_PID;
  log(
    isUsbJtag
      ? "Port is Espressif USB-JTAG/Serial — using UsbJtagSerialReset (classic DTR/RTS alone usually fails)."
      : `Port PID 0x${pid.toString(16)} — trying classic + USB-JTAG reset strategies.`
  );

  const probe = await probeSerialControl(port);
  if (!probe.ok) {
    log(probe.error);
  } else {
    log("USB control signals OK.");
  }

  // Order matters for Guition ESP32-P4 (native USB 0x303a:0x1001):
  // let esptool-js run UsbJtagSerialReset first; only then ask for BOOT+RST.
  const attempts = [
    {
      label: "USB-JTAG reset",
      mode: "usb_reset",
      async prep() {
        log("esptool UsbJtagSerialReset…");
      },
    },
    {
      label: "esptool default_reset",
      mode: "default_reset",
      async prep() {
        await sleep(200);
      },
    },
    {
      label: "manual BOOT+RST",
      mode: "no_reset",
      async prep() {
        log(
          "Manual download mode: hold BOOT → tap RST → release RST → release BOOT (you have ~6 s)…"
        );
        setStatus("Hold BOOT, tap RST, then release both…", "ok");
        await sleep(6000);
      },
    },
    {
      label: "classic UART reset",
      mode: "no_reset",
      async prep() {
        log("Toggle classic DTR/RTS (UART adapters)…");
        await classicBootloaderReset(port, baud);
      },
    },
  ];

  let lastErr = null;
  for (let i = 0; i < attempts.length; i++) {
    const step = attempts[i];
    let transport;
    try {
      await step.prep();
      await ensurePortClosed(port);
      await sleep(200);

      transport = new Transport(port, false);
      const loader = new ESPLoader({
        transport,
        baudrate: baud,
        terminal,
        debugLogging: false,
        resetConstructors: {
          classicReset: (t, delay) => new ClassicReset(t, Math.max(delay, 400)),
          usbJTAGSerialReset: (t) => new UsbJtagSerialReset(t),
        },
      });
      configureLoaderBaud(loader, baud);

      log(`Connecting ${i + 1}/${attempts.length}: ${step.label}…`);
      setStatus(`Connecting (${step.label})…`);
      const chip = await withTimeout(
        loader.main(step.mode),
        CONNECT_TIMEOUT_MS,
        `Connect timed out (${step.label})`
      );
      return { loader, transport, chip };
    } catch (e) {
      lastErr = e;
      log(`Connect failed (${step.label}): ${e.message || e}`);
      try {
        if (transport) await transport.disconnect();
      } catch (_) {}
      await ensurePortClosed(port);
      await sleep(500);
    }
  }

  throw new Error(
    (lastErr?.message || "Failed to connect with the device") +
      ". Close PlatformIO Serial Monitor / other serial tools, then retry. " +
      "If it still fails: hold BOOT, tap RST, release RST, release BOOT while the installer says so."
  );
}

async function installFirmware() {
  const tag = $("releaseSelect").value;
  const rel = releases.find((r) => r.tag === tag);
  if (!rel) {
    setStatus("Select a firmware release.", "err");
    return;
  }
  if (!$("ackFlash").checked) {
    setStatus("Confirm the checkbox first.", "err");
    return;
  }

  const baud = flashBaud();
  $("btnInstall").disabled = true;
  $("btnReadVersion").disabled = true;
  setProgress(0);
  setStatus("Downloading firmware…");

  const terminal = {
    clean: () => {},
    writeLine: (d) => log(d),
    write: (d) => log(d),
  };

  try {
    const firmware = await downloadFirmware(rel);
    log(`Downloaded ${(firmware.byteLength / 1024).toFixed(0)} KB.`);

    setStatus("Select USB port and flash…");
    selectedPort = null;
    const port = await requestPort();
    log(`Port: ${usbAdapterName(port)}`);
    log(`Flashing ${rel.tag} @ ${baud} baud…`);

    setStatus("Connecting to ESP32-P4 bootloader…");
    const { loader, transport, chip } = await connectLoader(
      port,
      baud,
      terminal
    );
    log(`Chip: ${chip}`);

    setStatus("Writing flash… do not unplug USB.");
    await loader.writeFlash({
      fileArray: [{ data: firmware, address: FLASH_ADDR }],
      flashMode: "dio",
      flashFreq: "80m",
      flashSize: "16MB",
      eraseAll: false,
      compress: true,
      reportProgress: (_idx, written, total) => {
        setProgress((written / total) * 100);
      },
    });
    log("Flash written.");

    await rebootAfterFlash(port, loader, transport, baud);
    selectedPort = null;

    deviceVersion = rel.tag;
    log("Install complete. Press RST / SW3 if the screen stays blank.");
    setStatus(`Installed ${rel.tag} successfully.`, "ok");
    updateUI();
  } catch (e) {
    selectedPort = null;
    if (e.name === "NotFoundError") setStatus("No port selected.", "err");
    else {
      log(`Install failed: ${e.message || e}`);
      setStatus(`Install failed: ${e.message || e}`, "err");
    }
  } finally {
    hideProgress();
    $("btnReadVersion").disabled = false;
    updateUI();
  }
}

function wireRepoLinks() {
  const releases = $("releasesLink");
  const repo = $("repoLink");
  const deploy = $("deployLink");
  if (releases) releases.href = `https://github.com/${REPO}/releases`;
  if (repo) {
    repo.href = `https://github.com/${REPO}`;
    repo.textContent = REPO;
  }
  if (deploy) {
    deploy.href = `https://github.com/${REPO}/blob/main/docs/flasher/DEPLOY.md`;
  }
  log(`Using repository ${REPO}`);
}

function init() {
  wireRepoLinks();

  if (!("serial" in navigator)) {
    $("noSerial").classList.remove("hidden");
    $("btnInstall").disabled = true;
    $("btnReadVersion").disabled = true;
    return;
  }

  $("btnInstall").addEventListener("click", installFirmware);
  $("btnReadVersion").addEventListener("click", readInstalledVersion);
  $("releaseSelect").addEventListener("change", updateUI);
  $("ackFlash").addEventListener("change", updateUI);
  $("flashBaud").addEventListener("change", updateUI);
  fetchReleases().catch((e) => {
    log(`Releases: ${e.message}`);
    setStatus(e.message, "err");
  });
}

init();
