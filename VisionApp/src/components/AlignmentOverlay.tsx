import React, { useCallback } from 'react';
import { StyleSheet, View } from 'react-native';
import HapticFeedback from 'react-native-haptic-feedback';
import { runOnJS, useAnimatedReaction, useSharedValue, type SharedValue } from 'react-native-reanimated';

import { AlignmentGuide } from './AlignmentGuide';
import { GyroscopicLeveler } from './GyroscopicLeveler';
import type { OrientationMode } from '../frameProcessors/productAnalyzer';

/** Minimum time (ms) between two success haptics, so jitter around the alignment threshold can't buzz repeatedly. */
const HAPTIC_COOLDOWN_MS = 1500;

export interface AlignmentOverlayProps {
  mode: OrientationMode;
  /** Device tilt error, normalized to [-1, 1] per axis - see ProductAnalysis.tilt. */
  tiltX: SharedValue<number>;
  tiltY: SharedValue<number>;
  /** 1 if a product contour was found in the last frame, 0 otherwise. */
  found: SharedValue<number>;
  /** Guide color stage: 0 = red, 1 = orange, 2 = green. */
  guideStage: SharedValue<number>;
  /** 1 when alignment/framing/lighting are all simultaneously ideal, 0 otherwise. */
  perfect: SharedValue<number>;
  /** Called on every aligned/misaligned transition (i.e. not on every frame). */
  onAlignedChange?: (aligned: boolean) => void;
}

/**
 * Camera overlay composed of the redesigned guidance pieces:
 *  - AlignmentGuide: a dashed silhouette guide whose border color reflects
 *    centering/framing quality,
 *  - GyroscopicLeveler: an ultra-simple leveler that swaps shape between
 *    flat-lay (concentric circles) and on-hanger (plumb line) modes.
 * Haptic feedback fires once per aligned transition. Consumes only the
 * enums/normalized vectors the native pipeline exposes - no raw pitch/roll/
 * centroid pixels cross into this component.
 */
export function AlignmentOverlay({ mode, tiltX, tiltY, found, guideStage, perfect, onAlignedChange }: AlignmentOverlayProps) {
  const triggerHaptic = useCallback(() => {
    HapticFeedback.trigger('notificationSuccess', { enableVibrateFallback: true });
  }, []);

  const lastHapticTimestamp = useSharedValue(0);

  useAnimatedReaction(
    () => perfect.value > 0.5,
    (aligned, wasAligned) => {
      if (aligned !== wasAligned && onAlignedChange != null) {
        runOnJS(onAlignedChange)(aligned);
      }
      if (aligned && wasAligned !== true) {
        const now = Date.now();
        if (now - lastHapticTimestamp.value >= HAPTIC_COOLDOWN_MS) {
          lastHapticTimestamp.value = now;
          runOnJS(triggerHaptic)();
        }
      }
    },
  );

  return (
    <View style={StyleSheet.absoluteFill} pointerEvents="none">
      <AlignmentGuide guideStage={guideStage} found={found} />
      <GyroscopicLeveler mode={mode} tiltX={tiltX} tiltY={tiltY} />
    </View>
  );
}
