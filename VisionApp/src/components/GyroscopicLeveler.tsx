import React from 'react';
import { StyleSheet, View } from 'react-native';
import Animated, { interpolateColor, useAnimatedStyle, useDerivedValue, type SharedValue } from 'react-native-reanimated';

import type { OrientationMode } from '../frameProcessors/productAnalyzer';
import { distance } from '../utils/geometry';

const LEVEL_SIZE = 96;
const RING_THICKNESS = 3;
const MAX_RING_OFFSET = (LEVEL_SIZE - RING_THICKNESS * 2) / 2 - 6;

const PLUMB_LINE_HEIGHT = 140;
const MAX_PLUMB_ROTATE_DEG = 20;

/** Normalized tilt magnitude below which the leveler counts as "level" and turns green. */
const LEVEL_THRESHOLD_NORM = 0.1;
const COLOR_STOPS = ['#00e676', '#00e676', '#ff5252'] as const;

export interface GyroscopicLevelerProps {
  mode: OrientationMode;
  /** Device tilt error, normalized to [-1, 1] per axis - see ProductAnalysis.tilt. */
  tiltX: SharedValue<number>;
  tiltY: SharedValue<number>;
}

/**
 * Ultra-simple gyroscopic leveler that swaps shape by shooting mode:
 *  - FLAT (garment laid flat): two concentric circles that merge into one
 *    when the phone is level.
 *  - HANGER (garment on a hanger/wall): a plumb line - a fixed vertical
 *    reference next to a live line that rotates with roll, aligning with
 *    the reference when the phone (and so the garment's Y axis) is plumb.
 */
export function GyroscopicLeveler({ mode, tiltX, tiltY }: GyroscopicLevelerProps) {
  return mode === 'HANGER' ? <PlumbLine tiltX={tiltX} /> : <FlatLeveler tiltX={tiltX} tiltY={tiltY} />;
}

function FlatLeveler({ tiltX, tiltY }: { tiltX: SharedValue<number>; tiltY: SharedValue<number> }) {
  const color = useDerivedValue(() => {
    const magnitude = distance(tiltX.value, tiltY.value);
    return interpolateColor(Math.min(magnitude, 1), [0, LEVEL_THRESHOLD_NORM, 1], COLOR_STOPS);
  });

  const innerStyle = useAnimatedStyle(() => ({
    borderColor: color.value,
    transform: [{ translateX: tiltX.value * MAX_RING_OFFSET }, { translateY: tiltY.value * MAX_RING_OFFSET }],
  }));

  return (
    <View style={styles.flatWrapper} pointerEvents="none">
      <View style={styles.outerRing} />
      <Animated.View style={[styles.innerRing, innerStyle]} />
    </View>
  );
}

function PlumbLine({ tiltX }: { tiltX: SharedValue<number> }) {
  const color = useDerivedValue(() =>
    interpolateColor(Math.min(Math.abs(tiltX.value), 1), [0, LEVEL_THRESHOLD_NORM, 1], COLOR_STOPS),
  );

  const liveLineStyle = useAnimatedStyle(() => ({
    backgroundColor: color.value,
    transform: [{ rotate: `${tiltX.value * MAX_PLUMB_ROTATE_DEG}deg` }],
  }));

  return (
    <View style={styles.plumbWrapper} pointerEvents="none">
      <View style={styles.plumbReference} />
      <Animated.View style={[styles.plumbLive, liveLineStyle]} />
    </View>
  );
}

const styles = StyleSheet.create({
  flatWrapper: {
    position: 'absolute',
    top: 72,
    alignSelf: 'center',
    width: LEVEL_SIZE,
    height: LEVEL_SIZE,
    alignItems: 'center',
    justifyContent: 'center',
  },
  outerRing: {
    position: 'absolute',
    width: LEVEL_SIZE,
    height: LEVEL_SIZE,
    borderRadius: LEVEL_SIZE / 2,
    borderWidth: RING_THICKNESS,
    borderColor: 'rgba(255, 255, 255, 0.5)',
    backgroundColor: 'rgba(0, 0, 0, 0.2)',
  },
  innerRing: {
    position: 'absolute',
    width: LEVEL_SIZE * 0.55,
    height: LEVEL_SIZE * 0.55,
    borderRadius: (LEVEL_SIZE * 0.55) / 2,
    borderWidth: RING_THICKNESS,
  },
  plumbWrapper: {
    position: 'absolute',
    top: 60,
    alignSelf: 'center',
    width: 40,
    height: PLUMB_LINE_HEIGHT,
    alignItems: 'center',
    justifyContent: 'center',
  },
  plumbReference: {
    position: 'absolute',
    width: StyleSheet.hairlineWidth * 2,
    height: PLUMB_LINE_HEIGHT,
    backgroundColor: 'rgba(255, 255, 255, 0.4)',
  },
  plumbLive: {
    position: 'absolute',
    width: 4,
    height: PLUMB_LINE_HEIGHT,
    borderRadius: 2,
  },
});
