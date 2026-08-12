// strata demo: UI orchestration. The engine lives in engine-worker.js; every
// call crosses one postMessage boundary and returns a promise. The engine is
// the real library (wasm, pthreads); the crash gate is the same byte-offset
// fault injection tools/crash_test uses.

const OPT = [512, 512, 4, 0]; // 512KB memtable, 512KB tables, L0 trigger 4, fsync=kAlways
const VALUE_SIZE = 100;

let worker = null;
let seq = 0;
const pending = new Map();
let acked = 0;        // records [0, acked) acknowledged by the engine
let armedAt = -1;     // absolute byte offset of the armed power cut
let railScale = 2 * 1024 * 1024;
let dead = false;
let pollTimer = null;

const $ = (id) => document.getElementById(id);
const fmt = (n) => Number(n).toLocaleString('en-US');
const BTNS = ['load-seq', 'load-conc', 'do-flush', 'do-compact', 'crash', 'kv-put', 'kv-get', 'kv-del', 'kv-key', 'kv-val'];

function call(fn, ...args) {
  return new Promise((resolve, reject) => {
    const id = seq++;
    pending.set(id, { resolve, reject });
    worker.postMessage({ id, fn, args });
  });
}

function setBusy(b) {
  for (const id of BTNS) $(id).disabled = b || dead;
}

function logLine(html, cls = '') {
  const c = $('console');
  const line = document.createElement('div');
  if (cls) line.className = cls;
  line.innerHTML = html;
  c.appendChild(line);
  c.scrollTop = c.scrollHeight;
  while (c.childNodes.length > 60) c.removeChild(c.firstChild);
}

// ---------- stats / rail / tree ----------

async function refresh() {
  try {
    const st = JSON.parse(await call('stats'));
    renderRail(st.bytes_written);
    renderTree(st.files || []);
    renderCounters(st);
  } catch (_) { /* worker busy or gone; next tick */ }
}

function renderRail(bytes) {
  while (bytes > railScale * 0.92) railScale *= 2;
  $('rail-bytes').textContent = fmt(bytes);
  $('rail-fill').style.width = (100 * bytes / railScale).toFixed(2) + '%';
  const gate = $('rail-gate');
  if (armedAt >= 0) {
    gate.hidden = false;
    gate.style.left = (100 * armedAt / railScale).toFixed(2) + '%';
  } else {
    gate.hidden = true;
  }
}

function renderTree(files) {
  const wal = [], sst = [], meta = [];
  for (const f of files) {
    const name = f.name.split('/').pop();
    if (name.endsWith('.log') || name.endsWith('.wal')) wal.push({ name, size: f.size });
    else if (name.endsWith('.sst')) sst.push({ name, size: f.size });
    else meta.push({ name, size: f.size });
  }
  sst.sort((a, b) => a.name < b.name ? -1 : 1);

  const row = (label, items, cls, empty) => {
    const blocks = items.length === 0
      ? `<span class="tree-empty">${empty}</span>`
      : items.map((f) => {
          const w = Math.max(8, Math.round(Math.sqrt(f.size) / 4));
          const label = w > 58 ? `<span>${f.name.replace(/^0+/, '') || '0'}</span>` : '';
          return `<div class="fblock ${cls}" style="width:${w}px" title="${f.name}: ${fmt(f.size)} bytes">${label}</div>`;
        }).join('');
    return `<div class="tree-row"><div class="tree-label">${label}</div><div class="tree-files">${blocks}</div></div>`;
  };

  $('tree').innerHTML =
    row('wal', wal, 'wal', 'empty: everything flushed') +
    row('sstables', sst, 'sst', 'none yet: write something, then flush') +
    row('manifest', meta, 'meta', '–');
}

