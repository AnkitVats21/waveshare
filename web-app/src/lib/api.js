// Host configuration + REST calls.
//
// Two upstream hosts:
//  - ESP host: the Waveshare device's own native HTTP server. Used only for
//    genuinely one-shot, non-realtime operations that live on the device
//    itself: SD library scan/list/delete.
//  - Daemon host (star-replica-daemon): the control-plane gateway. Used for
//    reading a one-shot status snapshot, and as the WebSocket endpoint for
//    all live device control (see lib/ws.js).
//
// Search + stream-URL resolution hit a third host (the Invidious-compatible
// stream resolver) directly from the browser - it never touches the ESP or
// the daemon.

const DEFAULT_ESP_HOST = '192.168.1.14';
const DEFAULT_DAEMON_HOST = 'localhost:8765';
const STREAM_API_BASE = 'https://stream.ankitm.xyz/api/v1';

function cleanHost(host) {
  return host.replace(/^https?:\/\//, '').replace(/\/$/, '');
}

export function getEspHost() {
  return localStorage.getItem('esp_host') || DEFAULT_ESP_HOST;
}

export function setEspHost(host) {
  const clean = cleanHost(host);
  localStorage.setItem('esp_host', clean);
  return clean;
}

export function getEspBaseUrl() {
  return `http://${getEspHost()}`;
}

export function getDaemonHost() {
  return localStorage.getItem('daemon_host') || DEFAULT_DAEMON_HOST;
}

export function setDaemonHost(host) {
  const clean = cleanHost(host);
  localStorage.setItem('daemon_host', clean);
  return clean;
}

export function getDaemonBaseUrl() {
  return `http://${getDaemonHost()}`;
}

export function getDaemonWsUrl() {
  return `ws://${getDaemonHost()}/api/dashboard/ws`;
}

async function fetchJson(url, opts) {
  const res = await fetch(url, opts);
  if (!res.ok) throw new Error(`${res.status} ${res.statusText}`);
  return res.json();
}

// ── Search / stream resolution (one-shot, browser -> resolver directly) ────

export async function searchYouTube(query) {
  if (!query || !query.trim()) return [];
  return fetchJson(`${STREAM_API_BASE}/search?q=${encodeURIComponent(query.trim())}`);
}

export async function resolveStreamUrl(videoId) {
  const data = await fetchJson(`${STREAM_API_BASE}/videos/${encodeURIComponent(videoId)}`);
  const formats = data.adaptiveFormats || [];

  const opusFormat = formats.find((f) => f.type && f.type.includes('opus'));
  if (opusFormat && opusFormat.url) return opusFormat.url;

  const webmFormat = formats.find((f) => f.type && f.type.includes('audio/webm'));
  if (webmFormat && webmFormat.url) return webmFormat.url;

  const audioFormat = formats.find((f) => f.type && f.type.startsWith('audio/'));
  if (audioFormat && audioFormat.url) return audioFormat.url;

  if (formats.length > 0 && formats[0].url) return formats[0].url;

  throw new Error('No playable audio stream found for video');
}

// ── SD library (one-shot, browser -> ESP directly) ──────────────────────────

export async function getMusicLibrary(filter = '') {
  const query = filter ? `?q=${encodeURIComponent(filter)}` : '';
  return fetchJson(`${getEspBaseUrl()}/api/music/library${query}`);
}

export async function scanMusicLibrary() {
  return fetchJson(`${getEspBaseUrl()}/api/music/library/scan`, { method: 'POST' });
}

export async function deleteFromLibrary(id) {
  return fetchJson(`${getEspBaseUrl()}/api/music/library?id=${encodeURIComponent(id)}`, { method: 'DELETE' });
}

export async function playLocalOnEsp(idOrPath) {
  return fetchJson(`${getEspBaseUrl()}/api/music/play_local`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ id: idOrPath }),
  });
}

// ── Alert chime (one-shot, browser -> ESP directly) ─────────────────────────
//
// Deviation from brief: the daemon's WAL wire protocol (StarProtocol.h /
// WalTypes.h) has no field or command id for triggering the chime - on the
// ESP side AlertPlayer is invoked straight from its own local HTTP handler,
// not through SysDb. Routing it through the daemon's WS channel would either
// require an ESP firmware change (out of scope: only web-app + daemon are
// in play here) or silently be a no-op. Kept as a direct one-shot REST call,
// same as the SD library operations above.

export async function playAlertSound() {
  return fetchJson(`${getEspBaseUrl()}/api/audio/alert`, { method: 'POST' });
}

// ── Daemon read-only snapshot (rarely needed; WS pushes this on connect) ────

export async function getDaemonStatus() {
  return fetchJson(`${getDaemonBaseUrl()}/api/status`);
}
