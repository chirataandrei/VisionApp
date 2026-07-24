import React, { useCallback } from 'react';
import { StyleSheet, View, useWindowDimensions } from 'react-native';
import HapticFeedback from 'react-native-haptic-feedback';
import Animated, {
  Easing,
  interpolateColor,
  runOnJS,
  useAnimatedReaction,
  useAnimatedStyle,
  useDerivedValue,
  withRepeat,
  withSequence,
  withTiming,
  type SharedValue,
} from 'react-native-reanimated';

/** Tilt tolerance (degrees) for the phone to count as "flat". */
const LEVEL_THRESHOLD_DEG = 3;
/** Max tilt (degrees) the bubble visualizes before pinning to the rim. */
const MAX_TILT_DEG = 30;
/** How close (px) the centroid must be to the screen center to count as centered. */
const CENTER_THRESHOLD_PX = 60;

const LEVEL_SIZE = 150;
const BUBBLE_SIZE = 36;
const ARROW_HEAD = 16;

export interface AlignmentOverlayProps {
  /** Device pitch in degrees (from the C++ sensor fusion). */
  pitch: SharedValue<number>;
  /** Device roll in degrees. */
  roll: SharedValue<number>;
  /** 1 if a product contour was found in the last frame, 0 otherwise. */
  found: SharedValue<number>;
  /** Centroid of the detected contour, mapped to screen coordinates (px). */
  centroidX: SharedValue<number>;
  centroidY: SharedValue<number>;
}

/**
 * Reanimated overlay driven by the frame processor payload:
 *  - a bubble level guiding the user to hold the phone flat,
 *  - an arrow from the screen center to the detected object's centroid,
 *  - a green border flash + haptic feedback when both are aligned.
 */
export function AlignmentOverlay({ pitch, roll, found, centroidX, centroidY }: AlignmentOverlayProps) {
  const { width: screenWidth, height: screenHeight } = useWindowDimensions();
  const centerX = screenWidth / 2;
  const centerY = screenHeight / 2;

  // ---- derived signals ----------------------------------------------------

  const tiltMagnitude = useDerivedValue(() => Math.sqrt(pitch.value ** 2 + roll.value ** 2));

  const centroidDistance = useDerivedValue(() => {
    const dx = centroidX.value - centerX;
    const dy = centroidY.value - centerY;
    return Math.sqrt(dx * dx + dy * dy);
  });

  const isAligned = useDerivedValue(
    () =>
      found.value > 0.5 &&
      Math.abs(pitch.value) < LEVEL_THRESHOLD_DEG &&
      Math.abs(roll.value) < LEVEL_THRESHOLD_DEG &&
      centroidDistance.value < CENTER_THRESHOLD_PX,
  );

  // ---- aligned: border flash + haptics -------------------------------------

  const flash = useDerivedValue(() =>
    isAligned.value
      ? withRepeat(
          withSequence(
            withTiming(1, { duration: 180, easing: Easing.out(Easing.quad) }),
            withTiming(0.35, { duration: 180, easing: Easing.in(Easing.quad) }),
          ),
          -1,
          true,
        )
      : withTiming(0, { duration: 200 }),
  );

  const triggerHaptic = useCallback(() => {
    HapticFeedback.trigger('notificationSuccess', { enableVibrateFallback: true });
  }, []);

  useAnimatedReaction(
    () => isAligned.value,
    (aligned, wasAligned) => {
      if (aligned && wasAligned !== true) {
        runOnJS(triggerHaptic)();
      }
    },
  );

  const borderStyle = useAnimatedStyle(() => ({ opacity: flash.value }));

  // ---- bubble level ---------------------------------------------------------

  // Subtle perspective tilt of the whole gauge makes it read as 3D.
  const levelStyle = useAnimatedStyle(() => ({
    transform: [
      { perspective: 600 },
      { rotateX: `${-pitch.value * 0.4}deg` },
      { rotateY: `${-roll.value * 0.4}deg` },
    ],
  }));

  const bubbleStyle = useAnimatedStyle(() => {
    const maxOffset = (LEVEL_SIZE - BUBBLE_SIZE) / 2 - 4;
    const clamp = (v: number) => Math.min(Math.max(v / MAX_TILT_DEG, -1), 1) * maxOffset;
    // A physical bubble drifts toward the raised side of the device.
    return {
      transform: [{ translateX: clamp(-roll.value) }, { translateY: clamp(-pitch.value) }],
      backgroundColor: interpolateColor(
        Math.min(tiltMagnitude.value, MAX_TILT_DEG),
        [0, LEVEL_THRESHOLD_DEG, MAX_TILT_DEG],
        ['#00e676', '#00e676', '#ff5252'],
      ),
    };
  });

  // ---- centroid arrow + target dot -----------------------------------------

  const arrowStyle = useAnimatedStyle(() => {
    const dx = centroidX.value - centerX;
    const dy = centroidY.value - centerY;
    const distance = Math.sqrt(dx * dx + dy * dy);
    const angle = Math.atan2(dy, dx);
    const visible = found.value > 0.5 && distance > CENTER_THRESHOLD_PX;
    const length = Math.min(distance - 24, Math.min(screenWidth, screenHeight) * 0.4);
    return {
      opacity: withTiming(visible ? 0.95 : 0, { duration: 150 }),
      width: Math.max(length, 0),
      transform: [{ rotate: `${angle}rad` }],
    };
  });

  const targetDotStyle = useAnimatedStyle(() => ({
    opacity: withTiming(found.value > 0.5 ? 1 : 0, { duration: 150 }),
    transform: [
      { translateX: centroidX.value - 14 },
      { translateY: centroidY.value - 14 },
      { scale: isAligned.value ? 1.2 : 1 },
    ],
  }));

  return (
    <View style={StyleSheet.absoluteFill} pointerEvents="none">
      {/* Green border flash when everything is aligned */}
      <Animated.View style={[styles.alignedBorder, borderStyle]} />

      {/* Target dot on the detected centroid */}
      <Animated.View style={[styles.targetDot, targetDotStyle]} />

      {/* Arrow from screen center toward the centroid (rotates around its left edge) */}
      <Animated.View style={[styles.arrow, { left: centerX, top: centerY - 2 }, arrowStyle]}>
        <View style={styles.arrowShaft} />
        <View style={styles.arrowHead} />
      </Animated.View>

      {/* Screen-center reticle */}
      <View style={[styles.reticle, { left: centerX - 6, top: centerY - 6 }]} />

      {/* Bubble level */}
      <Animated.View style={[styles.level, levelStyle]}>
        <View style={styles.levelRingOuter} />
        <View style={styles.levelRingInner} />
        <View style={styles.levelCrosshairH} />
        <View style={styles.levelCrosshairV} />
        <Animated.View style={[styles.bubble, bubbleStyle]}>
          <View style={styles.bubbleHighlight} />
        </Animated.View>
      </Animated.View>
    </View>
  );
}

