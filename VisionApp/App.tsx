import React, { useEffect, useRef } from 'react';
import { StatusBar, StyleSheet, Text, View } from 'react-native';
import { Camera, useCameraDevice, useCameraPermission } from 'react-native-vision-camera';

import { AlignmentOverlay } from './src/components/AlignmentOverlay';
import { CaptureCountdownRing } from './src/components/CaptureCountdownRing';
import { FeedbackBanner } from './src/components/FeedbackBanner';
import { ShutterButton } from './src/components/ShutterButton';
import { WorkflowStepper } from './src/components/WorkflowStepper';
import { useAutoCapture } from './src/hooks/useAutoCapture';
import { usePhotoWorkflow } from './src/hooks/usePhotoWorkflow';
import { useProductAlignment } from './src/hooks/useProductAlignment';

/**
 * Master switch for any on-screen debug UI (raw pipeline telemetry, latency
 * readouts, etc.) - everything of that kind must be gated behind this, not
 * shown unconditionally. Flip to `true` locally when you need it; never
 * commit it as `true`. Independent of `__DEV__`: a debug build should still
 * look like the production camera UI by default.
 */
const ENABLE_DEBUG_UI = false;

/** How long (ms) alignment/lighting/framing must all stay simultaneously ideal before a photo is captured automatically. */
const AUTO_CAPTURE_ALIGNED_MS = 1000;

/** Vinted multi-photo listing workflow, in order. */
const PHOTO_STEPS = ['Poza de Fata', 'Poza de Spate', 'Eticheta Marime/Brand', 'Defecte/Detalii'] as const;

function App() {
  const device = useCameraDevice('back');
  const { hasPermission, requestPermission } = useCameraPermission();
  const camera = useRef<Camera>(null);

  useEffect(() => {
    if (!hasPermission) {
      requestPermission();
    }
  }, [hasPermission, requestPermission]);

  const { frameProcessor, found, isOk, perfect, analysis, lastDetectionRef } = useProductAlignment(device?.id);

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
      <AlignmentOverlay isOk={isOk} found={found} perfect={perfect} onAlignedChange={handleAlignedChange} />
      <WorkflowStepper steps={PHOTO_STEPS} currentIndex={stepIndex} />
      {isDone ? (
        <View style={styles.doneBanner} pointerEvents="none">
          <Text style={styles.doneText}>{Object.keys(capturedPhotos).length} poze salvate</Text>
        </View>
      ) : (
        <FeedbackBanner analysis={analysis} />
      )}
      <CaptureCountdownRing perfect={perfect} durationMs={AUTO_CAPTURE_ALIGNED_MS} />
      <ShutterButton onPress={capturePhoto} disabled={isCapturing || isDone} />
      {ENABLE_DEBUG_UI && analysis != null && (
        <View style={styles.debugOverlay} pointerEvents="none">
          <Text style={styles.debugText}>
            {analysis.status} · {analysis.latencyMs.toFixed(1)}ms
          </Text>
        </View>
      )}
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
  doneBanner: {
    position: 'absolute',
    bottom: 140,
    left: 16,
    right: 16,
    alignItems: 'center',
    borderRadius: 20,
    backgroundColor: 'rgba(0, 0, 0, 0.6)',
    paddingVertical: 10,
  },
  doneText: {
    color: 'white',
    fontWeight: '600',
  },
  debugOverlay: {
    position: 'absolute',
    top: 8,
    right: 8,
    backgroundColor: 'rgba(0, 0, 0, 0.5)',
    paddingHorizontal: 8,
    paddingVertical: 4,
    borderRadius: 6,
  },
  debugText: {
    color: '#00e676',
    fontSize: 11,
    fontVariant: ['tabular-nums'],
  },
});

export default App;
