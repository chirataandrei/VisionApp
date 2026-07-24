import type { Frame } from 'react-native-vision-camera';
import { VisionCameraProxy } from 'react-native-vision-camera';

import type { Box } from '../utils/geometry';

/**
 * Coarse device-vs-product positioning verdict, checked in strict priority
 * order (only the single most impactful problem is ever surfaced):
 *   1. Nothing significant in frame (or the centering variant below) -> NOT_CENTERED
 *   2. Bounding box touches the frame edge                           -> CUT_OFF_MARGINS
 *   3. Centroid too far from frame center                            -> NOT_CENTERED
 *   4. Phone tilted (only checked once framing + centering pass)     -> PHONE_TILTED
 *   5. All of the above pass                                         -> OK
 * NOT_CENTERED covers two distinct situations (nothing detected yet, or a
 * detected product that's off-center) - `message` disambiguates.
 */
export type AnalysisStatus = 'NOT_CENTERED' | 'CUT_OFF_MARGINS' | 'PHONE_TILTED' | 'OK';

/** Coarse exposure classification of the current frame. */
export type LightingState = 'TOO_DARK' | 'OVEREXPOSED' | 'GOOD';

/**
 * Which physical setup the device is being held in, from pitch alone: FLAT
 * (phone roughly horizontal, shooting a garment laid flat) vs. HANGER (phone
 * roughly vertical, shooting a garment on a hanger/wall). Selects which
 * leveling widget the UI shows.
 */
export type OrientationMode = 'FLAT' | 'HANGER';

/** A simplified 2D transform vector, normalized to [-1.0, 1.0] on each axis. */
export interface Vector2 {
  dx: number;
  dy: number;
}

/**
 * Raw numeric telemetry from the native pipeline - exact pixel/degree
 * readings, only ever populated in debug builds (see ProductAnalyzer.h's
 * AnalysisResult doc comment). Production UI must be built from the
 * states/vectors/score on ProductAnalysis instead; this is for an optional
 * on-device debug readout only.
 */
export interface ProductAnalysisDebug {
  /** EMA-smoothed centroid of the largest contour, in frame pixel coordinates. */
  centroid: { x: number; y: number };
  /** Area of the largest contour in pixels². */
  contourArea: number;
  /** Device pitch in degrees (complementary-filtered accel + gyro). */
  pitch: number;
  /** Device roll in degrees. */
  roll: number;
}

/**
 * Result of the native C++ analysis pipeline
 * (grayscale → downscale → adaptive local-contrast threshold →
 * morphological close → largest contour → EMA-smoothed centroid →
 * status/message priority hierarchy, fused with accelerometer/gyroscope
 * pitch & roll).
 */
export interface ProductAnalysis {
  /** Whether a sufficiently large contour was found in the frame. */
  found: boolean;
  status: AnalysisStatus;
  /**
   * Short Romanian instruction matching `status` (e.g. "Centrează haina"),
   * finer-grained than `status` alone - the authoritative text; UI should
   * show it directly rather than re-deriving text from `status`.
   */
  message: string;
  /** True only when status is OK AND lighting is good - the frame is genuinely worth auto-capturing. */
  isReadyForCapture: boolean;
  lightingState: LightingState;
  orientationMode: OrientationMode;
  /** Centroid offset from the frame center, normalized to [-1, 1] per axis. */
  normalizedDx: number;
  normalizedDy: number;
  /** Device tilt error, normalized to [-1, 1] per axis. Drives the on-screen bubble level. */
  tilt: Vector2;
  frameWidth: number;
  frameHeight: number;
  /**
   * Bounding box of the largest detected contour, in frame pixel coordinates
   * (all 0 if not found). Use together with frameWidth/frameHeight to crop a
   * captured photo to the detected product (see ProductCropModule). Not
   * gated behind debug: cropping is a production feature, not telemetry.
   */
  boundingBox: Box;
  /** C++ pipeline execution time for the last processed frame, in ms. */
  latencyMs: number;
  /**
   * False when this frame was skipped (every 3rd frame is processed) and the
   * detection fields are the cached values from the last processed frame.
   */
  processed: boolean;
  /** Raw numeric telemetry, present only in debug builds. */
  debug?: ProductAnalysisDebug;
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
