import type { Frame } from 'react-native-vision-camera';
import { VisionCameraProxy } from 'react-native-vision-camera';

/** Coarse over/under-exposure classification of the current frame. */
export type ExposureWarning = 'none' | 'dark' | 'bright';

/**
 * Result of the native C++ analysis pipeline
 * (grayscale → downscale → adaptive local-contrast threshold →
 * morphological close → largest contour → EMA-smoothed centroid,
 * fused with accelerometer/gyroscope pitch & roll).
 */
export interface ProductAnalysis {
  /** Whether a sufficiently large contour was found in the frame. */
  found: boolean;
  /** EMA-smoothed centroid of the largest contour, in frame pixel coordinates (-1 if not found). */
  centroid: { x: number; y: number };
  /** Area of the largest contour in pixels². */
  contourArea: number;
  frameWidth: number;
  frameHeight: number;
  /**
   * Bounding box of the largest detected contour, in frame pixel coordinates
   * (all 0 if not found). Use together with frameWidth/frameHeight to crop a
   * captured photo to the detected product (see ProductCropModule).
   */
  boundingBox: { x: number; y: number; width: number; height: number };
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
  /** Whether the frame looks too dark or too bright to make a good photo. */
  exposureWarning: ExposureWarning;
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

/**
 * Clears the native pipeline's ROI-tracking state and cached detection.
 * Call this once after the camera session changes (device switch, restart)
 * so the new session doesn't inherit stale tracking state from a previous,
 * unrelated stream of frames. Must be called from the frame processor
 * worklet, since a `Frame` is only available there.
 */
export function resetAnalysisSession(frame: Frame): void {
  'worklet';
  if (plugin == null) {
    throw new Error('Frame processor plugin "analyzeProduct" is not registered!');
  }
  plugin.call(frame, { reset: true });
}
