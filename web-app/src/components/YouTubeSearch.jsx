import React, { useState } from 'react';
import { Search, Play, Loader2 } from 'lucide-react';
import { searchYouTube, resolveStreamUrl } from '../lib/api';

const QUICK_VIBES = [
  { label: 'Lofi chill', query: 'Lofi hip hop beats to relax' },
  { label: 'Synthwave', query: 'Synthwave retrowave radio' },
  { label: 'Coldplay', query: 'Coldplay top hits' },
  { label: 'Hans Zimmer', query: 'Hans Zimmer cinematic sound' },
  { label: 'A.R. Rahman', query: 'A.R. Rahman all time hits' },
  { label: 'Electro house', query: 'Electro house workout mix' },
];

function formatDuration(seconds) {
  if (!seconds) return '0:00';
  const m = Math.floor(seconds / 60);
  const s = Math.floor(seconds % 60);
  return `${m}:${s.toString().padStart(2, '0')}`;
}

export default function YouTubeSearch({ onPlay, disabled }) {
  const [query, setQuery] = useState('');
  const [results, setResults] = useState([]);
  const [isSearching, setIsSearching] = useState(false);
  const [resolvingId, setResolvingId] = useState(null);
  const [statusMsg, setStatusMsg] = useState('');

  const handleSearch = async (term) => {
    const q = (term !== undefined ? term : query).trim();
    if (!q) return;
    setIsSearching(true);
    setStatusMsg('Searching...');
    try {
      const data = await searchYouTube(q);
      setResults(Array.isArray(data) ? data : []);
      setStatusMsg('');
    } catch (err) {
      setStatusMsg(`Search failed: ${err.message}`);
    } finally {
      setIsSearching(false);
    }
  };

  const handlePlay = async (track) => {
    setResolvingId(track.videoId);
    setStatusMsg(`Resolving stream for "${track.title}"...`);
    try {
      const streamUrl = await resolveStreamUrl(track.videoId);
      onPlay(
        { id: track.videoId, title: track.title, artist: track.author, duration: track.lengthSeconds },
        streamUrl,
      );
      setStatusMsg('');
    } catch (err) {
      setStatusMsg(`Playback error: ${err.message}`);
    } finally {
      setResolvingId(null);
    }
  };

  return (
    <div className="search-view">
      <form className="search-bar" onSubmit={(e) => { e.preventDefault(); handleSearch(); }}>
        <Search size={16} className="search-icon" />
        <input
          type="text"
          value={query}
          onChange={(e) => setQuery(e.target.value)}
          placeholder="Search songs, artists, or paste a YouTube link..."
        />
        <button type="submit" className="btn-primary" disabled={isSearching}>
          {isSearching ? <Loader2 size={16} className="spin" /> : <Search size={16} />}
          Search
        </button>
      </form>

      <div className="quick-tags">
        {QUICK_VIBES.map((v) => (
          <button key={v.query} className="tag-btn" onClick={() => { setQuery(v.query); handleSearch(v.query); }}>
            {v.label}
          </button>
        ))}
      </div>

      {statusMsg && <div className="status-banner">{statusMsg}</div>}

      <div className="results-grid">
        {results.length > 0 ? (
          results.map((track) => (
            <div key={track.videoId} className="song-card">
              <img
                className="song-thumb"
                src={`https://i.ytimg.com/vi/${track.videoId}/mqdefault.jpg`}
                alt=""
                loading="lazy"
                onError={(e) => { e.target.src = `https://img.youtube.com/vi/${track.videoId}/0.jpg`; }}
              />
              <span className="song-duration">{formatDuration(track.lengthSeconds)}</span>
              <div className="song-body">
                <h4 className="song-title" title={track.title}>{track.title}</h4>
                <p className="song-artist">{track.author}</p>
                <button
                  className="btn-play"
                  onClick={() => handlePlay(track)}
                  disabled={disabled || resolvingId === track.videoId}
                >
                  {resolvingId === track.videoId ? (
                    <><Loader2 size={14} className="spin" /> Resolving...</>
                  ) : (
                    <><Play size={14} fill="currentColor" /> Play</>
                  )}
                </button>
              </div>
            </div>
          ))
        ) : (
          <div className="empty-state">
            <h3>Search to stream music to the Waveshare</h3>
            <p>Stream URLs are resolved directly in your browser and sent to the device.</p>
          </div>
        )}
      </div>
    </div>
  );
}
