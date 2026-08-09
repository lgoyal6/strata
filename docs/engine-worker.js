// Engine worker: hosts the WASM module off the UI thread.
//
// Pthread note: Emscripten spawns each engine thread as a worker running THIS
// script again (it reuses self.location), with name "em-pthread". In that
// mode strata.js self-instantiates and runs the pthread protocol; our only
// job is to load it and stay silent — no second instantiation, no onmessage.
importScripts('demo/strata.js');

if (globalThis.name !== 'em-pthread') {
  let api = null;

  const ready = StrataModule({
    locateFile: (file) => 'demo/' + file,
  }).then((M) => {
    const c = (name, ret, args) => M.cwrap(name, ret, args);
    api = {
      open: c('sw_open', 'number', ['number', 'number', 'number', 'number']),
      close: c('sw_close', 'number', []),
      put: c('sw_put', 'number', ['string', 'string']),
      del: c('sw_delete', 'number', ['string']),
      get: c('sw_get', 'string', ['string']),
      fill: c('sw_fill', 'number', ['number', 'number', 'number']),
      fillConcurrent: c('sw_fill_concurrent', 'number', ['number', 'number', 'number', 'number']),
      verify: c('sw_verify', 'number', ['number', 'number']),
      flush: c('sw_flush', 'number', []),
      compactAll: c('sw_compact_all', 'number', []),
      stats: c('sw_stats', 'string', []),
      crashIn: c('sw_crash_in', null, ['number']),
      isDead: c('sw_is_dead', 'number', []),
      recover: c('sw_recover', 'number', ['number', 'number', 'number', 'number']),
      wipe: c('sw_wipe', 'number', []),
      lastError: c('sw_last_error', 'string', []),
    };
  });

  onmessage = async (e) => {
    const { id, fn, args } = e.data;
    await ready;
    try {
      let ret = api[fn](...(args || []));
      if (typeof ret === 'bigint') ret = Number(ret);
      postMessage({ id, ret });
    } catch (err) {
      postMessage({ id, error: String(err) });
    }
  };
}
