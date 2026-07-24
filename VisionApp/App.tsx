import React, { useCallback, useEffect, useState } from 'react';
import { StatusBar, StyleSheet, Text, View, useWindowDimensions } from 'react-native';
import { useSharedValue, withSpring, withTiming } from 'react-native-reanimated';
import {
  Camera,
  useCameraDevice,
  useCameraPermission,
  useFrameProcessor,
} from 'react-native-vision-camera';
import { useRunOnJS } from 'react-native-worklets-core';

import { AlignmentOverlay } from './src/components/AlignmentOverlay';
import type { ProductAnalysis } from './src/frameProcessors/productAnalyzer';
import { analyzeProduct } from './src/frameProcessors/productAnalyzer';

const SPRING = { damping: 20, stiffness: 180 };

function App() {
  const device = useCameraDevice('back');
  const { hasPermission, requestPermission } = useCameraPermission();
  const [analysis, setAnalysis] = useState<ProductAnalysis | null>(null);
  const { width: screenWidth, height: screenHeight } = useWindowDimensions();

  // Reanimated shared values driving the overlay on the UI thread.
  const pitch = useSharedValue(0);
  const roll = useSharedValue(0);
  const found = useSharedValue(0);
  const centroidX = useSharedValue(screenWidth / 2);
  const centroidY = useSharedValue(screenHeight / 2);

  useEffect(() => {
    if (!hasPermission) {
      requestPermission();
    }
  }, [hasPermission, requestPermission]);

  const onAnalysis = useRunOnJS(
    useCallback(
      (result: ProductAnalysis) => {
        // Detection fields only change on processed frames (every 3rd);
        // skip the React re-render for the cached in-between frames.
        if (result.processed) {
          setAnalysis(result);
        }
        pitch.value = withSpring(result.pitch, SPRING);
        roll.value = withSpring(result.roll, SPRING);
        found.value = withTiming(result.found ? 1 : 0, { duration: 150 });
        if (result.found && result.frameWidth > 0 && result.frameHeight > 0) {
          // Naive frame→screen mapping: camera sensors deliver landscape
          // frames while the app runs in portrait, so treat the frame as
          // rotated 90° clockwise.
          const x = (1 - result.centroid.y / result.frameHeight) * screenWidth;
          const y = (result.centroid.x / result.frameWidth) * screenHeight;
          centroidX.value = withSpring(x, SPRING);
          centroidY.value = withSpring(y, SPRING);
        }
      },
      [pitch, roll, found, centroidX, centroidY, screenWidth, screenHeight],
    ),
    [screenWidth, screenHeight],
  );

  const frameProcessor = useFrameProcessor(
    frame => {
      'worklet';
      // The frame is handed to native C++ by reference via JSI — zero copies,
      // nothing crosses the old React Native bridge. C++ processes every 3rd
      // frame and answers from its cache in between, so calling it per-frame
      // is cheap and keeps pitch/roll fresh.
      const result = analyzeProduct(frame);
      onAnalysis(result);
    },
    [onAnalysis],
  );

  if (!hasPermission) {
    return (
      <View style={styles.center}>
        <Text style={styles.infoText}>Camera permission is required.</Text>
      </View>
    );
  }
  if (device == null) {
    return (
      <View style={styles.center}>
        <Text style={styles.infoText}>No back camera found.</Text>
      </View>
    );
  }

  return (
    <View style={styles.container}>
      <StatusBar barStyle="light-content" />
      <Camera
        style={StyleSheet.absoluteFill}
        device={device}
        isActive={true}
        pixelFormat="yuv"
        frameProcessor={frameProcessor}
      />
      <AlignmentOverlay pitch={pitch} roll={roll} found={found} centroidX={centroidX} centroidY={centroidY} />
      <View style={styles.panel} pointerEvents="none">
        <Text style={styles.panelTitle}>Product Analyzer (C++ / OpenCV)</Text>
        {analysis == null ? (
          <Text style={styles.panelText}>Waiting for frames…</Text>
        ) : (
          <>
            <Text style={styles.panelText}>
              Pitch: {analysis.pitch.toFixed(1)}°   Roll: {analysis.roll.toFixed(1)}°   C++:{' '}
              {analysis.latencyMs.toFixed(1)} ms
            </Text>
            <Text style={styles.panelText}>
              {analysis.found
                ? `Centroid: (${analysis.centroid.x.toFixed(0)}, ${analysis.centroid.y.toFixed(0)}) ` +
                  `in ${analysis.frameWidth}×${analysis.frameHeight}  ·  area ${Math.round(analysis.contourArea)}px²`
                : 'No product contour detected'}
            </Text>
          </>
        )}
      </View>
    </View>
  );
}

const styles = StyleSheet.create({
  container: {
    flex: 1,
    backgroundColor: 'black',
  },
  center: {
    flex: 1,
    alignItems: 'center',
    justifyContent: 'center',
    backgroundColor: 'black',
  },
  infoText: {
    color: 'white',
    fontSize: 16,
  },
  panel: {
    position: 'absolute',
    left: 16,
    right: 16,
    bottom: 48,
    borderRadius: 12,
    backgroundColor: 'rgba(0, 0, 0, 0.6)',
    padding: 12,
  },
  panelTitle: {
    color: 'white',
    fontWeight: 'bold',
    marginBottom: 4,
  },
  panelText: {
    color: 'white',
    fontVariant: ['tabular-nums'],
  },
});

export default App;
