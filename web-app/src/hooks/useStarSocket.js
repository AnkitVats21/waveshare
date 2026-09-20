import { useEffect, useRef, useState, useCallback } from 'react';
import { getDaemonWsUrl } from '../lib/api';

const RECONNECT_MS = 3000;

const EMPTY_SNAPSHOT = {
  connected: false,
  up: 0,
  state: { speaker_volume: 80, mic_gain_db: 60, mic_enabled: true, is_recording: false, sample_rate: 0 },
  led: { mode: 0, color: { r: 0, g: 0, b: 0 }, speed_ms: 500 },
  music: {
    state: 'IDLE',
    current_track: { id: '', title: '', artist: '', duration: 0 },
    position_ms: 0,
    duration_ms: 0,
    seekable: false,
    repeat_mode: 0,
    autoplay: true,
    caching: false,
  },
  bluetooth: { connected: false, device_name: '' },
};

// Single WebSocket connection to the star-replica-daemon dashboard channel.
//
// The daemon pushes a full state snapshot on connect and after every change
// (see StarServer::broadcastSnapshot). `send()` is the one command envelope
// for everything with live device state: {"cmd": "<name>", ...}. Read-only,
// one-shot operations (search, library ops, alert chime) don't go through
// this - see lib/api.js.
export function useStarSocket() {
  const [snapshot, setSnapshot] = useState(EMPTY_SNAPSHOT);
  const [online, setOnline] = useState(false);
  const wsRef = useRef(null);
  const reconnectRef = useRef(null);
  const destroyedRef = useRef(false);

  useEffect(() => {
    destroyedRef.current = false;

    const connect = () => {
      if (destroyedRef.current) return;
      const ws = new WebSocket(getDaemonWsUrl());
      wsRef.current = ws;

      ws.onopen = () => {
        if (destroyedRef.current) {
          ws.close();
          return;
        }
        setOnline(true);
      };

      ws.onmessage = (evt) => {
        if (destroyedRef.current) return;
        try {
          const data = JSON.parse(evt.data);
          if (data.type === 'device_status') {
            setSnapshot((prev) => ({ ...prev, connected: !!data.connected }));
          } else {
            setSnapshot(data);
          }
        } catch (e) {
          console.warn('[ws] parse error', e);
        }
      };

      ws.onerror = () => {};
      ws.onclose = () => {
        setOnline(false);
        if (!destroyedRef.current) {
          reconnectRef.current = setTimeout(connect, RECONNECT_MS);
        }
      };
    };

    connect();
    return () => {
      destroyedRef.current = true;
      if (reconnectRef.current) clearTimeout(reconnectRef.current);
      if (wsRef.current) wsRef.current.close();
    };
  }, []);

  const send = useCallback((cmd, payload = {}) => {
    const ws = wsRef.current;
    if (!ws || ws.readyState !== WebSocket.OPEN) return false;
    try {
      ws.send(JSON.stringify({ cmd, ...payload }));
      return true;
    } catch (e) {
      console.warn('[ws] send error', e);
      return false;
    }
  }, []);

  return { snapshot, online, send };
}
