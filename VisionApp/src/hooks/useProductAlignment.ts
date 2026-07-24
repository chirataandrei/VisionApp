import { useCallback, useEffect, useRef, useState } from 'react';
import { useSharedValue, withTiming } from 'react-native-reanimated';
import type { Orientation } from 'react-native-vision-camera';
import { useFrameProcessor } from 'react-native-vision-camera';
import { useRunOnJS } from 'react-native-worklets-core';

import { analyzeProduct, resetAnalysisSession } from '../frameProcessors/productAnalyzer';
import type { ProductAnalysis } from '../frameProcessors/productAnalyzer';

export interface DetectionSnapshot {
  analysis: ProductAnalysis;
  orientation: Orientation;
}

/**
 * Owns the camera → C++ vision pipeline wiring: the frame processor, the
 * Reanimated shared values it drives (consumed by AlignmentOverlay on the UI
 * thread), the latest JS-visible analysis snapshot (for the feedback
 * banner), and the freshest detection + frame orientation (for
 * photo-capture-time cropping, exposed as a ref since it needs to be read
 * synchronously from an imperative callback rather than trigger a re-render
 * on every frame).
 *
 * `isOk` and `perfect` are deliberately distinct signals: `isOk` mirrors
 * `AnalysisStatus === 'OK'` alone (drives the reticle's color/pulse), while
 * `perfect` additionally requires good lighting (`isReadyForCapture` -
 * drives the haptic and the auto-capture countdown), since a frame can be
 * perfectly aligned yet still too dark/bright to actually capture.
 */
export function useProductAlignment(deviceId: string | undefined) {
  const [analysis, setAnalysis] = useState<ProductAnalysis | null>(null);
  const lastDetectionRef = useRef<DetectionSnapshot | null>(null);

  const found = useSharedValue(0);
  const isOk = useSharedValue(0);
  const perfect = useSharedValue(0);
  // Set whenever the active camera device changes; consumed once by the
  // frame processor to reset native tracking state before the next analysis.
  const shouldResetSession = useSharedValue(false);

  useEffect(() => {
    shouldResetSession.value = true;
  }, [deviceId, shouldResetSession]);

  const onAnalysis = useRunOnJS(
    useCallback(
      (result: ProductAnalysis, orientation: Orientation) => {
        lastDetectionRef.current = { analysis: result, orientation };
        // Detection fields only change on processed frames (every 3rd);
        // skip the React re-render for the cached in-between frames.
        if (result.processed) {
          setAnalysis(result);
        }
        found.value = withTiming(result.found ? 1 : 0, { duration: 150 });
        isOk.value = withTiming(result.status === 'OK' ? 1 : 0, { duration: 200 });
        perfect.value = withTiming(result.isReadyForCapture ? 1 : 0, { duration: 150 });
      },
      [found, isOk, perfect],
    ),
    [],
  );

  const frameProcessor = useFrameProcessor(
    frame => {
      'worklet';
      if (shouldResetSession.value) {
        resetAnalysisSession(frame);
        shouldResetSession.value = false;
      }
      // The frame is handed to native C++ by reference via JSI — zero copies,
      // nothing crosses the old React Native bridge. C++ processes every 3rd
      // frame and answers from its cache in between, so calling it per-frame
      // is cheap and keeps the status fresh.
      const result = analyzeProduct(frame);
      onAnalysis(result, frame.orientation);
    },
    [onAnalysis, shouldResetSession],
  );

  return { frameProcessor, found, isOk, perfect, analysis, lastDetectionRef };
}
