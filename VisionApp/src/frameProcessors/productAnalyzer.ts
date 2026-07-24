import type { Frame } from 'react-native-vision-camera';
import { VisionCameraProxy } from 'react-native-vision-camera';

/**
 * Result of the native C++ analysis pipeline
 * (grayscale → Gaussian blur → Canny → largest contour → centroid,
 * fused with accelerometer/gyroscope pitch & roll).
 */
export interface ProductAnalysis {
  /** Whether a sufficiently large contour was found in the frame. */
  found: boolean;
  /** Centroid of the largest contour, in frame pixel coordinates (-1 if not found). */
  centroid: { x: number; y: number };
  /** Area of the largest contour in pixels². */
  contourArea: number;
  frameWidth: number;
  frameHeight: number;
  /** Device pitch in degrees (complementary-filtered accel + gyro). */
  pitch: number;
  /** Device roll in degrees. */
  roll: number;
  /** C++ pipeline execution time for the last processed frame, in ms. */
  latencyMs: number;
  /**
   * False when this frame was skipped (every 3rd frame is processed) and the
   * detection fields are the cached values from the last processed frame.
   * Pitch/roll are always fresh.
   */
  processed: boolean;
}

const plugin = VisionCameraProxy.initFrameProcessorPlugin('analyzeProduct', {});

/**
 * Runs the native "analyzeProduct" frame processor plugin on a camera frame.
 * The frame is passed by reference through JSI — no serialization, no bridge.
 */
export function analyzeProduct(frame: Frame): ProductAnalysis {
  'worklet';
  if (plugin == null) {
    throw new Error('Frame processor plugin "analyzeProduct" is not registered!');
  }
  return plugin.call(frame) as unknown as ProductAnalysis;
}