const styles = StyleSheet.create({
  alignedBorder: {
    position: 'absolute',
    top: 0,
    left: 0,
    right: 0,
    bottom: 0,
    borderWidth: 8,
    borderColor: '#00e676',
    borderRadius: 24,
  },
  level: {
    position: 'absolute',
    top: 72,
    alignSelf: 'center',
    width: LEVEL_SIZE,
    height: LEVEL_SIZE,
    borderRadius: LEVEL_SIZE / 2,
    backgroundColor: 'rgba(0, 0, 0, 0.35)',
    alignItems: 'center',
    justifyContent: 'center',
  },
  levelRingOuter: {
    position: 'absolute',
    top: 0,
    left: 0,
    right: 0,
    bottom: 0,
    borderRadius: LEVEL_SIZE / 2,
    borderWidth: 2,
    borderColor: 'rgba(255, 255, 255, 0.7)',
  },
  levelRingInner: {
    position: 'absolute',
    width: CENTER_THRESHOLD_PX,
    height: CENTER_THRESHOLD_PX,
    borderRadius: CENTER_THRESHOLD_PX / 2,
    borderWidth: 1.5,
    borderColor: 'rgba(255, 255, 255, 0.6)',
  },
  levelCrosshairH: {
    position: 'absolute',
    width: LEVEL_SIZE - 16,
    height: StyleSheet.hairlineWidth,
    backgroundColor: 'rgba(255, 255, 255, 0.5)',
  },
  levelCrosshairV: {
    position: 'absolute',
    width: StyleSheet.hairlineWidth,
    height: LEVEL_SIZE - 16,
    backgroundColor: 'rgba(255, 255, 255, 0.5)',
  },
  bubble: {
    width: BUBBLE_SIZE,
    height: BUBBLE_SIZE,
    borderRadius: BUBBLE_SIZE / 2,
    shadowColor: '#000',
    shadowOpacity: 0.4,
    shadowRadius: 4,
    shadowOffset: { width: 0, height: 3 },
    elevation: 4,
  },
  bubbleHighlight: {
    position: 'absolute',
    top: 5,
    left: 7,
    width: BUBBLE_SIZE * 0.35,
    height: BUBBLE_SIZE * 0.22,
    borderRadius: BUBBLE_SIZE * 0.2,
    backgroundColor: 'rgba(255, 255, 255, 0.55)',
    transform: [{ rotate: '-20deg' }],
  },
  arrow: {
    position: 'absolute',
    height: 4,
    flexDirection: 'row',
    alignItems: 'center',
    transformOrigin: 'left center',
  },
  arrowShaft: {
    flex: 1,
    height: 4,
    borderRadius: 2,
    backgroundColor: '#ffd740',
  },
  arrowHead: {
    width: 0,
    height: 0,
    borderTopWidth: ARROW_HEAD * 0.6,
    borderBottomWidth: ARROW_HEAD * 0.6,
    borderLeftWidth: ARROW_HEAD,
    borderTopColor: 'transparent',
    borderBottomColor: 'transparent',
    borderLeftColor: '#ffd740',
  },
  reticle: {
    position: 'absolute',
    width: 12,
    height: 12,
    borderRadius: 6,
    borderWidth: 2,
    borderColor: 'rgba(255, 255, 255, 0.9)',
  },
  targetDot: {
    position: 'absolute',
    width: 28,
    height: 28,
    borderRadius: 14,
    borderWidth: 3,
    borderColor: '#00e676',
    backgroundColor: 'rgba(0, 230, 118, 0.25)',
  },
});