function renderCounters(st) {
  const pct = (a, b) => (a + b > 0 ? (100 * a / (a + b)).toFixed(1) + '%' : '–');
  $('t-wamp').textContent = st.user_bytes_written > 0 ? (+st.write_amplification).toFixed(2) + '×' : '–';
  $('t-flush').textContent = fmt(st.flush_count ?? 0);
  $('t-comp').textContent = fmt(st.compaction_count ?? 0);
  $('t-stall').textContent = fmt(Math.round((st.write_stall_micros ?? 0) / 1000)) + 'ms';
  $('t-bloom').textContent = pct(st.bloom_skips ?? 0, (st.bloom_checks ?? 0) - (st.bloom_skips ?? 0));
  $('t-cache').textContent = pct(st.block_cache_hits ?? 0, st.block_cache_misses ?? 0);
}

function noteAcked() {
  $('acked').textContent = fmt(acked);
}

// ---------- boot (automatic) ----------

async function boot() {
  const led = $('led-label');
  if (!window.crossOriginIsolated) {
    // coi-serviceworker reloads once to enable isolation; if we are still not
    // isolated, the reload is coming (or the browser can't do it at all).
    led.textContent = 'enabling cross-origin isolation…';
    setTimeout(() => {
      if (!window.crossOriginIsolated) led.textContent = 'this browser cannot enable cross-origin isolation; the engine needs it for threads';
    }, 4000);
    return;
  }

  worker = new Worker('engine-worker.js');
  worker.onmessage = (e) => {
    const p = pending.get(e.data.id);
    if (!p) return;
    pending.delete(e.data.id);
    e.data.error ? p.reject(new Error(e.data.error)) : p.resolve(e.data.ret);
  };

  const rc = await call('open', ...OPT);
  if (rc !== 0) {
    led.textContent = 'boot failed: ' + await call('lastError');
    return;
  }
  led.textContent = 'engine live · wasm · fsync on every ack';
  setBusy(false);
  pollTimer = setInterval(refresh, 600);
  refresh();
}
boot();

// ---------- console ----------

$('kv-put').addEventListener('click', async () => {
  const k = $('kv-key').value, v = $('kv-val').value;
  if (!k) return;
  const rc = await call('put', k, v);
  rc === 0
    ? logLine(`put <span class="val">${k}</span> = <span class="val">${v}</span> <span class="ok">ok, synced</span>`)
    : logLine(`put ${k}: <span class="err">${await call('lastError')}</span>`);
});

$('kv-get').addEventListener('click', async () => {
  const k = $('kv-key').value;
  if (!k) return;
  const v = await call('get', k);
  logLine(v
    ? `get <span class="val">${k}</span> → <span class="val">${v}</span>`
    : `get <span class="val">${k}</span> → <span class="err">(not found)</span>`);
});

$('kv-del').addEventListener('click', async () => {
  const k = $('kv-key').value;
  if (!k) return;
  const rc = await call('del', k);
  rc === 0
    ? logLine(`delete <span class="val">${k}</span> <span class="ok">tombstone written</span>`)
    : logLine(`delete ${k}: <span class="err">${await call('lastError')}</span>`);
});

// ---------- workloads ----------

$('load-seq').addEventListener('click', async () => {
  setBusy(true);
  const chunks = 10, per = 2000;
  let micros = 0, failed = false;
  for (let i = 0; i < chunks; i++) {
    const us = await call('fill', acked, per, VALUE_SIZE);
    if (us < 0) { failed = true; break; }
    micros += us;
    acked += per;
    noteAcked();
    await refresh();
  }
  $('ops').innerHTML = failed
    ? `<span class="err">write failed: ${await call('lastError')}</span>`
    : `${fmt(Math.round(chunks * per / (micros / 1e6)))} synced writes/s · 1 writer · in-memory FS`;
  setBusy(false);
});

$('load-conc').addEventListener('click', async () => {
  setBusy(true);
  const us = await call('fillConcurrent', acked, 4, 5000, VALUE_SIZE);
  if (us < 0) {
    $('ops').innerHTML = `<span class="err">write failed: ${await call('lastError')}</span>`;
  } else {
    acked += 20000;
    noteAcked();
    $('ops').innerHTML = `${fmt(Math.round(20000 / (us / 1e6)))} synced writes/s · 4 threads, one WAL, group commit`;
  }
  await refresh();
  setBusy(false);
});

