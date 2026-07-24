import React from 'react';
import { StyleSheet, Text, View } from 'react-native';

export interface WorkflowStepperProps {
  /** Step labels, in order (e.g. ['Poza de Fata', 'Poza de Spate', ...]). */
  steps: readonly string[];
  /** Index of the current step; equal to steps.length once all are done. */
  currentIndex: number;
}

/**
 * Status banner for the Vinted multi-photo listing workflow: a frosted-glass
 * pill, clearly below the status bar, showing the current step ("Poza de
 * Spate | Pasul 2/4") plus a dot-row progress indicator. No blur library is
 * installed in this project, so the "glass" look is approximated with a
 * translucent fill, a light border highlight, and a soft shadow rather than
 * a true backdrop blur.
 */
export function WorkflowStepper({ steps, currentIndex }: WorkflowStepperProps) {
  const isDone = currentIndex >= steps.length;

  return (
    <View style={styles.card} pointerEvents="none">
      <Text style={styles.label}>
        {isDone ? (
          'Toate pozele au fost realizate'
        ) : (
          <>
            <Text style={styles.labelStrong}>{steps[currentIndex]}</Text>
            <Text style={styles.labelDim}> {'•'} Pasul </Text>
            <Text style={styles.labelStrong}>{currentIndex + 1}</Text>
            <Text style={styles.labelDim}>/{steps.length}</Text>
          </>
        )}
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
  card: {
    position: 'absolute',
    top: 20,
    alignSelf: 'center',
    maxWidth: '88%',
    alignItems: 'center',
    paddingHorizontal: 18,
    paddingVertical: 12,
    borderRadius: 22,
    backgroundColor: 'rgba(255, 255, 255, 0.14)',
    borderWidth: 1,
    borderColor: 'rgba(255, 255, 255, 0.28)',
    shadowColor: '#000',
    shadowOpacity: 0.25,
    shadowRadius: 12,
    shadowOffset: { width: 0, height: 4 },
    elevation: 6,
  },
  label: {
    color: 'white',
    fontSize: 14,
    textAlign: 'center',
    marginBottom: 8,
  },
  labelStrong: {
    fontWeight: '700',
  },
  labelDim: {
    color: 'rgba(255, 255, 255, 0.75)',
    fontWeight: '400',
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
