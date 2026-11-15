// Pure helpers of the installer (no DOM): which games go where on the card,
// and the badge console's sdrecv protocol. Shared by app.js and the tests.

export const CONSOLE_BAUD = 115200;
export const SYSTEMS = { gb: ".gb", gbc: ".gbc", nes: ".nes" };

// Card paths may only use letters, digits and . _ - (the badge's sdrecv rule).
export function cardName(name, ext) {
  const base = name.slice(0, name.length - ext.length)
    .replace(/[^A-Za-z0-9._-]+/g, "_")
    .replace(/\.{2,}/g, ".")
    .replace(/^[._-]+|[._-]+$/g, "")
    .slice(0, 50) || "game";
  return base;
}

export function planFiles(fileList) {
  const used = new Set();
  const out = [], skipped = [];
  for (const f of fileList) {
    const ext = (f.name.match(/\.[^.]+$/) || [""])[0].toLowerCase();
    const dirs = (f.webkitRelativePath || "").toLowerCase().split("/").slice(0, -1);
    // The folder decides (roms/gbc/...), else the file ending.
    let sys = Object.keys(SYSTEMS).find((s) => dirs.includes(s));
    if (!sys) sys = Object.keys(SYSTEMS).find((s) => SYSTEMS[s] === ext);
    const fits = sys && (sys === "nes" ? ext === ".nes" : ext === ".gb" || ext === ".gbc");
    if (!fits) {
      if (!f.name.startsWith(".")) skipped.push(f.name);
      continue;
    }
    const base = cardName(f.name, ext);
    let name = base + ext;
    for (let k = 2; used.has(sys + "/" + name); k++) name = `${base}_${k}${ext}`;
    used.add(sys + "/" + name);
    out.push({ file: f, dest: `roms/${sys}/${name}` });
  }
  return { out, skipped };
}

// The badge console over Web Serial: a text buffer and "wait for this pattern".
export class BadgeConsole {
  constructor(port) {
    this.port = port;
    this.text = "";
    this.waiters = [];
    this.closed = false;
  }

  async open() {
    await this.port.open({ baudRate: CONSOLE_BAUD, bufferSize: 65536 });
    await this.port.setSignals({ dataTerminalReady: false, requestToSend: false });  // don't hold the chip in reset
    this.reader = this.port.readable.getReader();
    this.writer = this.port.writable.getWriter();
    this.pumping = this.pump();
  }

  async pump() {
    const dec = new TextDecoder();
    try {
      for (;;) {
        const { value, done } = await this.reader.read();
        if (done) break;
        this.text += dec.decode(value, { stream: true });
        if (this.text.length > 200000) this.text = this.text.slice(-100000);
        this.wake();
      }
    } catch (e) {
      this.error = e;
    }
    this.closed = true;
    this.wake();
  }

  wake() {
    const w = this.waiters;
    this.waiters = [];
    w.forEach((f) => f());
  }

  // Resolves with the first match of re in what arrived since the last
  // expect (consumed up to the match), or null after ms.
  async expect(re, ms) {
    const end = performance.now() + ms;
    for (;;) {
      const m = re.exec(this.text);
      if (m) {
        this.text = this.text.slice(m.index + m[0].length);
        return m;
      }
      const left = end - performance.now();
      if (left <= 0 || this.closed) return null;
      await new Promise((r) => { this.waiters.push(r); setTimeout(r, Math.min(left, 250)); });
    }
  }

  write(str) {
    return this.writer.write(new TextEncoder().encode(str));
  }

  async close() {
    try { await this.reader.cancel(); } catch {}
    try { this.reader.releaseLock(); } catch {}
    try { this.writer.releaseLock(); } catch {}
    try { await this.port.close(); } catch {}
  }
}

export function toBase64(data) {
  let s = "";
  for (let i = 0; i < data.length; i += 0x8000) s += String.fromCharCode.apply(null, data.subarray(i, i + 0x8000));
  return btoa(s);
}

export async function sha256hex(data) {
  const d = new Uint8Array(await crypto.subtle.digest("SHA-256", data));
  return Array.from(d, (b) => b.toString(16).padStart(2, "0")).join("");
}

// One file with the badge's sdrecv protocol: it asks for the base64 a block at
// a time and checks size and SHA-256 at the end (main/lua/lua_cmd.c).
export async function sendFile(con, data, dest, onProgress) {
  const sha = await sha256hex(data);
  const b64 = toBase64(data);
  con.text = "";
  await con.write(`sdrecv ${dest} ${data.length} ${sha}\r\n`);
  let sent = 0;
  const re = /recv: (ready|more) (\d+)\r?\n|recv: ok \d+\r?\n|recv: error ([^\r\n]*)\r?\n|usage: sdrecv|Unrecognized command/;
  for (;;) {
    const m = await con.expect(re, 15000);
    if (!m) throw new Error("No answer from the badge. Is it switched on, and is Retro installed (step 1)?");
    if (m[1]) {
      const chunk = b64.slice(sent, sent + Number(m[2]));
      await con.write(chunk);
      sent += chunk.length;
      onProgress(sent / b64.length);
    } else if (m[0].startsWith("recv: ok")) {
      return;
    } else if (m[3] !== undefined) {
      throw new Error("The badge says: " + m[3]);
    } else {
      throw new Error("This badge can't receive files yet. Install Retro first (step 1).");
    }
  }
}

