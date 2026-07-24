#pragma once

#include <cstddef>
#include <cstdint>

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
};

/**
 * Runs the product-detection pipeline on a raw camera frame buffer
 * (zero-copy pointer into the camera's pixel data):
 *
 *   downscale → grayscale → Gaussian blur → Canny edges → largest contour → centroid
 *
 * and combines the result with the current sensor-fusion pitch/roll.
 * Centroid coordinates are in full-resolution frame pixel space.
 *
 * Performance: only every 3rd call runs the pipeline (others return the
 * cached detection with fresh pitch/roll). Downscaling to ~320 px happens
 * before any other pixel work and uses nearest-neighbor sampling, so per-frame
 * cost is independent of camera resolution; the search is restricted to a
 * region of interest around the previous detection when one exists.
 *
 * Not thread-safe across frames — VisionCamera invokes the frame processor
 * from a single camera thread, which this relies on for its tracking state.
 */
AnalysisResult analyzeFrame(const uint8_t* data,
                            int width,
                            int height,
                            size_t bytesPerRow,
                            PixelLayout layout);

} // namespace visionapp
