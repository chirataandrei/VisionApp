#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace visionapp {

/** How the raw frame buffer handed to analyzeFrame() is laid out. */
enum class PixelLayout {
  /** Single-channel 8-bit — e.g. the Y plane of a YUV 4:2:0 frame. */
  GRAY8,
  /** 32-bit BGRA (iOS kCVPixelFormatType_32BGRA). */
  BGRA8888,
  /** 32-bit RGBA (Android RGBA_8888). */
  RGBA8888,
};

/** Coarse exposure classification derived from the frame's brightness histogram. */
enum class ExposureWarning {
  None = 0,
  TooDark = 1,
  TooBright = 2,
};

struct AnalysisResult {
  bool found = false;
  double centroidX = -1.0;
  double centroidY = -1.0;
  double contourArea = 0.0;
  int frameWidth = 0;
  int frameHeight = 0;
  double pitchDegrees = 0.0;
  double rollDegrees = 0.0;
  /** Wall-clock time the C++ pipeline spent on the last processed frame, in ms. */
  double latencyMs = 0.0;
  /** False when this call skipped processing and returned the cached detection. */
  bool processed = false;
  /** Bounding box of the largest detected contour, in full-resolution frame pixel space (all 0 if not found). */
  double boundingBoxX = 0.0;
  double boundingBoxY = 0.0;
  double boundingBoxWidth = 0.0;
  double boundingBoxHeight = 0.0;
  /** Coarse over/under-exposure classification of the current frame. */
  ExposureWarning exposureWarning = ExposureWarning::None;
};

/**
 * Runs the product-detection pipeline on a raw camera frame buffer
 * (zero-copy pointer into the camera's pixel data):
 *
 *   downscale → grayscale → adaptive local-contrast threshold → morphological
 *   close → largest contour → EMA-smoothed centroid
 *
 * and combines the result with the current sensor-fusion pitch/roll.
 * Centroid coordinates are in full-resolution frame pixel space, and are
 * exponentially smoothed frame-to-frame to damp jitter in the on-screen
 * alignment arrow; the tracking ROI itself still follows the raw (unsmoothed)
 * detection so it can't lag behind a fast-moving object.
 *
 * Performance: only every 3rd call runs the pipeline (others return the
 * cached detection with fresh pitch/roll). Downscaling to ~320 px happens
 * before any other pixel work, using area-averaging (INTER_AREA) rather than
 * nearest-neighbor sampling, so per-frame cost scales with camera resolution
 * (every full-resolution pixel is read) in exchange for not aliasing fine
 * fabric patterns into moiré noise; the search is restricted to a region of
 * interest around the previous detection when one exists.
 *
 * Not thread-safe across frames — VisionCamera invokes the frame processor
 * from a single camera thread, which this relies on for its tracking state.
 */
AnalysisResult analyzeFrame(const uint8_t* data,
                            int width,
                            int height,
                            size_t bytesPerRow,
                            PixelLayout layout);

/**
 * Clears the ROI-tracking state and cached detection (frame counter, region
 * of interest, last result). Call this whenever the camera session changes
 * (device switch, restart) so the new session doesn't inherit stale tracking
 * state from a previous, unrelated stream of frames.
 *
 * Same threading constraint as analyzeFrame(): must be called from the
 * camera thread, never concurrently with analyzeFrame().
 */
void resetPipelineState();

/**
 * Crops the photo file at `sourcePath` to `boxX/Y/Width/Height` and writes
 * the result as a JPEG to `destPath`.
 *
 * The box is given in the same pixel space as `analysisFrameWidth` x
 * `analysisFrameHeight` (i.e. AnalysisResult::boundingBox* together with
 * AnalysisResult::frameWidth/frameHeight from the detection that produced
 * it) and is scaled proportionally into the photo's own resolution. This
 * assumes the photo and the analysis frame share the same aspect ratio and
 * orientation, which holds for a given VisionCamera device's frame-processor
 * and photo streams but is not guaranteed identical on every device/OS
 * combination - verify on-device.
 *
 * Returns false if the source photo can't be read, or if the box has zero
 * area after being clamped to the photo bounds.
 *
 * Not part of the per-frame pipeline: safe to call from any thread, and does
 * not touch or depend on PipelineState.
 */
bool cropPhotoToBoundingBox(const std::string& sourcePath,
                             const std::string& destPath,
                             double boxX,
                             double boxY,
                             double boxWidth,
                             double boxHeight,
                             int analysisFrameWidth,
                             int analysisFrameHeight);

} // namespace visionapp
