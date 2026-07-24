import React from 'react';
import { StyleSheet, Text, View } from 'react-native';

export interface WorkflowStepperProps {
  /** Step labels, in order (e.g. ['Poza de Fata', 'Poza de Spate', ...]). */
  steps: readonly string[];
  /** Index of the current step; equal to steps.length once all are done. */
  currentIndex: number;
}

/** Step indicator for the Vinted multi-photo listing workflow (front, back, label, defects, ...). */
export function WorkflowStepper({ steps, currentIndex }: WorkflowStepperProps) {
  const isDone = currentIndex >= steps.length;

  return (
    <View style={styles.container} pointerEvents="none">
      <Text style={styles.label}>
        {isDone ? 'Toate pozele au fost realizate' : `Pasul ${currentIndex + 1}/${steps.length}: ${steps[currentIndex]}`}
      </Text>
      <View style={styles.dotsRow}>
        {steps.map((step, index) => (
          <View
            key={step}
            style={[styles.dot, index < currentIndex && styles.dotDone, index === currentIndex && styles.dotActive]}
          />
        ))}
      </View>
    </View>
  );
}

const DOT_SIZE = 8;

const styles = StyleSheet.create({
  container: {
    position: 'absolute',
    top: 16,
    left: 16,
    right: 16,
    alignItems: 'center',
  },
  label: {
    color: 'white',
    fontWeight: '600',
    fontSize: 14,
    textShadowColor: 'rgba(0, 0, 0, 0.6)',
    textShadowRadius: 4,
    marginBottom: 8,
  },
  dotsRow: {
    flexDirection: 'row',
    gap: 8,
  },
  dot: {
    width: DOT_SIZE,
    height: DOT_SIZE,
    borderRadius: DOT_SIZE / 2,
    backgroundColor: 'rgba(255, 255, 255, 0.35)',
  },
  dotDone: {
    backgroundColor: '#00e676',
  },
  dotActive: {
    backgroundColor: 'white',
  },
});
