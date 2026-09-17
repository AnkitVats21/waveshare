const DEFAULT_ESP_HOST = '192.168.1.14';
const STREAM_API_BASE = 'https://stream.ankitm.xyz/api/v1';

export function getEspHost() {
  return localStorage.getItem('esp_host') || DEFAULT_ESP_HOST;
}

export function setEspHost(host) {
  const clean = host.replace(/^https?:\/\//, '').replace(/\/$/, '');
  localStorage.setItem('esp_host', clean);
  return clean;
}

export function getEspBaseUrl() {
  const host = getEspHost();
  return `http://${host}`;
}

// ─────────────────────────────────────────────────────────────
// Online Stream & Invidious / YouTube Search
// ─────────────────────────────────────────────────────────────

export async function searchYouTube(query) {
  if (!query || !query.trim()) return [];
  const res = await fetch(`${STREAM_API_BASE}/search?q=${encodeURIComponent(query.trim())}`);
  if (!res.ok) throw new Error(`Search failed: ${res.status} ${res.statusText}`);
  return await res.json();
}

export async function resolveStreamUrl(videoId) {
  const res = await fetch(`${STREAM_API_BASE}/videos/${encodeURIComponent(videoId)}`);
  if (!res.ok) throw new Error(`Video resolve failed: ${res.status}`);
  const data = await res.json();
  const formats = data.adaptiveFormats || [];

  // 1. Prefer opus / webm audio format
  const opusFormat = formats.find(f => f.type && f.type.includes('opus'));
  if (opusFormat && opusFormat.url) return opusFormat.url;

  // 2. Audio/webm format
  const webmFormat = formats.find(f => f.type && f.type.includes('audio/webm'));
  if (webmFormat && webmFormat.url) return webmFormat.url;

  // 3. Fallback to any audio container
  const audioFormat = formats.find(f => f.type && f.type.startsWith('audio/'));
  if (audioFormat && audioFormat.url) return audioFormat.url;

  // 4. Any format with url
  if (formats.length > 0 && formats[0].url) return formats[0].url;

  throw new Error('No playable audio stream found for video');
}

// ─────────────────────────────────────────────────────────────
// ESP32 Music Control APIs
// ─────────────────────────────────────────────────────────────

export async function playDirectOnEsp(track, streamUrl) {
  const url = `${getEspBaseUrl()}/api/music/play`;
  const res = await fetch(url, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({
      stream_url: streamUrl,
      id: track.videoId || track.id || '',
      title: track.title || 'Unknown',
      artist: track.author || track.artist || 'Unknown',
      duration: track.lengthSeconds || track.durationSeconds || 0,
    }),
  });
  if (!res.ok) throw new Error(`Play failed: ${res.status}`);
  return await res.json();
}

export async function playLocalOnEsp(idOrPath) {
  const url = `${getEspBaseUrl()}/api/music/play_local`;
  const res = await fetch(url, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ id: idOrPath }),
  });
  if (!res.ok) throw new Error(`Play local failed: ${res.status}`);
  return await res.json();
}

export async function controlPlayback(action, value = null) {
  const url = `${getEspBaseUrl()}/api/music/control`;
  const payload = { action };
  if (value !== null) payload.value = value;

  const res = await fetch(url, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(payload),
  });
  if (!res.ok) throw new Error(`Control failed: ${res.status}`);
  return await res.json();
}

export async function getMusicStatus() {
  const url = `${getEspBaseUrl()}/api/music/status`;
  const res = await fetch(url);
  if (!res.ok) throw new Error(`Status failed: ${res.status}`);
  return await res.json();
}

export async function getMusicLibrary(filter = '') {
  const query = filter ? `?q=${encodeURIComponent(filter)}` : '';
  const url = `${getEspBaseUrl()}/api/music/library${query}`;
  const res = await fetch(url);
  if (!res.ok) throw new Error(`Library failed: ${res.status}`);
  return await res.json();
}

export async function scanMusicLibrary() {
  const url = `${getEspBaseUrl()}/api/music/library/scan`;
  const res = await fetch(url, { method: 'POST' });
  if (!res.ok) throw new Error(`Scan failed: ${res.status}`);
  return await res.json();
}

export async function deleteFromLibrary(id) {
  const url = `${getEspBaseUrl()}/api/music/library?id=${encodeURIComponent(id)}`;
  const res = await fetch(url, { method: 'DELETE' });
  if (!res.ok) throw new Error(`Delete failed: ${res.status}`);
  return await res.json();
}

// ─────────────────────────────────────────────────────────────
// Hardware Remote & Telemetry APIs
// ─────────────────────────────────────────────────────────────

export async function getSystemDelta(logSeq = 0) {
  const url = `${getEspBaseUrl()}/api/system/delta?log_seq=${logSeq}`;
  const res = await fetch(url);
  if (!res.ok) throw new Error(`Delta failed: ${res.status}`);
  return await res.json();
}

export async function setSpeakerVolume(volume) {
  const url = `${getEspBaseUrl()}/api/audio/volume`;
  const res = await fetch(url, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ volume }),
  });
  if (!res.ok) throw new Error(`Volume failed: ${res.status}`);
  return await res.json();
}

export async function setMicGain(gain) {
  const url = `${getEspBaseUrl()}/api/audio/mic_gain`;
  const res = await fetch(url, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ gain }),
  });
  if (!res.ok) throw new Error(`Mic gain failed: ${res.status}`);
  return await res.json();
}

export async function setMicMute(muted) {
  const url = `${getEspBaseUrl()}/api/audio/mic_mute`;
  const res = await fetch(url, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ muted }),
  });
  if (!res.ok) throw new Error(`Mic mute failed: ${res.status}`);
  return await res.json();
}

export async function playAlertSound() {
  const url = `${getEspBaseUrl()}/api/audio/alert`;
  const res = await fetch(url, { method: 'POST' });
  if (!res.ok) throw new Error(`Alert chime failed: ${res.status}`);
  return await res.json();
}

export async function setLedLighting({ mode = 'solid', r = 0, g = 0, b = 0, speed_ms = 500 }) {
  const url = `${getEspBaseUrl()}/api/led/set`;
  const res = await fetch(url, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ mode, r, g, b, speed_ms }),
  });
  if (!res.ok) throw new Error(`LED set failed: ${res.status}`);
  return await res.json();
}
