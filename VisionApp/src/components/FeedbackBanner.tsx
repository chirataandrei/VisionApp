import React from 'react';
import { StyleSheet, Text, View } from 'react-native';

import type { ProductAnalysis } from '../frameProcessors/productAnalyzer';

/**
 * Reduces the full analysis down to a single, max-3-word instruction. The
 * native pipeline already provides `message` for the alignment/framing/tilt
 * priority hierarchy (see AnalysisStatus) - lighting sits outside that
 * hierarchy (an independent, image-only signal), so it's checked here first,
 * ahead of native's text, since nothing else is readable if it's too
 * dark/bright. "Nothing detected yet" is native's own top-priority case
 * (status NOT_CENTERED, message "Așează haina în cadru"), so it doesn't need
 * special-casing here.
 */
function feedbackMessage(analysis: ProductAnalysis | null): string {
  if (analysis == null || !analysis.found) {
    return 'Așează haina în cadru';
  }
  if (analysis.lightingState === 'TOO_DARK') {
    return 'Mărește lumina';
  }
  if (analysis.lightingState === 'OVEREXPOSED') {
    return 'Redu lumina';
  }
  return analysis.message;
}

export interface FeedbackBannerProps {
  analysis: ProductAnalysis | null;
}

/**
 * Short natural-language instruction, positioned just above the shutter
 * button/capture countdown ring - right where the user's attention already
 * is when they're about to take the photo, rather than up at the top with
 * the step banner (see WorkflowStepper).
 */
export function FeedbackBanner({ analysis }: FeedbackBannerProps) {
  return (
    <View style={styles.banner} pointerEvents="none">
      <Text style={styles.text}>{feedbackMessage(analysis)}</Text>
    </View>
  );
}

const styles = StyleSheet.create({
  banner: {
    position: 'absolute',
    bottom: 140,
    left: 16,
    right: 16,
    alignItems: 'center',
    paddingHorizontal: 20,
    paddingVertical: 10,
    borderRadius: 20,
    backgroundColor: 'rgba(0, 0, 0, 0.6)',
  },
  text: {
    color: 'white',
    fontWeight: '700',
    fontSize: 16,
  },
});
