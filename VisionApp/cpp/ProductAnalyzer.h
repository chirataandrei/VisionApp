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

/**
 * Describes one 8-bit chroma (U or V) plane of a YUV 4:2:0 frame, passed
 * alongside a GRAY8 luma buffer so analyzeFrame can build a color-saturation
 * signal without needing a full color (BGRA/RGBA) buffer. General enough to
 * cover both platforms' native layouts with zero repacking on the bridge
 * side:
 *  - iOS's biplanar CbCr: U and V share one physical buffer at different
 *    byte offsets (U at offset 0, V at offset 1), both with pixelStride 2.
 *  - Android's YUV_420_888: independent U/V planes; pixelStride is 1 (fully
 *    planar) or 2 (interleaved, NV-style) depending on device/vendor -
 *    Android's own Image.Plane already reports which, per plane.
 * `data == nullptr` means "not available"; analyzeFrame degrades gracefully
 * to edge-only detection (its pre-existing behavior) in that case.
 */
struct ChromaPlane {
  const uint8_t* data = nullptr;
  size_t rowStride = 0;
  int pixelStride = 1;
};

/**
 * Coarse device-vs-product positioning verdict for the current frame,
 * checked in strict priority order - only the single most impactful problem
 * is ever surfaced:
 *   1. Nothing significant in frame (or NotCentered's centering variant) -> NotCentered
 *   2. Bounding box touches the frame edge AND is large enough to
 *      plausibly be outgrowing the frame (a small box merely brushing
 *      the edge falls through to check 3 instead)                       -> CutOffMargins
 *   3. Centroid too far from frame center, on either axis independently -> NotCentered
 *   4. Phone tilted (only checked once framing + centering already pass) -> PhoneTilted
 *   5. All of the above pass                                             -> Ok
 * NotCentered covers two distinct situations (nothing detected yet, or a
 * detected product that's off-center) - AnalysisResult::message disambiguates.
 */
enum class AnalysisStatus {
  NotCentered,
  CutOffMargins,
  PhoneTilted,
  Ok,
};

/** Coarse exposure classification derived from the frame's brightness histogram. */
enum class LightingState {
  TooDark,
  Overexposed,
  Good,
};

/**
 * Which physical setup the device is being held in, from device pitch:
 * flat (phone roughly horizontal, shooting a garment laid on a surface) vs.
 * hanger (phone roughly vertical, shooting a garment on a hanger/wall). The
 * UI swaps its leveling widget between the two - a flat-lay bubble level
 * doesn't make sense held vertically, and vice versa.
 *
 * Classification happens in SensorFusion (see Attitude::isFlatMode), not
 * here, because it needs hysteresis around the ~45° boundary to avoid
 * flapping between modes when the phone is held near that angle - and
 * hysteresis needs to persist across calls, which is state this header's
 * classifyAlignment-driving logic deliberately doesn't carry.
 */
enum class OrientationMode {
  Flat,
  Hanger,
};

/** A simplified 2D transform vector, normalized to [-1.0, 1.0] on each axis. */
struct Vector2 {
  double dx = 0.0;
  double dy = 0.0;
};

struct AnalysisResult {
  bool found = false;
  int frameWidth = 0;
  int frameHeight = 0;
  /** Wall-clock time the C++ pipeline spent on the last processed frame, in ms. */
  double latencyMs = 0.0;
  /** False when this call skipped processing and returned the cached detection. */
  bool processed = false;

  // ---- Structured state: what the pipeline actually wants to tell the UI ----

  AnalysisStatus status = AnalysisStatus::NotCentered;
  /**
   * Short Romanian instruction matching `status` (e.g. "Centrează haina"),
   * finer-grained than `status` alone: `NotCentered` covers two distinct
   * situations (nothing detected yet vs. an off-center product) that need
   * different copy, so this is the authoritative text - UI should show it
   * directly rather than re-deriving text from `status`.
   */
  std::string message;
  /**
   * Same information as `message`, as a small int (0-4) instead of a string:
   * JNI can't marshal a std::string through the primitive array bridge to
   * Android, so ProductAnalyzerPlugin.kt keeps its own copy of these five
   * strings and looks this up. iOS/C++ callers should just use `message`.
   */
  int messageCode = 0;
  /** True only when status == Ok AND lighting is good - the frame is genuinely worth auto-capturing. */
  bool isReadyForCapture = false;
  LightingState lightingState = LightingState::Good;
  /** Flat-lay vs. on-hanger, from device pitch alone. Selects which leveling widget the UI shows. */
  OrientationMode orientationMode = OrientationMode::Flat;
  /** Centroid offset from the frame center, normalized to [-1, 1] per axis (0 if not found). Drives the on-screen direction arrow. */
  double normalizedDx = 0.0;
  double normalizedDy = 0.0;
  /** Device tilt error, normalized to [-1, 1] per axis (dx from roll, dy from pitch error; {0, 0} when level). Drives the on-screen bubble level. */
  Vector2 tilt;

