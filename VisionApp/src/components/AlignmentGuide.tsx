import React from 'react';
import { StyleSheet, useWindowDimensions } from 'react-native';
import Animated, {
  interpolateColor,
  useAnimatedStyle,
  useDerivedValue,
  withTiming,
  type SharedValue,
} from 'react-native-reanimated';

/** Fraction of the screen the corner-bracket guide occupies on each axis. */
const GUIDE_WIDTH_FRACTION = 0.72;
const GUIDE_HEIGHT_FRACTION = 0.55;
const CORNER_SIZE = 40;
const CORNER_RADIUS = 16;
const STROKE_WIDTH = 4;

export interface AlignmentGuideProps {
  /** Guide color stage: 0 = red (not centered / cut off), 1 = orange (still adjusting), 2 = neon green (isReadyForCapture). */
  guideStage: SharedValue<number>;
  /** 1 if a product contour was found in the last frame, 0 otherwise. */
  found: SharedValue<number>;
}

/**
 * Modern viewfinder guide: four rounded corner brackets marking a fixed
 * target zone (doesn't track the detected object's position - it's a static
 * placement guide, not a live outline). Colored red/orange while still
 * adjusting, and glows neon green with a soft halo the instant
 * isReadyForCapture flips true (see useProductAlignment's guideStageFor).
 */
export function AlignmentGuide({ guideStage, found }: AlignmentGuideProps) {
  const { width: screenWidth, height: screenHeight } = useWindowDimensions();

  const borderColor = useDerivedValue(() =>
    interpolateColor(guideStage.value, [0, 1, 2], ['#ff5252', '#ffab40', '#39ff8f']),
  );

  const cornerStyle = useAnimatedStyle(() => ({
    borderColor: borderColor.value,
  }));

  const containerStyle = useAnimatedStyle(() => {
    const ready = guideStage.value > 1.5;
    return {
      opacity: withTiming(found.value > 0.5 ? 1 : 0.55, { duration: 200 }),
      transform: [{ scale: withTiming(ready ? 1.03 : 1, { duration: 200 }) }],
      // Neon halo - iOS only (Android's `elevation` shadows can't be
      // colored, so this gracefully degrades to no glow there; the color
      // change to neon green already communicates readiness on its own).
      shadowOpacity: withTiming(ready ? 0.9 : 0, { duration: 200 }),
      shadowRadius: withTiming(ready ? 18 : 0, { duration: 200 }),
    };
  });

  const guideWidth = screenWidth * GUIDE_WIDTH_FRACTION;
  const guideHeight = screenHeight * GUIDE_HEIGHT_FRACTION;

  return (
    <Animated.View
      pointerEvents="none"
      style={[
        styles.container,
        {
          width: guideWidth,
          height: guideHeight,
          left: (screenWidth - guideWidth) / 2,
          top: (screenHeight - guideHeight) / 2,
        },
        containerStyle,
      ]}>
      <Animated.View style={[styles.corner, styles.topLeft, cornerStyle]} />
      <Animated.View style={[styles.corner, styles.topRight, cornerStyle]} />
      <Animated.View style={[styles.corner, styles.bottomLeft, cornerStyle]} />
      <Animated.View style={[styles.corner, styles.bottomRight, cornerStyle]} />
    </Animated.View>
  );
}

const styles = StyleSheet.create({
  container: {
    position: 'absolute',
    shadowColor: '#39ff8f',
    shadowOffset: { width: 0, height: 0 },
  },
  corner: {
    position: 'absolute',
    width: CORNER_SIZE,
    height: CORNER_SIZE,
  },
  topLeft: {
    top: 0,
    left: 0,
    borderTopWidth: STROKE_WIDTH,
    borderLeftWidth: STROKE_WIDTH,
    borderTopLeftRadius: CORNER_RADIUS,
  },
  topRight: {
    top: 0,
    right: 0,
    borderTopWidth: STROKE_WIDTH,
    borderRightWidth: STROKE_WIDTH,
    borderTopRightRadius: CORNER_RADIUS,
  },
  bottomLeft: {
    bottom: 0,
    left: 0,
    borderBottomWidth: STROKE_WIDTH,
    borderLeftWidth: STROKE_WIDTH,
    borderBottomLeftRadius: CORNER_RADIUS,
  },
  bottomRight: {
    bottom: 0,
    right: 0,
    borderBottomWidth: STROKE_WIDTH,
    borderRightWidth: STROKE_WIDTH,
    borderBottomRightRadius: CORNER_RADIUS,
  },
});
