import React, { useCallback } from 'react';
import { StyleSheet, View } from 'react-native';
import HapticFeedback from 'react-native-haptic-feedback';
import { runOnJS, useAnimatedReaction, useSharedValue, type SharedValue } from 'react-native-reanimated';

import { AlignmentGuide } from './AlignmentGuide';

/** Minimum time (ms) between two success haptics, so jitter around the alignment threshold can't buzz repeatedly. */
const HAPTIC_COOLDOWN_MS = 1500;

export interface AlignmentOverlayProps {
  /** 1 once AnalysisStatus is OK, 0 otherwise - drives the reticle's color/pulse. */
  isOk: SharedValue<number>;
  /** 1 if a product contour was found in the last frame, 0 otherwise. */
  found: SharedValue<number>;
  /** 1 when alignment/framing/lighting are all simultaneously ideal, 0 otherwise. */
  perfect: SharedValue<number>;
  /** Called on every aligned/misaligned transition (i.e. not on every frame). */
  onAlignedChange?: (aligned: boolean) => void;
}

/**
 * Camera overlay: a single, unified AlignmentGuide reticle - replaces the
 * previous dashed corner brackets + gyroscopic leveler pairing. Haptic
 * feedback fires once per aligned transition. Consumes only the
 * enums/normalized signals the native pipeline exposes - no raw
 * pitch/roll/centroid pixels cross into this component.
 */
export function AlignmentOverlay({ isOk, found, perfect, onAlignedChange }: AlignmentOverlayProps) {
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
      <AlignmentGuide isOk={isOk} found={found} />
    </View>
  );
}