  /**
   * Bounding box of the largest detected contour, in full-resolution frame
   * pixel space (all 0 if not found). Not just UI telemetry: cropPhotoToBoundingBox
   * needs this together with frameWidth/frameHeight to crop the captured
   * photo, so it's always populated regardless of build type.
   */
  double boundingBoxX = 0.0;
  double boundingBoxY = 0.0;
  double boundingBoxWidth = 0.0;
  double boundingBoxHeight = 0.0;

#ifdef DEBUG
  // ---- Raw numeric telemetry: debug builds only. The states/vectors/score
  // above are what production UI consumes; these exact pixel/degree/area
  // readings exist only for an on-device debug readout and must never reach
  // a release build's JS layer (see the bridge plugins' own #ifdef DEBUG /
  // BuildConfig.DEBUG / __DEV__ gates). ----
  double centroidX = -1.0;
  double centroidY = -1.0;
  double contourArea = 0.0;
  double pitchDegrees = 0.0;
  double rollDegrees = 0.0;
#endif
};

/**
 * Runs the product-detection pipeline on a raw camera frame buffer
 * (zero-copy pointer into the camera's pixel data):
 *
 *   downscale → grayscale → adaptive local-contrast threshold (OR'd with a
 *   color-saturation mask, when chroma data is available - see ChromaPlane)
 *   → morphological open+close → smart contour scoring → EMA-smoothed
 *   centroid → status/message priority hierarchy → normalized transform
 *   vectors
 *
 * and combines the result with the current sensor-fusion pitch/roll.
 * Centroid coordinates are computed in full-resolution frame pixel space and
 * exponentially smoothed frame-to-frame to damp jitter in the on-screen
 * alignment arrow (the tracking ROI itself still follows the raw, unsmoothed
 * detection so it can't lag behind a fast-moving object), but that raw
 * position never leaves this translation unit in a release build: it's
 * reduced to AnalysisResult's normalized offset/tilt vectors and enums
 * before crossing into the bridge plugins' release-build JS payload.
 *
 * Performance, tuned for 60 FPS on-device: only every 3rd call runs the
 * pipeline (others return the cached detection with fresh pitch/roll).
 * Downscaling to a fixed 240 px width happens before any other pixel work,
 * using nearest-neighbor sampling (INTER_NEAREST) - a deliberate speed-over-
 * quality trade versus area-averaging: it doesn't read every full-resolution
 * source pixel, so cost is roughly constant regardless of camera resolution,
 * at the cost of some aliasing on fine fabric patterns (acceptable - the
 * adaptive local-contrast threshold below already tolerates a fair amount of
 * texture noise). The search is restricted to a region of interest around
 * the previous detection when one exists, and all intermediate cv::Mat
 * buffers (blur/threshold/morphology scratch space, not just the frame
 * itself) are members of the persistent pipeline state, reused frame to
 * frame instead of being reallocated on every call. The optional chroma
 * planes are sampled directly at the already-downscaled working resolution
 * (never materialized at native chroma resolution), so supplying them costs
 * roughly the same order of magnitude of work as the luma downscale itself,
 * not the multiple-hundred-thousand-pixel cost their native resolution
 * would otherwise imply.
 *
 * Not thread-safe across frames — VisionCamera invokes the frame processor
 * from a single camera thread, which this relies on for its tracking state.
 */
AnalysisResult analyzeFrame(const uint8_t* data,
                            int width,
                            int height,
                            size_t bytesPerRow,
                            PixelLayout layout,
                            int chromaWidth = 0,
                            int chromaHeight = 0,
                            ChromaPlane chromaU = {},
                            ChromaPlane chromaV = {});

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
