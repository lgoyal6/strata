/* Minimal COOP/COEP service worker so SharedArrayBuffer (wasm pthreads) works
   on hosts that can't set headers, like GitHub Pages. On first visit it
   registers and reloads once; after that every response is served with
   cross-origin isolation headers. */

if (typeof window === 'undefined') {
  // ----- service worker scope -----
  self.addEventListener('install', () => self.skipWaiting());
  self.addEventListener('activate', (e) => e.waitUntil(self.clients.claim()));
  self.addEventListener('fetch', (e) => {
    if (e.request.cache === 'only-if-cached' && e.request.mode !== 'same-origin') return;
    e.respondWith(
      fetch(e.request).then((res) => {
        if (res.status === 0) return res;
        const headers = new Headers(res.headers);
        headers.set('Cross-Origin-Embedder-Policy', 'require-corp');
        headers.set('Cross-Origin-Opener-Policy', 'same-origin');
        headers.set('Cross-Origin-Resource-Policy', 'cross-origin');
        return new Response(res.body, { status: res.status, statusText: res.statusText, headers });
      })
    );
  });
} else {
  // ----- page scope -----
  (async () => {
    if (window.crossOriginIsolated) return;
    if (!('serviceWorker' in navigator)) {
      console.warn('strata demo: no service worker support; wasm threads unavailable');
      return;
    }
    const reg = await navigator.serviceWorker.register(document.currentScript.src);
    await navigator.serviceWorker.ready;
    // Reload once so the page itself is served through the worker.
    if (!sessionStorage.getItem('coi-reloaded')) {
      sessionStorage.setItem('coi-reloaded', '1');
      location.reload();
    }
  })();
}
