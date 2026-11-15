// Tests for protocol.js against a simulated badge that answers sdrecv the way
// main/lua/lua_cmd.c does (block size 2048, size and SHA-256 check).
//   node installer/test.mjs            (exit code 0 = all passed)
import { createHash } from "node:crypto";

const mod = process.env.PROTOCOL_URL || new URL("./protocol.js", import.meta.url).href;
const { planFiles, BadgeConsole, sendFile } = await import(mod);

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

console.log(`${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
