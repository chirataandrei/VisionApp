import React from 'react';
import { StyleSheet, useWindowDimensions } from 'react-native';
import Animated, {
  interpolateColor,
  useAnimatedReaction,
  useAnimatedStyle,
  useDerivedValue,
  useSharedValue,
  withSequence,
  withTiming,
  type SharedValue,
} from 'react-native-reanimated';

/** Fraction of the screen the reticle occupies on each axis. */
const GUIDE_WIDTH_FRACTION = 0.72;
const GUIDE_HEIGHT_FRACTION = 0.55;
const BORDER_RADIUS = 28;
const BORDER_WIDTH = 1.5;

const DEFAULT_COLOR = 'rgba(255, 255, 255, 0.7)';
const READY_COLOR = '#00FF00';

export interface AlignmentGuideProps {
  /** 1 once AnalysisStatus is OK, 0 otherwise - see useProductAlignment. */
  isOk: SharedValue<number>;
  /** 1 if a product contour was found in the last frame, 0 otherwise. */
  found: SharedValue<number>;
}

/**
 * Single, unified framing guide: a subtle rounded dashed reticle. Doesn't
 * track the detected object's position - it's a static placement target,
 * not a live outline. The border is white/gray by default and turns vibrant
 * green with a brief scale pulse the instant AnalysisStatus flips to OK.
 * Replaces the previous three-piece guide (dashed corner brackets + a
 * gyroscopic leveler hanging from the top bar) with one element.
 */
export function AlignmentGuide({ isOk, found }: AlignmentGuideProps) {
  const { width: screenWidth, height: screenHeight } = useWindowDimensions();
  const pulseScale = useSharedValue(1);

  // Fires a single brief scale bump on the 0→1 transition only - not a
  // repeating "breathing" loop, and not retriggered while already OK.
  useAnimatedReaction(
    () => isOk.value > 0.5,
    (ready, wasReady) => {
      if (ready && wasReady !== true) {
        pulseScale.value = withSequence(withTiming(1.06, { duration: 140 }), withTiming(1, { duration: 180 }));
      }
    },
  );

  const borderColor = useDerivedValue(() => interpolateColor(isOk.value, [0, 1], [DEFAULT_COLOR, READY_COLOR]));

  const style = useAnimatedStyle(() => ({
    borderColor: borderColor.value,
    opacity: withTiming(found.value > 0.5 ? 1 : 0.55, { duration: 200 }),
    transform: [{ scale: pulseScale.value }],
  }));

  const guideWidth = screenWidth * GUIDE_WIDTH_FRACTION;
  const guideHeight = screenHeight * GUIDE_HEIGHT_FRACTION;

  return (
    <Animated.View
      pointerEvents="none"
      style={[
        styles.reticle,
        {
          width: guideWidth,
          height: guideHeight,
          left: (screenWidth - guideWidth) / 2,
          top: (screenHeight - guideHeight) / 2,
        },
        style,
      ]}
    />
  );
}

const styles = StyleSheet.create({
  reticle: {
    position: 'absolute',
    borderWidth: BORDER_WIDTH,
    borderStyle: 'dashed',
    borderRadius: BORDER_RADIUS,
  },
});
