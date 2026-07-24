import React from 'react';
import { StyleSheet, Text } from 'react-native';

import type { ExposureWarning } from '../frameProcessors/productAnalyzer';

export interface QualityBadgesProps {
  exposureWarning: ExposureWarning;
}

/** Image-quality warnings surfaced to the user before they take the photo. */
export function QualityBadges({ exposureWarning }: QualityBadgesProps) {
  if (exposureWarning === 'none') {
    return null;
  }
  return (
    <Text style={styles.warningText}>
      {exposureWarning === 'dark' ? '⚠ Imagine prea întunecată' : '⚠ Imagine supraexpusă'}
    </Text>
  );
}

const styles = StyleSheet.create({
  warningText: {
    color: '#ffd740',
    fontWeight: '600',
    marginTop: 4,
  },
});
