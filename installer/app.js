// Retro installer: puts the Retro firmware on a Solana OS badge and copies
// games onto its microSD card, from the browser (Web Serial, Chrome or Edge).
//
// Install writes exactly what `idf.py flash` writes (bootloader, partition
// table, otadata, app) and never erases: Wi-Fi, badge ID, keys and store apps
// stay. Copy games speaks the badge console's `sdrecv` protocol, the same as
// tools/sd_put.py.

import { ESPLoader, Transport } from "https://cdn.jsdelivr.net/npm/esptool-js@0.7.0/+esm";
import { md5 } from "./md5.js";
import { planFiles, BadgeConsole, sendFile } from "./protocol.js";

const CH340 = { usbVendorId: 0x1a86 };      // the badge's USB-serial chip
const FLASH_BAUD = 230400;                  // faster rates drop bytes on the CH340 (measured)

const $ = (id) => document.getElementById(id);
let busy = false;

function log(text) {
  const el = $("log");
  el.textContent += text + "\n";
  el.scrollTop = el.scrollHeight;
}

function setStatus(id, text, kind = "") {
  const el = $(id);
  el.textContent = text;
  el.dataset.kind = kind;
}

function setProgress(id, fraction) {
  const bar = $(id);
  bar.hidden = fraction === null;
  if (fraction !== null) bar.value = Math.round(fraction * 1000);
}

function setBusy(on) {
  busy = on;
  for (const b of document.querySelectorAll("button, input[type=file]")) b.disabled = on || !("serial" in navigator);
  $("copy").disabled = on || !planned.length || !("serial" in navigator);
}

// ---- Step 1: install ----

async function install() {
  setBusy(true);
  setStatus("install-status", "Loading the firmware…");
  setProgress("install-progress", null);
  let transport;
  try {
    const fw = await (await fetch("firmware/firmware.json", { cache: "no-store" })).json();
    const files = [];
    for (const p of fw.parts) {
      const res = await fetch("firmware/" + p.path, { cache: "no-store" });
      if (!res.ok) throw new Error(`Couldn't load ${p.path} (${res.status})`);
      files.push({ address: p.offset, data: new Uint8Array(await res.arrayBuffer()) });
    }
    setStatus("install-status", "Pick your badge in the list (USB-SERIAL CH340)…");
    const port = await navigator.serial.requestPort({ filters: [CH340] });
    transport = new Transport(port, false);
    const loader = new ESPLoader({
      transport,
      baudrate: FLASH_BAUD,
      romBaudrate: 115200,
      terminal: { clean() {}, writeLine: log, write: (s) => log(s) },
    });
    setStatus("install-status", "Connecting… (the badge screen goes dark, that's normal)");
    const chip = await loader.main();
    if (!/ESP32-C5/i.test(chip)) throw new Error(`That's a ${chip}, not the badge's ESP32-C5. Nothing was written.`);
    const total = files.reduce((n, f) => n + f.data.length, 0);
    const before = files.map((f, i) => files.slice(0, i).reduce((n, g) => n + g.data.length, 0));
    setStatus("install-status", "Writing Retro to the badge… keep it plugged in");
    setProgress("install-progress", 0);
    await loader.writeFlash({
      fileArray: files,
      flashMode: "keep",
      flashFreq: "keep",
      flashSize: "keep",
      eraseAll: false,                      // never: it would wipe the badge's ID, keys and apps
      compress: true,
      reportProgress: (i, written) => setProgress("install-progress", (before[i] + written) / total),
      calculateMD5Hash: md5,                // the chip checks every part against this
    });
    await loader.after("hard_reset");
    setProgress("install-progress", 1);
    setStatus("install-status", `Done: Retro ${fw.version} is on your badge. It's restarting now.`, "ok");
  } catch (e) {
    const cancelled = e && e.name === "NotFoundError";
    setStatus("install-status", cancelled ? "No badge picked." : "Install failed: " + (e.message || e), cancelled ? "" : "error");
    if (!cancelled) log(String(e && e.stack || e));
  } finally {
    if (transport) await transport.disconnect().catch(() => {});
    setBusy(false);
  }
}

