// Tests for protocol.js against a simulated badge that answers sdrecv the way
// main/lua/lua_cmd.c does (block size 2048, size and SHA-256 check).
//   node installer/test.mjs            (exit code 0 = all passed)
import { createHash } from "node:crypto";

const mod = process.env.PROTOCOL_URL || new URL("./protocol.js", import.meta.url).href;
const { planFiles, BadgeConsole, sendFile, isBadgeBackup, readWholeFlash, restartBadge } = await import(mod);
const { md5 } = await import(new URL("./md5.js", import.meta.url).href);

let failed = 0, passed = 0;
function check(name, ok, detail = "") {
  if (ok) passed++;
  else { failed++; console.log(`FAIL ${name} ${detail}`); }
}

// A fake Web Serial port with a badge console behind it.
function fakeBadge({ knowsSdrecv = true, corrupt = false } = {}) {
  const enc = new TextEncoder(), dec = new TextDecoder();
  const toHost = [];
  let wake = null, rx = "", card = new Map(), job = null;
  const say = (s) => { toHost.push(enc.encode(s)); if (wake) { const w = wake; wake = null; w(); } };
  function onData(text) {
    rx += text;
    for (;;) {
      if (job) {
        const need = Math.min(2048, job.total - job.got.length);
        if (rx.replace(/\s/g, "").length < need) return;
        const clean = rx.replace(/\s/g, "");
        job.got += clean.slice(0, need);
        rx = clean.slice(need);
        if (job.got.length < job.total) { say("recv: more 2048\n"); continue; }
        let bin = Buffer.from(job.got, "base64");
        if (corrupt) bin[0] ^= 1;
        const sha = createHash("sha256").update(bin).digest("hex");
        if (bin.length !== job.bytes) say("recv: error wrong size\n");
        else if (sha !== job.sha) say("recv: error the SHA-256 doesn't match\n");
        else { card.set(job.path, bin); say(`recv: ok ${job.bytes}\n`); }
        job = null;
        continue;
      }
      const nl = rx.indexOf("\n");
      if (nl < 0) return;
      const line = rx.slice(0, nl).trim();
      rx = rx.slice(nl + 1);
      say(line + "\r\n");                   // the REPL echoes the command
      const m = line.match(/^sdrecv (\S+) (\d+) ([0-9a-f]{64})$/);
      if (!knowsSdrecv) { say("Unrecognized command\n"); continue; }
      if (!m) { say("usage: sdrecv <path> <bytes> <sha256>\n"); continue; }
      job = { path: m[1], bytes: Number(m[2]), sha: m[3], total: Math.ceil(Number(m[2]) / 3) * 4, got: "" };
      say("recv: ready 2048\n");
    }
  }
  return {
    card,
    async open() {}, async setSignals() {}, async close() {},
    readable: { getReader: () => ({
      async read() {
        while (!toHost.length) await new Promise((r) => { wake = r; });
        return { value: toHost.shift(), done: false };
      },
      async cancel() {}, releaseLock() {},
    }) },
    writable: { getWriter: () => ({ async write(b) { onData(dec.decode(b)); }, releaseLock() {} }) },
  };
}

// ---- planFiles ----
const f = (name, rel = "", size = 10) => ({ name, webkitRelativePath: rel, size });
{
  const { out, skipped } = planFiles([
    f("Game One (USA).gb", "roms/gb/Game One (USA).gb"),
    f("colour.gbc", "roms/gbc/colour.gbc"),
    f("old.gb", "roms/gbc/old.gb"),                // folder wins: a .gb in gbc/ goes to gbc
    f("tank.nes", "roms/nes/tank.nes"),
    f("wrong.gb", "roms/nes/wrong.gb"),            // a .gb can't be an NES game
    f("save.sav", "roms/gb/save.sav"),
    f(".DS_Store", "roms/.DS_Store"),
    f("loose.nes"),                                // single file: by ending
    f("a..b.gb"), f("a..b.gb"),                    // ".." removed, duplicates numbered
  ]);
  const dests = out.map((p) => p.dest);
  check("sanitized name", dests[0] === "roms/gb/Game_One_USA.gb", dests[0]);
  check("gbc folder", dests[1] === "roms/gbc/colour.gbc", dests[1]);
  check("folder wins", dests[2] === "roms/gbc/old.gb", dests[2]);
  check("nes", dests[3] === "roms/nes/tank.nes", dests[3]);
  check("by ending", dests.includes("roms/nes/loose.nes"), dests.join(","));
  check("no ..", dests.every((d) => !d.includes("..")), dests.join(","));
  check("dupes numbered", dests.includes("roms/gb/a.b.gb") && dests.includes("roms/gb/a.b_2.gb"), dests.join(","));
  check("skipped", skipped.includes("wrong.gb") && skipped.includes("save.sav") && !skipped.includes(".DS_Store"), skipped.join(","));
  check("badge path rule", dests.every((d) => /^[A-Za-z0-9._\-/]{1,120}$/.test(d)), dests.join(","));
}

