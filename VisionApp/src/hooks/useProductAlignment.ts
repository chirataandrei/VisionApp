import { useCallback, useEffect, useRef, useState } from 'react';
import { useSharedValue, withSpring, withTiming } from 'react-native-reanimated';
import type { Orientation } from 'react-native-vision-camera';
import { useFrameProcessor } from 'react-native-vision-camera';
import { useRunOnJS } from 'react-native-worklets-core';

import { analyzeProduct, resetAnalysisSession } from '../frameProcessors/productAnalyzer';
import type { ProductAnalysis } from '../frameProcessors/productAnalyzer';
import { mapFrameToScreen } from '../utils/cameraTransforms';

const SPRING = { damping: 20, stiffness: 180 };

export interface DetectionSnapshot {
  analysis: ProductAnalysis;
  orientation: Orientation;
}

/**
 * Owns the camera → C++ vision pipeline wiring: the frame processor, the
 * Reanimated shared values it drives (consumed by AlignmentOverlay on the UI
 * thread), the latest JS-visible analysis snapshot (for the info panel), and
 * the freshest detection + frame orientation (for photo-capture-time
 * cropping, exposed as a ref since it needs to be read synchronously from an
 * imperative callback rather than trigger a re-render on every frame).
 */
export function useProductAlignment(deviceId: string | undefined, screenWidth: number, screenHeight: number) {
  const [analysis, setAnalysis] = useState<ProductAnalysis | null>(null);
  const lastDetectionRef = useRef<DetectionSnapshot | null>(null);

  const pitch = useSharedValue(0);
  const roll = useSharedValue(0);
  const found = useSharedValue(0);
  const centroidX = useSharedValue(screenWidth / 2);
  const centroidY = useSharedValue(screenHeight / 2);
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
        pitch.value = withSpring(result.pitch, SPRING);
        roll.value = withSpring(result.roll, SPRING);
        found.value = withTiming(result.found ? 1 : 0, { duration: 150 });
        if (result.found && result.frameWidth > 0 && result.frameHeight > 0) {
          const { x, y } = mapFrameToScreen(
            result.centroid.x,
            result.centroid.y,
            result.frameWidth,
            result.frameHeight,
            orientation,
            screenWidth,
            screenHeight,
          );
          centroidX.value = withSpring(x, SPRING);
          centroidY.value = withSpring(y, SPRING);
        }
      },
      [pitch, roll, found, centroidX, centroidY, screenWidth, screenHeight],
    ),
    [screenWidth, screenHeight],
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
      // is cheap and keeps pitch/roll fresh.
      const result = analyzeProduct(frame);
      onAnalysis(result, frame.orientation);
    },
    [onAnalysis, shouldResetSession],
  );

  return { frameProcessor, pitch, roll, found, centroidX, centroidY, analysis, lastDetectionRef };
}