$('do-flush').addEventListener('click', async () => {
  setBusy(true);
  await call('flush');
  await refresh();
  setBusy(false);
});

$('do-compact').addEventListener('click', async () => {
  setBusy(true);
  await call('compactAll');
  await refresh();
  setBusy(false);
});

// ---------- crash lab ----------

$('crash').addEventListener('click', async () => {
  setBusy(true);
  const verdict = $('verdict');
  verdict.hidden = false;
  verdict.className = 'verdict';

  const st = JSON.parse(await call('stats'));
  const delta = 300_000 + Math.floor(Math.random() * 600_000);
  armedAt = st.bytes_written + delta;
  await call('crashIn', delta);
  renderRail(st.bytes_written);
  verdict.textContent = `Gate armed ${fmt(delta)} bytes ahead (absolute byte ${fmt(armedAt)}); writing toward it…`;

  // Write toward the gate until a write tears.
  let lastError = '';
  for (let guard = 0; guard < 4000; guard++) {
    const us = await call('fill', acked, 250, VALUE_SIZE);
    if (us < 0) { lastError = await call('lastError'); break; }
    acked += 250;
    if (guard % 4 === 0) { noteAcked(); refresh(); }
  }

  // The failing batch names the exact record the tear interrupted; everything
  // before it was individually acknowledged.
  const m = lastError.match(/at index (\d+)/);
  if (m) acked = Math.max(acked, parseInt(m[1], 10));
  noteAcked();

  dead = true;
  document.body.classList.add('dead');
  $('rail').classList.add('torn');
  $('led-label').textContent = 'dead: torn write mid-WAL';
  setBusy(true);
  $('crash').hidden = true;
  const rb = $('reboot');
  rb.hidden = false;
  rb.disabled = false;

  verdict.className = 'verdict bad';
  verdict.innerHTML =
    `<b>Power cut at byte ${fmt(armedAt)}.</b> Engine error: ` +
    `${lastError.replace(/ \(at index \d+\)/, '')}. ` +
    `${fmt(acked)} records were acknowledged before the lights went out; every one of them is now a promise.`;
});

$('reboot').addEventListener('click', async () => {
  const verdict = $('verdict');
  verdict.className = 'verdict';
  verdict.textContent = 'Rebooting: WAL replay, torn-tail truncation, manifest recovery…';
  $('reboot').disabled = true;

  const rc = await call('recover', ...OPT);
  if (rc !== 0) {
    verdict.className = 'verdict bad';
    verdict.textContent = 'Reopen failed: ' + await call('lastError');
    $('reboot').disabled = false;
    return;
  }

  const t0 = performance.now();
  const missing = await call('verify', acked, VALUE_SIZE);
  const ms = (performance.now() - t0).toFixed(0);

  dead = false;
  document.body.classList.remove('dead');
  $('rail').classList.remove('torn');
  armedAt = -1;
  $('led-label').textContent = 'engine live · recovered';
  $('reboot').hidden = true;
  $('crash').hidden = false;
  setBusy(false);
  refresh();

  if (missing === -1) {
    verdict.className = 'verdict ok';
    verdict.innerHTML =
      `<b>Recovery complete.</b> Read back all ${fmt(acked)} acknowledged records in ${ms}ms: ` +
      `every key present, every value byte-identical. The torn write was cut from the WAL tail ` +
      `during replay, exactly as designed. Run it again: the gate lands somewhere new every time.`;
  } else {
    verdict.className = 'verdict bad';
    verdict.innerHTML =
      `<b>RECOVERY FAILED:</b> record ${fmt(missing)} of ${fmt(acked)} is missing or corrupt. ` +
      `This would be a real durability bug; please open an issue with this seed.`;
  }
});
