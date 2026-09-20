// Shared noembed.com track-metadata resolution + cache.
//
// Both the search results list and the SD library list show raw YouTube
// video IDs as the "title" until real metadata is resolved. This used to be
// implemented twice (PlayerDock.jsx and SdLibrary.jsx each had their own
// copy of this exact cache) - deduped into one module here.

const cache = new Map();
const inflight = new Map();

export function looksLikeVideoId(id) {
  return typeof id === 'string' && id.length === 11;
}

export function needsResolution(track) {
  if (!track) return false;
  const id = track.id || track.videoId;
  if (!looksLikeVideoId(id)) return false;
  return !track.artist || track.artist === 'Local Storage' || track.title === id;
}

export async function resolveTrackInfo(id) {
  if (!looksLikeVideoId(id)) return null;
  if (cache.has(id)) return cache.get(id);
  if (inflight.has(id)) return inflight.get(id);

  const promise = (async () => {
    try {
      const res = await fetch(`https://noembed.com/embed?url=https://www.youtube.com/watch?v=${encodeURIComponent(id)}`);
      if (res.ok) {
        const data = await res.json();
        if (data && data.title) {
          const info = { title: data.title, artist: data.author_name || 'YouTube' };
          cache.set(id, info);
          return info;
        }
      }
    } catch {
      // Offline / network error - just leave it unresolved.
    }
    return null;
  })();

  inflight.set(id, promise);
  try {
    return await promise;
  } finally {
    inflight.delete(id);
  }
}