// ---- Step 2: copy games ----

let planned = [];

function showPlan(skipped) {
  const list = $("plan");
  list.replaceChildren(...planned.map((p) => {
    const li = document.createElement("li");
    li.textContent = `${p.file.name}  →  ${p.dest}  (${Math.ceil(p.file.size / 1024)} KB)`;
    return li;
  }));
  const kb = planned.reduce((n, p) => n + p.file.size, 0) / 1024;
  const minutes = Math.max(1, Math.round(kb / 8 / 60));
  setStatus("copy-status", planned.length
    ? `${planned.length} game${planned.length === 1 ? "" : "s"}, ${Math.ceil(kb)} KB, about ${minutes} min.` +
      (skipped.length ? ` Skipped (not .gb, .gbc or .nes): ${skipped.join(", ")}` : "")
    : "No .gb, .gbc or .nes files found there.", planned.length ? "" : "error");
  $("copy").disabled = busy || !planned.length;
}

function pick(input) {
  const { out, skipped } = planFiles(Array.from(input.files));
  planned = out;
  showPlan(skipped);
  input.value = "";
}

async function copyGames() {
  if (!planned.length) return;
  setBusy(true);
  setProgress("copy-progress", null);
  let con;
  try {
    setStatus("copy-status", "Pick your badge in the list (USB-SERIAL CH340)…");
    const port = await navigator.serial.requestPort({ filters: [CH340] });
    con = new BadgeConsole(port);
    await con.open();
    // Opening the port can restart the badge: if it did, wait for its console.
    setStatus("copy-status", "Connecting…");
    if (await con.expect(/ESP-ROM|rst:/, 1500)) {
      setStatus("copy-status", "The badge restarted, waiting for it…");
      await con.expect(/console ready/, 20000);
      await new Promise((r) => setTimeout(r, 1500));
    }
    const total = planned.reduce((n, p) => n + p.file.size, 0);
    let done = 0;
    for (const [i, p] of planned.entries()) {
      setStatus("copy-status", `Copying ${i + 1} of ${planned.length}: ${p.dest}`);
      const data = new Uint8Array(await p.file.arrayBuffer());
      await sendFile(con, data, p.dest, (f) => setProgress("copy-progress", (done + f * data.length) / total));
      done += data.length;
      log(`copied ${p.dest} (${data.length} bytes, SHA-256 checked)`);
    }
    setProgress("copy-progress", 1);
    setStatus("copy-status", `Done: ${planned.length} game${planned.length === 1 ? "" : "s"} on the card. On the badge: Retro → console → game.`, "ok");
  } catch (e) {
    const cancelled = e && e.name === "NotFoundError";
    const busyPort = e && e.name === "InvalidStateError";
    setStatus("copy-status", cancelled ? "No badge picked." : busyPort
      ? "The badge's port is in use by another program or tab. Close it and try again."
      : "Copy failed: " + (e.message || e), cancelled ? "" : "error");
    if (!cancelled) log(String(e && e.stack || e));
  } finally {
    if (con) await con.close();
    setBusy(false);
  }
}

// ---- Wire up ----

if (!("serial" in navigator)) {
  $("unsupported").hidden = false;
}
$("install").addEventListener("click", install);
$("copy").addEventListener("click", copyGames);
$("folder").addEventListener("change", (e) => pick(e.target));
$("files").addEventListener("change", (e) => pick(e.target));
setBusy(false);
fetch("firmware/firmware.json", { cache: "no-store" })
  .then((r) => r.json())
  .then((fw) => { $("version").textContent = `Retro ${fw.version}, built ${fw.built}`; })
  .catch(() => { $("version").textContent = "Firmware files missing on this page."; });
