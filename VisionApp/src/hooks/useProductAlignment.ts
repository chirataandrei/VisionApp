import { useCallback, useEffect, useRef, useState } from 'react';
import { useSharedValue, withSpring, withTiming } from 'react-native-reanimated';
import type { Orientation } from 'react-native-vision-camera';
import { useFrameProcessor } from 'react-native-vision-camera';
import { useRunOnJS } from 'react-native-worklets-core';

import { analyzeProduct, resetAnalysisSession } from '../frameProcessors/productAnalyzer';
import type { ProductAnalysis } from '../frameProcessors/productAnalyzer';

const SPRING = { damping: 20, stiffness: 180 };

export interface DetectionSnapshot {
  analysis: ProductAnalysis;
  orientation: Orientation;
}

/** Guide color stage for AlignmentGuide's border: 0 = red, 1 = orange, 2 = green. */
const GUIDE_STAGE_RED = 0;
const GUIDE_STAGE_ORANGE = 1;
const GUIDE_STAGE_GREEN = 2;

/**
 * Reduces a ProductAnalysis down to the guide's color stage: neon green only
 * once isReadyForCapture is actually true (alignment AND lighting), red for
 * the two problems the corner guide is meant to call out (not centered, cut
 * off at the frame edge), orange for everything else still being worked
 * toward (phone tilted, framing/centering ok but lighting bad, or nothing
 * found yet).
 */
function guideStageFor(result: ProductAnalysis): number {
  if (result.isReadyForCapture) {
    return GUIDE_STAGE_GREEN;
  }
  if (result.status === 'NOT_CENTERED' || result.status === 'CUT_OFF_MARGINS') {
    return GUIDE_STAGE_RED;
  }
  return GUIDE_STAGE_ORANGE;
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
 * The native pipeline already reduces raw pitch/roll/centroid pixels down to
 * enums and normalized [-1, 1] vectors (see ProductAnalysis) - this hook
 * only turns those into smoothly-animated shared values: `tiltX`/`tiltY` for
 * the leveler, `guideStage` for the silhouette's color, `perfect` for the
 * aligned-transition (haptics, auto-capture).
 */
export function useProductAlignment(deviceId: string | undefined) {
  const [analysis, setAnalysis] = useState<ProductAnalysis | null>(null);
  const lastDetectionRef = useRef<DetectionSnapshot | null>(null);

  const tiltX = useSharedValue(0);
  const tiltY = useSharedValue(0);
  const found = useSharedValue(0);
  const guideStage = useSharedValue(GUIDE_STAGE_ORANGE);
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
        tiltX.value = withSpring(result.tilt.dx, SPRING);
        tiltY.value = withSpring(result.tilt.dy, SPRING);
        found.value = withTiming(result.found ? 1 : 0, { duration: 150 });
        guideStage.value = withTiming(guideStageFor(result), { duration: 200 });
        perfect.value = withTiming(result.isReadyForCapture ? 1 : 0, { duration: 150 });
      },
      [tiltX, tiltY, found, guideStage, perfect],
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
      // is cheap and keeps the tilt vector fresh.
      const result = analyzeProduct(frame);
      onAnalysis(result, frame.orientation);
    },
    [onAnalysis, shouldResetSession],
  );

  return { frameProcessor, tiltX, tiltY, found, guideStage, perfect, analysis, lastDetectionRef };
}
