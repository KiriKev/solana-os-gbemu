// Retro installer: puts the Retro firmware on a Solana OS badge and copies
// games onto its microSD card, from the browser (Web Serial, Chrome or Edge).
//
// Install writes exactly what `idf.py flash` writes (bootloader, partition
// table, otadata, app) and never erases: Wi-Fi, badge ID, keys and store apps
// stay. Copy games speaks the badge console's `sdrecv` protocol, the same as
// tools/sd_put.py.

import { ESPLoader, Transport } from "https://cdn.jsdelivr.net/npm/esptool-js@0.7.0/+esm";
import { md5 } from "./md5.js";
import { planFiles, BadgeConsole, sendFile, isBadgeBackup, readWholeFlash, restartBadge } from "./protocol.js";

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

// The chip's ROM loader on `port`: checks it's the badge's ESP32-C5.
async function connect(port) {
  const transport = new Transport(port, false);
  const loader = new ESPLoader({
    transport,
    baudrate: FLASH_BAUD,
    romBaudrate: 115200,
    terminal: { clean() {}, writeLine: log, write: (s) => log(s) },
  });
  try {
    const chip = await loader.main();
    if (!/ESP32-C5/i.test(chip)) throw new Error(`That's a ${chip}, not the badge's ESP32-C5. Nothing was changed.`);
  } catch (e) {
    await transport.disconnect().catch(() => {});
    throw e;
  }
  return { transport, loader };
}

function failed(statusId, what, e) {
  const cancelled = e && e.name === "NotFoundError";
  setStatus(statusId, cancelled ? "No badge picked." : `${what} failed: ` + (e.message || e), cancelled ? "" : "error");
  if (!cancelled) log(String(e && e.stack || e));
}

// ---- Back up the whole badge ----

async function backup() {
  setBusy(true);
  setProgress("backup-progress", null);
  let link;
  try {
    setStatus("backup-status", "Pick your badge in the list (USB-SERIAL CH340)…");
    const port = await navigator.serial.requestPort({ filters: [CH340] });
    setStatus("backup-status", "Connecting… (the badge screen goes dark, that's normal)");
    link = await connect(port);
    setStatus("backup-status", "Reading all 16 MB… about 10-15 minutes, keep it plugged in");
    setProgress("backup-progress", 0);
    const session = {
      readFlash: (addr, n, onBytes) => link.loader.readFlash(addr, n, (_, got) => onBytes(got)),
      readDigest: () => link.loader.transport.read(3000),     // the MD5 the chip sends after each read
      reconnect: async () => {
        log("read broke off, reconnecting…");
        await link.transport.disconnect().catch(() => {});
        link = await connect(port);
      },
    };
    const image = await readWholeFlash(session, md5, (f) => setProgress("backup-progress", f));
    if (!isBadgeBackup(image)) throw new Error("What came back doesn't look like a badge (no bootloader or partition table).");
    await restartBadge(link.transport);
    const a = document.createElement("a");
    a.href = URL.createObjectURL(new Blob([image], { type: "application/octet-stream" }));
    a.download = `badge-backup-${new Date().toISOString().slice(0, 10)}.bin`;
    document.body.append(a);
    a.click();
    a.remove();
    setStatus("backup-status", `Done: ${a.download} (16 MB) is in your downloads. Keep it private: it holds your badge's identity and keys.`, "ok");
  } catch (e) {
    failed("backup-status", "Backup", e);
  } finally {
    if (link) await link.transport.disconnect().catch(() => {});
    setBusy(false);
  }
}

// ---- Restore a backup ----

async function restore(input) {
  const file = input.files[0];
  input.value = "";
  if (!file) return;
  const image = new Uint8Array(await file.arrayBuffer());
  if (!isBadgeBackup(image)) {
    setStatus("restore-status", `${file.name} isn't a badge backup (it must be the 16 MB file from "Back up badge").`, "error");
    return;
  }
  if (!confirm(`Put the badge back exactly as it was in ${file.name}?\n\nEverything on the badge is replaced by the backup: the operating system, apps, settings and badge identity. Games on the microSD card stay.`)) {
    return;
  }
  setBusy(true);
  setProgress("restore-progress", null);
  let link;
  try {
    setStatus("restore-status", "Pick your badge in the list (USB-SERIAL CH340)…");
    const port = await navigator.serial.requestPort({ filters: [CH340] });
    setStatus("restore-status", "Connecting… (the badge screen goes dark, that's normal)");
    link = await connect(port);
    setStatus("restore-status", "Writing the backup… a few minutes, keep it plugged in");
    setProgress("restore-progress", 0);
    await link.loader.writeFlash({
      fileArray: [{ address: 0, data: image }],
      flashMode: "keep",
      flashFreq: "keep",
      flashSize: "keep",
      eraseAll: false,
      compress: true,
      reportProgress: (_, written) => setProgress("restore-progress", written / image.length),
      calculateMD5Hash: md5,
    });
    await restartBadge(link.transport);
    setProgress("restore-progress", 1);
    setStatus("restore-status", "Done: the badge is back to how it was in the backup. It's restarting now.", "ok");
  } catch (e) {
    failed("restore-status", "Restore", e);
  } finally {
    if (link) await link.transport.disconnect().catch(() => {});
    setBusy(false);
  }
}

// ---- Install ----

async function install() {
  setBusy(true);
  setStatus("install-status", "Loading the firmware…");
  setProgress("install-progress", null);
  let link;
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
    setStatus("install-status", "Connecting… (the badge screen goes dark, that's normal)");
    link = await connect(port);
    const { loader } = link;
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
    await restartBadge(link.transport);
    setProgress("install-progress", 1);
    setStatus("install-status", `Done: Retro ${fw.version} is on your badge. It's restarting now.`, "ok");
  } catch (e) {
    failed("install-status", "Install", e);
  } finally {
    if (link) await link.transport.disconnect().catch(() => {});
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

// The console picked for "Choose game files"; "" = by file ending.
const chosenConsole = () => document.querySelector("input[name=console]:checked").value;
let picked = null;                          // the files of the last "Choose game files", to sort again

function pick(input) {
  const files = Array.from(input.files);
  picked = input.id === "files" ? files : null;
  const { out, skipped } = planFiles(files, picked ? chosenConsole() : null);
  planned = out;
  showPlan(skipped);
  input.value = "";
}

function sortAgain() {
  if (picked) {
    const { out, skipped } = planFiles(picked, chosenConsole());
    planned = out;
    showPlan(skipped);
  }
  const c = chosenConsole();
  $("files").accept = c === "nes" ? ".nes" : c ? ".gb,.gbc" : ".gb,.gbc,.nes";
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
$("backup").addEventListener("click", backup);
$("restore-file").addEventListener("change", (e) => restore(e.target));
$("install").addEventListener("click", install);
$("copy").addEventListener("click", copyGames);
$("folder").addEventListener("change", (e) => pick(e.target));
$("files").addEventListener("change", (e) => pick(e.target));
for (const r of document.querySelectorAll("input[name=console]")) r.addEventListener("change", sortAgain);
setBusy(false);
fetch("firmware/firmware.json", { cache: "no-store" })
  .then((r) => r.json())
  .then((fw) => { $("version").textContent = `Retro ${fw.version}, built ${fw.built}`; })
  .catch(() => { $("version").textContent = "Firmware files missing on this page."; });
