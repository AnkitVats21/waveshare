import { useEffect, useRef, useState, useCallback } from 'react';

const CONFIRM_TIMEOUT_MS = 2500;

// Optimistic local update for a single control value backed by a server
// snapshot that arrives asynchronously over the WebSocket.
//
// Call commit(newValue) on user action -> the UI shows newValue immediately
// ("pending"). Once `serverValue` catches up to match it, pending clears.
// If nothing confirms it within ~2.5s, it reverts to the last-known server
// value and `justFailed` flips true briefly (surface a "didn't apply" cue).
//
// This makes a whole class of bug impossible: a control that silently no-ops
// on the backend (like the old repeat/autoplay/caching buttons, which updated
// local state forever even though the daemon dropped them) will now visibly
// snap back instead of lying to the user.
export function usePendingValue(serverValue, sendCommit, isEqual = Object.is) {
  const [pending, setPending] = useState(null); // { value } | null
  const [justFailed, setJustFailed] = useState(false);
  const timerRef = useRef(null);
  const failTimerRef = useRef(null);

  useEffect(() => {
    if (pending !== null && isEqual(serverValue, pending.value)) {
      setPending(null);
      if (timerRef.current) clearTimeout(timerRef.current);
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [serverValue]);

  useEffect(() => {
    return () => {
      if (timerRef.current) clearTimeout(timerRef.current);
      if (failTimerRef.current) clearTimeout(failTimerRef.current);
    };
  }, []);

  const commit = useCallback((value) => {
    setPending({ value });
    sendCommit(value);

    if (timerRef.current) clearTimeout(timerRef.current);
    timerRef.current = setTimeout(() => {
      setPending((cur) => {
        if (cur && isEqual(cur.value, value)) {
          setJustFailed(true);
          if (failTimerRef.current) clearTimeout(failTimerRef.current);
          failTimerRef.current = setTimeout(() => setJustFailed(false), 1500);
          return null;
        }
        return cur;
      });
    }, CONFIRM_TIMEOUT_MS);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [sendCommit]);

  return {
    value: pending !== null ? pending.value : serverValue,
    isPending: pending !== null,
    justFailed,
    commit,
  };
}
