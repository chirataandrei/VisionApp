import React from 'react';
import { Pressable, StyleSheet, View } from 'react-native';
import Animated, { useAnimatedStyle, useSharedValue, withSpring } from 'react-native-reanimated';

const PRESS_SCALE = 0.85;
const PRESS_SPRING = { damping: 15, stiffness: 300 };
const RELEASE_SPRING = { damping: 12, stiffness: 200 };

export interface ShutterButtonProps {
  onPress: () => void;
  disabled?: boolean;
}

/** Physical camera shutter button: a white ring that scales down on press. */
export function ShutterButton({ onPress, disabled = false }: ShutterButtonProps) {
  const scale = useSharedValue(1);

  const ringStyle = useAnimatedStyle(() => ({
    transform: [{ scale: scale.value }],
  }));

  return (
    <Pressable
      accessibilityRole="button"
      accessibilityLabel="Capture photo"
      disabled={disabled}
      onPressIn={() => {
        scale.value = withSpring(PRESS_SCALE, PRESS_SPRING);
      }}
      onPressOut={() => {
        scale.value = withSpring(1, RELEASE_SPRING);
      }}
      onPress={onPress}
      style={styles.touchArea}>
      <Animated.View style={[styles.ring, ringStyle, disabled && styles.ringDisabled]}>
        <View style={styles.innerCircle} />
      </Animated.View>
    </Pressable>
  );
}

const RING_SIZE = 76;
const INNER_SIZE = 60;

const styles = StyleSheet.create({
  touchArea: {
    position: 'absolute',
    bottom: 32,
    alignSelf: 'center',
  },
  ring: {
    width: RING_SIZE,
    height: RING_SIZE,
    borderRadius: RING_SIZE / 2,
    borderWidth: 4,
    borderColor: 'white',
    alignItems: 'center',
    justifyContent: 'center',
  },
  ringDisabled: {
    opacity: 0.4,
  },
  innerCircle: {
    width: INNER_SIZE,
    height: INNER_SIZE,
    borderRadius: INNER_SIZE / 2,
    backgroundColor: 'white',
  },
});
