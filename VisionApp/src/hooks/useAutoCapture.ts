import { useCallback, useEffect, useRef } from 'react';

/**
 * Fires `onCapture` once alignment holds continuously for `delayMs`.
 * Returns `handleAlignedChange`, meant to be wired as AlignmentOverlay's
 * `onAlignedChange` prop: it consumes the aligned/misaligned transition
 * signal, starting the timer on "became aligned" and cancelling it on
 * "became misaligned" (or when `enabled` is false) so a photo is never
 * captured from a stale alignment state.
 */
export function useAutoCapture(onCapture: () => void, delayMs: number, enabled: boolean) {
  const timer = useRef<ReturnType<typeof setTimeout> | null>(null);

  useEffect(() => {
    return () => {
      if (timer.current != null) {
        clearTimeout(timer.current);
      }
    };
  }, []);

  const handleAlignedChange = useCallback(
    (aligned: boolean) => {
      if (timer.current != null) {
        clearTimeout(timer.current);
        timer.current = null;
      }
      if (aligned && enabled) {
        timer.current = setTimeout(() => {
          timer.current = null;
          onCapture();
        }, delayMs);
      }
    },
    [onCapture, delayMs, enabled],
  );

  return { handleAlignedChange };
}
