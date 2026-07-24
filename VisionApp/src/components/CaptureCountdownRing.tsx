import React from 'react';
import { StyleSheet, View } from 'react-native';
import Animated, { Easing, useAnimatedReaction, useAnimatedStyle, useSharedValue, withTiming, type SharedValue } from 'react-native-reanimated';

const RING_SIZE = 100;
const STROKE = 3;
const DOT_SIZE = 10;
/** How quickly the ring resets (not counts down) when alignment breaks mid-hold. */
const CANCEL_DURATION_MS = 150;

export interface CaptureCountdownRingProps {
  /** 1 when alignment/framing/lighting are all simultaneously ideal, 0 otherwise - see ProductAnalysis. */
  perfect: SharedValue<number>;
  /** How long `perfect` must hold before the photo auto-captures - must match useAutoCapture's delayMs so the ring completes exactly when the shutter fires. */
  durationMs: number;
}

/**
 * Progress ring wrapped around the shutter button: a dot sweeps one full
 * lap while `perfect` holds continuously, landing back at the top exactly
 * when auto-capture fires. Resets (not just pauses) the moment alignment
 * breaks, so a completed lap always means "a photo was just taken".
 */
export function CaptureCountdownRing({ perfect, durationMs }: CaptureCountdownRingProps) {
  const progress = useSharedValue(0);

  useAnimatedReaction(
    () => perfect.value > 0.5,
    aligned => {
      progress.value = aligned
        ? withTiming(1, { duration: durationMs, easing: Easing.linear })
        : withTiming(0, { duration: CANCEL_DURATION_MS });
    },
  );

  const containerStyle = useAnimatedStyle(() => ({
    opacity: progress.value > 0.01 ? 1 : 0,
  }));

  const orbitStyle = useAnimatedStyle(() => ({
    transform: [{ rotate: `${progress.value * 360}deg` }],
  }));

  return (
    <Animated.View style={[styles.container, containerStyle]} pointerEvents="none">
      <View style={styles.track} />
      <Animated.View style={[styles.orbit, orbitStyle]}>
        <View style={styles.dot} />
      </Animated.View>
    </Animated.View>
  );
}

const styles = StyleSheet.create({
  container: {
    position: 'absolute',
    bottom: 20,
    alignSelf: 'center',
    width: RING_SIZE,
    height: RING_SIZE,
  },
  track: {
    position: 'absolute',
    width: RING_SIZE,
    height: RING_SIZE,
    borderRadius: RING_SIZE / 2,
    borderWidth: STROKE,
    borderColor: 'rgba(0, 230, 118, 0.3)',
  },
  orbit: {
    position: 'absolute',
    width: RING_SIZE,
    height: RING_SIZE,
  },
  dot: {
    position: 'absolute',
    top: -DOT_SIZE / 2,
    left: RING_SIZE / 2 - DOT_SIZE / 2,
    width: DOT_SIZE,
    height: DOT_SIZE,
    borderRadius: DOT_SIZE / 2,
    backgroundColor: '#00e676',
  },
});
