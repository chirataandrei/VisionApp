import React, { useEffect, useRef } from 'react';
import { StatusBar, StyleSheet, Text, View, useWindowDimensions } from 'react-native';
import { Camera, useCameraDevice, useCameraPermission } from 'react-native-vision-camera';

import { AlignmentOverlay } from './src/components/AlignmentOverlay';
import { QualityBadges } from './src/components/QualityBadges';
import { ShutterButton } from './src/components/ShutterButton';
import { WorkflowStepper } from './src/components/WorkflowStepper';
import { useAutoCapture } from './src/hooks/useAutoCapture';
import { usePhotoWorkflow } from './src/hooks/usePhotoWorkflow';
import { useProductAlignment } from './src/hooks/useProductAlignment';

/** How long (ms) `isAligned` must stay true before a photo is captured automatically. */
const AUTO_CAPTURE_ALIGNED_MS = 500;

/** Vinted multi-photo listing workflow, in order. */
const PHOTO_STEPS = ['Poza de Fata', 'Poza de Spate', 'Eticheta Marime/Brand', 'Defecte/Detalii'] as const;

function App() {
  const device = useCameraDevice('back');
  const { hasPermission, requestPermission } = useCameraPermission();
  const { width: screenWidth, height: screenHeight } = useWindowDimensions();
  const camera = useRef<Camera>(null);

  useEffect(() => {
    if (!hasPermission) {
      requestPermission();
    }
  }, [hasPermission, requestPermission]);

  const { frameProcessor, pitch, roll, found, centroidX, centroidY, analysis, lastDetectionRef } =
    useProductAlignment(device?.id, screenWidth, screenHeight);

  const { capturePhoto, isCapturing, stepIndex, isDone, capturedPhotos } = usePhotoWorkflow(
    camera,
    PHOTO_STEPS,
    lastDetectionRef,
  );

  const { handleAlignedChange } = useAutoCapture(capturePhoto, AUTO_CAPTURE_ALIGNED_MS, !isDone);

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
        ref={camera}
        style={StyleSheet.absoluteFill}
        device={device}
        isActive={true}
        photo={true}
        pixelFormat="yuv"
        frameProcessor={frameProcessor}
      />
      <AlignmentOverlay
        pitch={pitch}
        roll={roll}
        found={found}
        centroidX={centroidX}
        centroidY={centroidY}
        onAlignedChange={handleAlignedChange}
      />
      <WorkflowStepper steps={PHOTO_STEPS} currentIndex={stepIndex} />
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
            <QualityBadges exposureWarning={analysis.exposureWarning} />
            {isDone && <Text style={styles.panelText}>{Object.keys(capturedPhotos).length} poze salvate</Text>}
          </>
        )}
      </View>
      <ShutterButton onPress={capturePhoto} disabled={isCapturing || isDone} />
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
    bottom: 140,
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