// ---- planFiles with a console picked ----
{
  const { out, skipped } = planFiles([f("pkmn.gb"), f("tank.nes"), f("x.gbc", "roms/gb/x.gbc")], "gbc");
  const dests = out.map((p) => p.dest);
  check("picked console wins over the ending", dests.includes("roms/gbc/pkmn.gb"), dests.join(","));
  check("picked console wins over the folder", dests.includes("roms/gbc/x.gbc"), dests.join(","));
  check("an .nes can't go to Game Boy Color", skipped.includes("tank.nes") && dests.length === 2, skipped.join(","));
  const nes = planFiles([f("tank.nes"), f("pkmn.gb")], "nes");
  check("NES picked", nes.out.length === 1 && nes.out[0].dest === "roms/nes/tank.nes" && nes.skipped.includes("pkmn.gb"),
        nes.out.map((p) => p.dest).join(","));
  const auto = planFiles([f("pkmn.gb")], "");
  check("none picked: by ending", auto.out[0].dest === "roms/gb/pkmn.gb", auto.out[0].dest);
}

// ---- restartBadge: a real reset pulse that boots the firmware ----
{
  const calls = [];
  const transport = { async setDTR(v) { calls.push("DTR" + +v); }, async setRTS(v) { calls.push("RTS" + +v); } };
  await restartBadge(transport, async () => calls.push("wait"));
  check("reset pulse", calls.join(" ") === "DTR0 RTS1 wait RTS0", calls.join(" "));
}

// ---- sendFile ----
async function send(badgeOpts, size) {
  const port = fakeBadge(badgeOpts);
  const con = new BadgeConsole(port);
  await con.open();
  const data = new Uint8Array(size);
  for (let i = 0; i < size; i++) data[i] = (i * 131 + 7) & 255;
  let last = 0;
  try {
    await sendFile(con, data, "roms/gb/t.gb", (p) => { last = p; });
    return { ok: true, port, data, last };
  } catch (e) {
    return { ok: false, err: String(e.message || e) };
  } finally {
    await con.close();
  }
}

for (const size of [0, 1, 1535, 1536, 1537, 40976, 1048576]) {
  const r = await send({}, size);
  const got = r.ok && r.port.card.get("roms/gb/t.gb");
  check(`send ${size} B`, r.ok && got && Buffer.compare(got, Buffer.from(r.data)) === 0, r.err || "");
  if (size) check(`progress ${size} B`, r.ok && r.last === 1, String(r.last));
}
{
  const r = await send({ knowsSdrecv: false }, 100);
  check("old firmware", !r.ok && /Install Retro first/.test(r.err), r.err);
}
{
  const r = await send({ corrupt: true }, 5000);
  check("badge rejects", !r.ok && /SHA-256 doesn't match/.test(r.err), r.err);
}

// ---- backup / restore ----
{
  const size = 16 << 20;
  const good = new Uint8Array(size);
  good[0x2000] = 0xe9; good[0x8000] = 0xaa; good[0x8001] = 0x50;
  check("backup ok", isBadgeBackup(good));
  check("backup wrong size", !isBadgeBackup(good.subarray(0, size - 1)));
  const noPt = good.slice(); noPt[0x8001] = 0;
  check("backup no partition table", !isBadgeBackup(noPt));
  const noBoot = good.slice(); noBoot[0x2000] = 0xff;
  check("backup no bootloader", !isBadgeBackup(noBoot));
}

// A fake flash session: `faults` lists reads that go wrong (by call number):
// "throw" (the link breaks off) or "corrupt" (a flipped bit, so the MD5 differs).
function fakeFlash(flash, faults = {}) {
  let call = 0, last = null, reconnects = 0;
  return {
    get reconnects() { return reconnects; },
    async readFlash(addr, n, onBytes) {
      const fault = faults[++call];
      if (fault === "throw") throw new Error("Timeout");
      const data = flash.slice(addr, addr + n);
      last = md5(data);
      if (fault === "corrupt") data[n >> 1] ^= 0x40;
      for (let got = 4096; got <= n; got += 4096) onBytes(got);
      return data;
    },
    async readDigest() { return Uint8Array.from(last.match(/../g), (h) => parseInt(h, 16)); },
    async reconnect() { reconnects++; },
  };
}
{
  const size = 4 << 20, chunk = 1 << 20;
  const flash = new Uint8Array(size);
  for (let i = 0; i < size; i++) flash[i] = (i * 2654435761) >>> 24;
  const opts = { size, chunk };
  let p = 0;
  const clean = await readWholeFlash(fakeFlash(flash), md5, (x) => { p = x; }, opts);
  check("read whole flash", Buffer.compare(Buffer.from(clean), Buffer.from(flash)) === 0);
  check("read progress", p === 1, String(p));
  const s2 = fakeFlash(flash, { 2: "throw", 4: "corrupt" });
  let retried = null, rerr = "";
  try { retried = await readWholeFlash(s2, md5, () => {}, opts); } catch (e) { rerr = String(e.message); }
  check("read retries", retried && Buffer.compare(Buffer.from(retried), Buffer.from(flash)) === 0 && s2.reconnects === 2,
        rerr || String(s2.reconnects));
  let err = "";
  try { await readWholeFlash(fakeFlash(flash, { 1: "corrupt", 2: "corrupt", 3: "corrupt" }), md5, () => {}, opts); }
  catch (e) { err = String(e.message); }
  check("read gives up", /failed 3 times.*checksum mismatch/.test(err), err);
}

console.log(`${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
