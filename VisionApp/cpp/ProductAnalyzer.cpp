#include "ProductAnalyzer.h"

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

#include "SensorFusion.h"

namespace visionapp {

namespace {

constexpr int kGaussianKernelSize = 5;
// Local-background blur radius for the adaptive contrast threshold: cast
// shadows are smooth over a much wider area than genuine fabric/edge detail,
// so comparing each pixel against a blur this wide isolates real edges while
// shadow gradients average into their own local background and vanish.
constexpr int kLocalContrastKernelSize = 31;
// Minimum |pixel - localMean| to count as a foreground edge.
constexpr double kContrastThreshold = 12.0;
// Structuring element used to remove small isolated noise blobs (bright
// window reflections, sensor speckle) BEFORE the close step below - applied
// first so noise never gets a chance to bridge into the product's
// silhouette. Small on purpose: big enough to erase a few stray pixels,
// small enough to leave real garment edges intact.
constexpr int kMorphOpenKernelSize = 3;
// Structuring element used to close gaps between the many small edges a
// patterned/textured garment produces, merging them into one silhouette.
constexpr int kMorphCloseKernelSize = 9;
// EMA smoothing factor for the reported centroid (0 < a <= 1): higher tracks
// the raw detection more closely, lower damps jitter more but adds lag.
constexpr double kCentroidSmoothingAlpha = 0.35;

// ---- Contour scoring (smart selection, see scoreContour) ----

// Fraction of a candidate contour's arc length used as approxPolyDP's
// simplification tolerance: small enough that a curved garment silhouette
// still needs many vertices to approximate within tolerance, large enough
// that an already-straight-edged object (a TV, a shelf, a picture frame)
// collapses down to just its corners.
constexpr double kApproxPolyEpsilonFraction = 0.02;
// A contour that approxPolyDP simplifies down to this few vertices reads as
// a straight-edged rectangular object rather than an organic garment
// silhouette.
constexpr int kLinearShapeMinVertices = 4;
constexpr int kLinearShapeMaxVertices = 6;
// EdgeLinearity assigned to a contour judged linear/rectangular (see
// above) - deliberately not 1.0: a strong penalty (Score *= 1 - 0.9 = 0.1,
// a 90% reduction) rather than an outright disqualification, so a
// genuinely large garment contour that happens to simplify to a low vertex
// count under harsh lighting still has some chance of being picked if
// nothing better is in frame.
constexpr double kLinearShapeEdgeLinearity = 0.9;

// ---- Color saturation mask (see computeSaturationValue) ----

// Chroma-plane magnitude / HSV saturation value above which a pixel counts
// as "colorful" rather than a neutral gray/wood-toned background. HSV
// saturation is 0-255 in OpenCV's 8-bit convention; the chroma-magnitude
// proxy used for GRAY8+chroma frames is deliberately scaled to match this
// same range (see computeSaturationValue), so this one threshold applies to
// both.
constexpr double kSaturationThreshold = 40.0;
// Average saturation (0-255, within a candidate's bounding box) below which
// a contour already flagged looksLinear (see scoreContour) gets an EXTRA,
// compounded penalty on top of kLinearShapeEdgeLinearity - specifically
// targets the case a TV/wall/wood-floor actually is (straight-edged AND
// neutral-toned) without ever touching the score of an organic-shaped
// contour: a plain white/black/gray garment is never looksLinear in the
// first place, so this threshold never applies to it.
constexpr double kLinearShapeSaturationPenaltyThreshold = 30.0;
// EdgeLinearity for a contour that's both looksLinear and low-saturation -
// Score *= 1 - 0.97 = 0.03, roughly a 33x reduction versus the ~10x
// reduction for a linear-but-colorful shape (kLinearShapeEdgeLinearity).
constexpr double kLinearShapeLowSaturationEdgeLinearity = 0.97;

// AnalysisResult::status/message only changes once the newly-classified
// value has stayed consistent for this many consecutive analyzeFrame calls
// - see stabilizeStatus(). Stops a single stray misclassification from
// flickering the on-screen instruction, the alignment guide's color, or
// restarting the auto-capture countdown.
constexpr int kStatusConfirmFrames = 3;

// Brightness histogram bins (8-bit grayscale, so 256 of them).
constexpr int kExposureHistogramBins = 256;
// Intensities below this count toward "shadow" for exposure purposes.
constexpr int kShadowClipBin = 25;
// Intensities at/above this count toward "highlight" for exposure purposes.
constexpr int kHighlightClipBin = 230;
// Fraction of pixels that must fall in the shadow/highlight range to warn.
constexpr double kExposureClipFraction = 0.35;

// Only every Nth call runs the pipeline; the rest reuse the cached result.
constexpr uint64_t kProcessEveryNthFrame = 3;
// Edge detection runs on an image downscaled to a fixed width, regardless of
// camera resolution - see analyzeFrame's doc comment on why this is
// INTER_NEAREST rather than area-averaging. 240px is plenty for contour/
// framing detection (the product only needs to be roughly outlined, not
// finely detailed) and keeps per-frame cost well under 2ms.
constexpr int kTargetProcessingWidth = 240;
// The tracking ROI is the previous bounding box grown by this fraction.
constexpr double kRoiMarginFraction = 0.3;

// ---- Motion-blur bypass (see analyzeFrame) ----

// Variance of the Laplacian - the standard proxy for "how much
// high-frequency detail survives" in an image. A frame blurred by fast
// phone motion or rotation has almost none, so this collapses toward zero;
// below this threshold the frame is too motion-blurred for contour
// detection to mean anything, so it's skipped outright rather than wasting
// a frame budget - and risking a stale/garbage detection - on it. Chosen
// empirically (per the usual OpenCV blur-detection convention); frames with
// genuine focused detail from a 240px-wide downscale typically score
// several times higher than this.
constexpr double kMinBlurVariance = 60.0;
// AnalysisResult::messageCode for the motion-blur bypass - one past the
// last index in kMessages, kept separate from AnalysisStatus's own
// priority hierarchy (see classifyAlignment) since it's a distinct,
// image-quality gate that preempts that hierarchy entirely rather than
// being one more rung in it.
constexpr int kMotionBlurMessageCode = 5;

// ---- Alignment / framing classification ----

// Tilt tolerance (degrees), per axis, for the phone to count as level.
// Deliberately relaxed (was 3°): most garment photos don't need to be
// perfectly level, and being this strict made PHONE_TILTED fire too often.
constexpr double kLevelThresholdDeg = 7.0;
// Tilt (degrees) beyond which the reported tilt vector clamps to magnitude 1.
constexpr double kMaxTiltDeg = 30.0;
// Centroid offset from frame center, as a fraction of the half-frame extent
// on EITHER axis independently, beyond which the product counts as not
// centered (not a combined/Euclidean magnitude - a product that's centered
// vertically but drifted 22% horizontally should still read as off-center).
constexpr double kCenterThresholdNorm = 0.20;
// Minimum contour/bounding-box area, as a fraction of the frame, for
// anything to count as "significant" (too small/far away, or nothing there
// at all, both read as NotCentered, see AnalysisStatus) - this single
// threshold is checked twice, once at the OpenCV level (detectBestContour
// discards a candidate contour outright if it's this small, before it's
// even scored) and once again here against the
// reported bounding box, so a stray bit of background noise can never be
// mistaken for the product AND can never reach the CutOffMargins check
// below with a bogus edge-touching box. The fraction is scale-invariant -
// identical whether measured against the downscaled working image or the
// full-resolution frame, since both scale together.
constexpr double kMinPresenceAreaFraction = 0.12;
// How close (as a fraction of the corresponding frame dimension) the
// bounding box must get to a frame edge to count as touching it.
// Deliberately generous (was 3%): on physical devices, sensor/lens noise
// and background clutter routinely put a contour's edge within a few
// percent of the frame boundary even when the product itself is nowhere
// near actually being cut off, which was spuriously triggering
// CutOffMargins - see the touchesOppositeMargins/kCutOffMinAreaFraction
// doc comments below for the other two layers of defense against the same
// false positive.
constexpr double kFramingEdgeMarginFraction = 0.10;
// CutOffMargins requires the bounding box to touch two OPPOSITE margins at
// once (left+right, or top+bottom) - not merely any single edge. A single
// edge touch is common, ordinary noise (the object drifted a bit far, a
// sleeve brushed the frame boundary); spanning all the way from one side
// of the frame to the other is a much stronger, harder-to-fake signal that
// the product genuinely doesn't fit.
//
// A bounding box meeting that bar only counts as CutOffMargins if it's
// ALSO at least this large a fraction of the frame - a small object that
// merely brushes two edges (e.g. a thin diagonal sliver) isn't "cut off",
// it just hasn't been centered yet; only a genuinely large product that's
// outgrown the frame should read as cut off. Raised well above the
// product's typical in-frame size (was 35%) specifically so ordinary
// framing at a normal shooting distance can never trip this by accident.
constexpr double kCutOffMinAreaFraction = 0.60;

// Romanian instructions matching AnalysisStatus's priority hierarchy, in
// AnalysisResult::messageCode order. Single source of truth: every branch of
// classifyAlignment sets both messageCode and message from this table, so
// they can never disagree.
constexpr const char* kMessages[] = {
    "Așează haina în cadru",    // 0: nothing significant detected
    "Îndepărtează camera",      // 1: cut off at the frame margins
    "Centrează haina",          // 2: off-center
    "Ține telefonul mai drept", // 3: phone tilted
    "Perfect!",                 // 4: ok
    "Mișcare prea rapidă",      // 5: motion-blur bypass, see kMinBlurVariance
};

// Structuring elements for the morphological open/close below. Pure
// functions of compile-time constants, so they're computed once here rather
// than on every call to detectBestContour.
const cv::Mat kMorphOpenKernel =
    cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(kMorphOpenKernelSize, kMorphOpenKernelSize));
const cv::Mat kMorphCloseKernel =
    cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(kMorphCloseKernelSize, kMorphCloseKernelSize));

struct Detection {
  cv::Point2d centroid;   // in the coordinates of the searched image
  double area = 0.0;      // px² in the searched image
  cv::Rect boundingBox;   // in the coordinates of the searched image
};

// Tracking state across frames. VisionCamera calls the frame processor from
// a single camera thread, so no locking is needed here.
struct PipelineState {
  uint64_t frameCounter = 0;
  bool hasRoi = false;
  cv::Rect roi;             // in downscaled-image coordinates
  AnalysisResult lastResult;

  // EMA state for the reported centroid, in full-resolution frame pixel
  // space. Cleared whenever the object is lost so the next detection snaps
  // to place instead of lerping in from a stale position.
  bool hasSmoothedCentroid = false;
  double smoothedCentroidX = 0.0;
  double smoothedCentroidY = 0.0;

  // Scratch buffers for the per-frame pixel pipeline, kept as members and
  // reused frame to frame instead of being declared (and reallocated) fresh
  // on every call: cv::Mat::create() only actually allocates when the
  // requested size/type differs from what the Mat already holds, so as long
  // as the ROI size stays stable across consecutive frames (the common case
  // once tracking locks on), these buffers stop reallocating entirely.
  cv::Mat small;         // downscaled + grayscale frame
  cv::Mat smallColor;    // downscaled, still-color frame (BGRA/RGBA layouts only)
  cv::Mat denoised;
  cv::Mat localMean;
  cv::Mat contrast;
  cv::Mat mask;
  cv::Mat histogram;
  cv::Mat laplacian;  // motion-blur variance scratch, see analyzeFrame

  // Color-saturation mask scratch (see computeSaturationValue) - all sized
  // to match `small`, so they're directly OR-able/sliceable against `mask`.
  cv::Mat hsv;              // BGRA/RGBA path: smallColor converted to HSV
  cv::Mat chromaRaw;        // GRAY8 path: chroma planes, deinterleaved and sampled to `small`'s size
  cv::Mat saturationValue;  // continuous 0-255 "how colorful" map, either path (empty if no color data)
  cv::Mat saturationMask;   // saturationValue thresholded to binary, OR'd into `mask`
  std::vector<std::vector<cv::Point>> contours;

  // Debounces AnalysisResult::status/messageCode/message/isReadyForCapture
  // against single-frame misclassifications - see stabilizeStatus(). Tracked
  // by messageCode rather than the coarser AnalysisStatus: "nothing in
  // frame" and "off-center" are both AnalysisStatus::NotCentered but
  // different messages, so messageCode is what actually needs debouncing to
  // stop the visible instruction text from flickering.
  int rawMessageCodeStreak = -1;
  int rawMessageCodeStreakCount = 0;
  bool hasStableMessageCode = false;
  AnalysisStatus stableStatus = AnalysisStatus::NotCentered;
  int stableMessageCode = 0;
};
PipelineState gState;

/**
 * Score = Area × AspectAreaRatio × (1 - EdgeLinearity), used by
 * detectBestContour to rank the candidates that survive its hard
 * convex-rectangle filter (see there) instead of blindly assuming the
 * largest one is the product.
 *
 * AspectAreaRatio is the contour's "extent" (its own area divided by its
 * bounding box's area, capped at 1.0): a curved, organic garment silhouette
 * only partially fills its bounding rectangle, while an already-rectangular
 * object fills nearly all of it - so this factor alone would actually favor
 * a TV, which is exactly what EdgeLinearity exists to counteract.
 *
 * `looksLinear` - whether the contour approximates to a small (4-6-vertex)
 * polygon - is computed once by the caller and reused for both its hard
 * filter and this softer one: a few-cornered contour that ISN'T convex (an
 * L-shape, a concave silhouette) isn't disqualified outright, but still
 * reads as suspiciously simple and gets penalized here (see
 * kLinearShapeEdgeLinearity's doc comment on why this is a strong penalty
 * rather than outright disqualification) - and, when color data is
 * available (`saturationValue` non-empty), penalized further still if it's
 * ALSO low-saturation (neutral-toned), compounding specifically for the
 * TV/wall/wood-floor case (straight-edged AND neutral) without ever
 * touching the score of a non-linear contour - so a plain white/black/gray
 * garment (never looksLinear to begin with) is completely unaffected by
 * saturation either way.
 */
double scoreContour(double area, const cv::Rect& boundingBox, bool looksLinear, const cv::Mat& saturationValue) {
  const double boundingBoxArea = static_cast<double>(boundingBox.width) * boundingBox.height;
  const double aspectAreaRatio = boundingBoxArea > 0.0 ? std::clamp(area / boundingBoxArea, 0.0, 1.0) : 0.0;

  double edgeLinearity = 0.0;
  if (looksLinear) {
    bool lowSaturation = false;
    if (!saturationValue.empty()) {
      const cv::Rect clampedBox = boundingBox & cv::Rect(0, 0, saturationValue.cols, saturationValue.rows);
      if (clampedBox.area() > 0) {
        lowSaturation = cv::mean(saturationValue(clampedBox))[0] < kLinearShapeSaturationPenaltyThreshold;
      }
    }
    edgeLinearity = lowSaturation ? kLinearShapeLowSaturationEdgeLinearity : kLinearShapeEdgeLinearity;
  }

  return area * aspectAreaRatio * (1.0 - edgeLinearity);
}

/**
 * blur → adaptive local-contrast threshold → morphological open+close →
 * smart contour scoring → centroid, on an already-gray image.
 *
 * Unlike Canny (a fixed gradient-magnitude threshold, which still fires on
 * the soft gradient a cast shadow leaves behind even after blurring), each
 * pixel here is compared to its own local neighborhood mean: true object
 * edges have high local contrast, while a shadow - smooth and gradual -
 * averages into its own background and stays below threshold regardless of
 * how dark it is. This is also polarity-agnostic, unlike a binary threshold
 * that has to assume which side (light or dark) is the foreground.
 *
 * A bright background (a window, a reflection) can still clear the contrast
 * threshold as scattered speckle, though - the open/close order matters
 * here: OPEN erases that speckle first (it's too small to survive erosion),
 * *then* CLOSE bridges the garment's own edges into one silhouette. Doing
 * it in the other order would let CLOSE fuse the speckle into the garment's
 * silhouette before OPEN got a chance to remove it, artificially inflating
 * the bounding box (and, historically, spuriously triggering CutOffMargins
 * from a fabricated edge-touching box that was actually just window noise).
 *
 * The winning contour is no longer just the largest one, either: a TV, a
 * wall panel, or a piece of furniture in the background can easily out-area
 * a garment. Every contour clearing the `minArea` floor first goes through a
 * hard filter - approximate it with approxPolyDP, and if it simplifies to a
 * convex 4-6-vertex polygon, skip it outright (`continue`), since a genuine
 * straight-edged rectangular object (a TV, a picture frame, a door, a
 * shelf) is exactly that, while a garment silhouette even when it happens
 * to approximate to a similar vertex count is virtually never convex
 * (necklines, sleeves, and hems all carve concave notches into the
 * outline). Whatever survives that filter is ranked by scoreContour (which
 * still softly penalizes a few-cornered-but-non-convex shape) and the
 * highest-scoring one wins.
 *
 * `saturationValue` (see computeSaturationValue) must already be in the
 * SAME coordinate space as `gray` - when `gray` is an ROI sub-view of
 * state.small, pass the matching sub-view of state.saturationValue too, not
 * the full-frame Mat (a size mismatch would break the mask OR below, and an
 * unaligned one would silently score the wrong region). May be empty (no
 * color data available), which cleanly skips both the mask contribution and
 * scoreContour's saturation-aware penalty.
 */
std::optional<Detection> detectBestContour(const cv::Mat& gray, double minArea, PipelineState& state,
                                            const cv::Mat& saturationValue) {
  cv::GaussianBlur(gray, state.denoised, cv::Size(kGaussianKernelSize, kGaussianKernelSize), 0);
  cv::GaussianBlur(state.denoised, state.localMean, cv::Size(kLocalContrastKernelSize, kLocalContrastKernelSize), 0);
  cv::absdiff(state.denoised, state.localMean, state.contrast);
  cv::threshold(state.contrast, state.mask, kContrastThreshold, 255, cv::THRESH_BINARY);

  // Combine with the color-saturation mask, when color data was available
  // for this frame - a pixel counts as foreground if it's EITHER a strong
  // local-contrast edge OR part of a colorful (non-neutral) region, so a
  // solid-color garment's interior (near-zero internal edge signal on its
  // own) still contributes to a solid silhouette instead of relying on its
  // boundary alone.
  if (!saturationValue.empty()) {
    cv::threshold(saturationValue, state.saturationMask, kSaturationThreshold, 255, cv::THRESH_BINARY);
    cv::bitwise_or(state.mask, state.saturationMask, state.mask);
  }

  // Remove small isolated noise blobs first...
  cv::morphologyEx(state.mask, state.mask, cv::MORPH_OPEN, kMorphOpenKernel);
  // ...then bridge the many small edges a patterned/textured garment
  // produces into one connected silhouette, so its outer boundary is a
  // single scoreable candidate instead of dozens of fragments.
  cv::morphologyEx(state.mask, state.mask, cv::MORPH_CLOSE, kMorphCloseKernel);

  state.contours.clear();
  cv::findContours(state.mask, state.contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);

  double bestScore = -1.0;
  double bestArea = 0.0;
  cv::Rect bestBoundingBox;
  const std::vector<cv::Point>* best = nullptr;
  for (const auto& contour : state.contours) {
    const double area = cv::contourArea(contour);
    // Skip the (relatively expensive) scoring work for anything too small
    // to be a candidate at all.
    if (area < minArea) {
      continue;
    }

    std::vector<cv::Point> approx;
    const double perimeter = cv::arcLength(contour, true);
    cv::approxPolyDP(contour, approx, perimeter * kApproxPolyEpsilonFraction, true);
    const bool looksLinear = static_cast<int>(approx.size()) >= kLinearShapeMinVertices &&
                              static_cast<int>(approx.size()) <= kLinearShapeMaxVertices;

    // Hard filter: a convex 4-6-sided polygon is a genuine straight-edged
    // rectangular object (a TV, a picture frame, a door, a shelf), not a
    // garment - skip it outright rather than merely score-penalizing it, so
    // it can never win regardless of how large or well-lit it is.
    if (looksLinear && cv::isContourConvex(approx)) {
      continue;
    }

    const cv::Rect boundingBox = cv::boundingRect(contour);
    const double score = scoreContour(area, boundingBox, looksLinear, saturationValue);
    if (score > bestScore) {
      bestScore = score;
      bestArea = area;
      bestBoundingBox = boundingBox;
      best = &contour;
    }
  }
  if (best == nullptr) {
    return std::nullopt;
  }

  const cv::Moments moments = cv::moments(*best);
  if (moments.m00 <= 0.0) {
    return std::nullopt;
  }

  Detection detection;
  detection.centroid = {moments.m10 / moments.m00, moments.m01 / moments.m00};
  detection.area = bestArea;
  detection.boundingBox = bestBoundingBox;
  return detection;
}

/**
 * Classifies exposure from the grayscale image's brightness histogram: too
 * dark if a large fraction of pixels sit in the shadow bins, too bright if a
 * large fraction sit in the highlight bins.
 */
LightingState computeLightingState(const cv::Mat& gray, PipelineState& state) {
  const int histSize = kExposureHistogramBins;
  const float range[] = {0.0f, 256.0f};
  const float* histRange = range;
  cv::calcHist(&gray, 1, nullptr, cv::Mat(), state.histogram, 1, &histSize, &histRange);

  double shadowCount = 0.0;
  double highlightCount = 0.0;
  for (int bin = 0; bin < kShadowClipBin; bin++) {
    shadowCount += state.histogram.at<float>(bin);
  }
  for (int bin = kHighlightClipBin; bin < histSize; bin++) {
    highlightCount += state.histogram.at<float>(bin);
  }

  const double totalPixels = static_cast<double>(gray.total());
  if (shadowCount / totalPixels > kExposureClipFraction) {
    return LightingState::TooDark;
  }
  if (highlightCount / totalPixels > kExposureClipFraction) {
    return LightingState::Overexposed;
  }
  return LightingState::Good;
}

/**
 * Builds a continuous "how colorful is this pixel" map, 0-255, aligned to
 * `targetSize` (matching state.small) and roughly matching OpenCV's HSV
 * saturation convention regardless of source:
 *  - BGRA8888/RGBA8888: real HSV conversion, S channel extracted directly
 *    from `smallColor` (already downscaled, already available).
 *  - GRAY8 (Y-plane primary, chroma supplied separately - see ChromaPlane):
 *    chroma-plane magnitude (distance from neutral gray in the Cb/Cr plane)
 *    approximates HSV saturation without the cost of a full YUV→RGB→HSV
 *    round trip on a path that's specifically optimized to avoid touching
 *    full color data - valid because HSV saturation, for a given lightness,
 *    is monotonic in chroma-plane distance from center. Sampled directly at
 *    `targetSize` (never materialized at native chroma resolution, which
 *    for a 1080p capture would be roughly 500K pixels instead of the
 *    ~30K `targetSize` implies) so this stays proportional to the same
 *    240px-ish working resolution as everything else in the pipeline.
 *
 * Returns an empty Mat if no color data is available at all (GRAY8 with no
 * chroma planes supplied) - callers must treat "empty" as "skip the
 * saturation signal entirely", not as "zero saturation everywhere" (which
 * would wrongly suppress every contour, including legitimate garments).
 */
cv::Mat computeSaturationValue(PixelLayout layout, const cv::Mat& smallColor, int chromaWidth, int chromaHeight,
                                const ChromaPlane& chromaU, const ChromaPlane& chromaV, const cv::Size& targetSize,
                                PipelineState& state) {
  if (layout == PixelLayout::BGRA8888 || layout == PixelLayout::RGBA8888) {
    cv::cvtColor(smallColor, state.hsv, layout == PixelLayout::BGRA8888 ? cv::COLOR_BGRA2BGR : cv::COLOR_RGBA2RGB);
    cv::cvtColor(state.hsv, state.hsv, layout == PixelLayout::BGRA8888 ? cv::COLOR_BGR2HSV : cv::COLOR_RGB2HSV);
    cv::extractChannel(state.hsv, state.saturationValue, 1);
    return state.saturationValue;
  }

  if (chromaU.data == nullptr || chromaV.data == nullptr || chromaWidth <= 0 || chromaHeight <= 0 ||
      targetSize.width <= 0 || targetSize.height <= 0) {
    state.saturationValue.release();
    return state.saturationValue;
  }

  // Deinterleave + downsample in one pass: for each TARGET pixel, sample
  // the corresponding source chroma sample directly via nearest-neighbor
  // mapping, rather than first building a native-chroma-resolution buffer
  // and resizing it - see this function's doc comment on why that matters.
  state.chromaRaw.create(targetSize, CV_8UC2);
  const double xRatio = static_cast<double>(chromaWidth) / targetSize.width;
  const double yRatio = static_cast<double>(chromaHeight) / targetSize.height;
  for (int row = 0; row < targetSize.height; row++) {
    const int srcRow = std::min(static_cast<int>(row * yRatio), chromaHeight - 1);
    const uint8_t* uRow = chromaU.data + static_cast<size_t>(srcRow) * chromaU.rowStride;
    const uint8_t* vRow = chromaV.data + static_cast<size_t>(srcRow) * chromaV.rowStride;
    uint8_t* dstRow = state.chromaRaw.ptr<uint8_t>(row);
    for (int col = 0; col < targetSize.width; col++) {
      const int srcCol = std::min(static_cast<int>(col * xRatio), chromaWidth - 1);
      dstRow[col * 2] = uRow[srcCol * chromaU.pixelStride];
      dstRow[col * 2 + 1] = vRow[srcCol * chromaV.pixelStride];
    }
  }

  cv::Mat channels[2];
  cv::split(state.chromaRaw, channels);
  cv::Mat centeredU, centeredV, magnitude;
  channels[0].convertTo(centeredU, CV_32F, 1.0, -128.0);
  channels[1].convertTo(centeredV, CV_32F, 1.0, -128.0);
  cv::magnitude(centeredU, centeredV, magnitude);
  // Raw chroma-plane magnitude tops out around 180 (sqrt(127²+127²)); scale
  // to roughly match HSV-S's 0-255 range so both paths share the same
  // threshold constants.
  magnitude.convertTo(state.saturationValue, CV_8UC1, 255.0 / 180.0);
  return state.saturationValue;
}

/** Classification derived purely from geometry/sensors - no image work, so cheap enough to run every call. */
struct Alignment {
  AnalysisStatus status = AnalysisStatus::NotCentered;
  int messageCode = 0;
  OrientationMode orientationMode = OrientationMode::Flat;
  double normalizedDx = 0.0;
  double normalizedDy = 0.0;
  Vector2 tilt;
};

/**
 * Reduces raw pitch/roll/centroid/bounding-box into AnalysisResult's status/
 * message/normalized-vector fields, per the strict priority hierarchy
 * documented on AnalysisStatus. Runs every call (not just processed frames)
 * so the tilt vector stays responsive even on frames where the image
 * pipeline itself is skipped - lighting is deliberately not part of this
 * hierarchy (it's an independent, image-derived signal only known on
 * processed frames), folded in separately when AnalysisResult::isReadyForCapture
 * is set.
 *
 * `isFlatMode` comes straight from SensorFusion::Attitude rather than being
 * derived here from `pitchDegrees`: SensorFusion already hysteresis-filters
 * it around the flat/hanger boundary, and that filtering needs to persist
 * across calls the same way pitch/roll integration does - state this
 * function deliberately doesn't have (it's a pure, stateless classifier).
 */
Alignment classifyAlignment(bool found, double pitchDegrees, double rollDegrees, bool isFlatMode, double centroidX,
                             double centroidY, double boundingBoxX, double boundingBoxY, double boundingBoxWidth,
                             double boundingBoxHeight, int frameWidth, int frameHeight) {
  Alignment result;

  result.orientationMode = isFlatMode ? OrientationMode::Flat : OrientationMode::Hanger;

  const double pitchError = std::min(std::abs(pitchDegrees), std::abs(pitchDegrees - 90.0));
  result.tilt.dx = std::clamp(-rollDegrees / kMaxTiltDeg, -1.0, 1.0);
  result.tilt.dy = std::clamp(-pitchError / kMaxTiltDeg, -1.0, 1.0);
  const bool tooTilted = pitchError >= kLevelThresholdDeg || std::abs(rollDegrees) >= kLevelThresholdDeg;

  const double areaFraction = (found && frameWidth > 0 && frameHeight > 0)
                                   ? (boundingBoxWidth * boundingBoxHeight) / (static_cast<double>(frameWidth) * frameHeight)
                                   : 0.0;

  // a) Absence: nothing significant detected - covers both "no contour at
  //    all" and "a contour was found but it's too small to be the product".
  if (!found || frameWidth <= 0 || frameHeight <= 0 || areaFraction < kMinPresenceAreaFraction) {
    result.status = AnalysisStatus::NotCentered;
    result.messageCode = 0;
    return result;
  }

  result.normalizedDx = std::clamp((centroidX - frameWidth / 2.0) / (frameWidth / 2.0), -1.0, 1.0);
  result.normalizedDy = std::clamp((centroidY - frameHeight / 2.0) / (frameHeight / 2.0), -1.0, 1.0);

  const double edgeMarginX = frameWidth * kFramingEdgeMarginFraction;
  const double edgeMarginY = frameHeight * kFramingEdgeMarginFraction;
  const bool touchesLeft = boundingBoxX <= edgeMarginX;
  const bool touchesRight = boundingBoxX + boundingBoxWidth >= frameWidth - edgeMarginX;
  const bool touchesTop = boundingBoxY <= edgeMarginY;
  const bool touchesBottom = boundingBoxY + boundingBoxHeight >= frameHeight - edgeMarginY;
  // Two OPPOSITE margins, not merely any one edge - see
  // kFramingEdgeMarginFraction's doc comment for why a single-edge touch
  // isn't a reliable enough signal on its own.
  const bool touchesOppositeMargins = (touchesLeft && touchesRight) || (touchesTop && touchesBottom);

  // b) Cut off at the frame margins - only for a product that's actually
  // large enough to plausibly be outgrowing the frame; a small box merely
  // brushing the edges falls through to the centering/tilt checks instead.
  if (touchesOppositeMargins && areaFraction > kCutOffMinAreaFraction) {
    result.status = AnalysisStatus::CutOffMargins;
    result.messageCode = 1;
    return result;
  }

  // c) Centering - per-axis, not combined magnitude (see kCenterThresholdNorm).
  if (std::abs(result.normalizedDx) > kCenterThresholdNorm || std::abs(result.normalizedDy) > kCenterThresholdNorm) {
    result.status = AnalysisStatus::NotCentered;
    result.messageCode = 2;
    return result;
  }

  // d) Phone tilt - only reached once framing and centering already pass.
  if (tooTilted) {
    result.status = AnalysisStatus::PhoneTilted;
    result.messageCode = 3;
    return result;
  }

  // e) Perfect.
  result.status = AnalysisStatus::Ok;
  result.messageCode = 4;
  return result;
}

/**
 * Copies an Alignment classification into an AnalysisResult, deriving the
 * RAW (not yet debounced - see stabilizeStatus) `message` from `messageCode`
 * via kMessages (the single source of truth) and a RAW `isReadyForCapture`
 * by ANDing in the lighting signal - the one thing the status/message
 * hierarchy above deliberately doesn't cover, but a frame isn't actually
 * worth auto-capturing if it's too dark/bright.
 */
void applyAlignment(AnalysisResult& result, const Alignment& alignment) {
  result.status = alignment.status;
  result.messageCode = alignment.messageCode;
  result.message = kMessages[alignment.messageCode];
  result.orientationMode = alignment.orientationMode;
  result.normalizedDx = alignment.normalizedDx;
  result.normalizedDy = alignment.normalizedDy;
  result.tilt = alignment.tilt;
  result.isReadyForCapture = alignment.status == AnalysisStatus::Ok && result.lightingState == LightingState::Good;
}

/**
 * Debounces `result.status`/`messageCode`/`message`/`isReadyForCapture`
 * (as just computed by applyAlignment, from this call's raw classification)
 * against single-frame misclassifications: the reported status only changes
 * once the raw classification has been consistent for kStatusConfirmFrames
 * consecutive calls, so a garment casting a stray shadow for one frame
 * can't flicker the on-screen instruction, the alignment guide's color, or
 * restart the auto-capture countdown ring.
 *
 * Runs on every call (not just processed frames) so a skipped frame's
 * fresh, tilt-driven classification still gets a vote in the debounce -
 * consistent with "cadre" (frames) in the spec meaning every rendered
 * frame, not just the ones that ran the image pipeline.
 */
void stabilizeStatus(PipelineState& state, AnalysisResult& result) {
  const int rawMessageCode = result.messageCode;

  if (rawMessageCode == state.rawMessageCodeStreak) {
    state.rawMessageCodeStreakCount++;
  } else {
    state.rawMessageCodeStreak = rawMessageCode;
    state.rawMessageCodeStreakCount = 1;
  }

  if (!state.hasStableMessageCode || state.rawMessageCodeStreakCount >= kStatusConfirmFrames) {
    state.stableStatus = result.status;
    state.stableMessageCode = rawMessageCode;
    state.hasStableMessageCode = true;
  }

  result.status = state.stableStatus;
  result.messageCode = state.stableMessageCode;
  result.message = kMessages[state.stableMessageCode];
  result.isReadyForCapture = state.stableStatus == AnalysisStatus::Ok && result.lightingState == LightingState::Good;
}

/** Previous bounding box grown by the margin and clamped to the image. */
cv::Rect expandedRoi(const cv::Rect& box, const cv::Size& imageSize) {
  const int marginX = static_cast<int>(box.width * kRoiMarginFraction);
  const int marginY = static_cast<int>(box.height * kRoiMarginFraction);
  cv::Rect roi(box.x - marginX, box.y - marginY, box.width + 2 * marginX, box.height + 2 * marginY);
  return roi & cv::Rect(0, 0, imageSize.width, imageSize.height);
}

// Set for the duration of analyzeFrame's real work - see ProcessingGuard.
std::atomic<bool> gIsProcessing{false};

/**
 * Non-blocking re-entrancy guard around analyzeFrame's global PipelineState.
 * VisionCamera invokes frame processors synchronously and serially on a
 * single dedicated thread - camera frame N+1 is never delivered until the
 * call for frame N has returned, so this can never actually be contended
 * under normal operation. It exists as defense-in-depth against that
 * assumption ever being violated (a future VisionCamera version, a second
 * camera session mid-device-switch, a bug), and specifically uses a
 * lock-free compare-exchange rather than a mutex: a mutex would make a
 * contended call BLOCK and wait its turn, which is exactly the "camera
 * thread stalls while frames queue up behind a slow one" failure mode this
 * needs to prevent - see analyzeFrame's use of it, which drops the frame
 * (returns immediately) instead of waiting when acquisition fails.
 */
class ProcessingGuard {
 public:
  ProcessingGuard() : acquired_(!gIsProcessing.exchange(true, std::memory_order_acquire)) {}
  ~ProcessingGuard() {
    if (acquired_) {
      gIsProcessing.store(false, std::memory_order_release);
    }
  }
  ProcessingGuard(const ProcessingGuard&) = delete;
  ProcessingGuard& operator=(const ProcessingGuard&) = delete;

  bool acquired() const { return acquired_; }

 private:
  bool acquired_;
};

} // namespace

void resetPipelineState() {
  gState = PipelineState{};
}

AnalysisResult analyzeFrame(const uint8_t* data,
                            int width,
                            int height,
                            size_t bytesPerRow,
                            PixelLayout layout,
                            int chromaWidth,
                            int chromaHeight,
                            ChromaPlane chromaU,
                            ChromaPlane chromaV) {
  // Frame-buffer management: if a previous call is somehow still in flight
  // (should be unreachable - see ProcessingGuard's doc comment), drop this
  // frame outright rather than queue behind it or race on gState's
  // completely unsynchronized fields. Returning a fresh default here (not
  // gState.lastResult) is deliberate: that other call is actively mutating
  // gState right now, so reading ANY of its fields, even just-cached ones,
  // would itself be a data race.
  ProcessingGuard guard;
  if (!guard.acquired()) {
    return AnalysisResult{};
  }

  const auto attitude = SensorFusion::instance().getAttitude();

  // Frame skipping: refresh only pitch/roll (and the alignment/tilt state
  // derived from them) on skipped frames, and return the cached detection
  // without touching the pixels.
  const bool shouldProcess = gState.frameCounter % kProcessEveryNthFrame == 0;
  gState.frameCounter++;
  if (!shouldProcess) {
    AnalysisResult result = gState.lastResult;
    result.processed = false;

    const Alignment alignment =
        classifyAlignment(result.found, attitude.pitchDegrees, attitude.rollDegrees, attitude.isFlatMode,
                           gState.smoothedCentroidX, gState.smoothedCentroidY, result.boundingBoxX, result.boundingBoxY,
                           result.boundingBoxWidth, result.boundingBoxHeight, result.frameWidth, result.frameHeight);
    applyAlignment(result, alignment);
    stabilizeStatus(gState, result);
#ifdef DEBUG
    result.pitchDegrees = attitude.pitchDegrees;
    result.rollDegrees = attitude.rollDegrees;
#endif
    return result;
  }

  const auto startTime = std::chrono::steady_clock::now();

  AnalysisResult result;
  result.frameWidth = width;
  result.frameHeight = height;
  result.processed = true;
#ifdef DEBUG
  result.pitchDegrees = attitude.pitchDegrees;
  result.rollDegrees = attitude.rollDegrees;
#endif

  if (data == nullptr || width <= 0 || height <= 0) {
    const Alignment alignment = classifyAlignment(false, attitude.pitchDegrees, attitude.rollDegrees,
                                                    attitude.isFlatMode, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, width, height);
    applyAlignment(result, alignment);
    stabilizeStatus(gState, result);
    gState.lastResult = result;
    return result;
  }

  // The Mat wraps the camera buffer in place (zero-copy); for YUV frames the
  // Y plane already is the grayscale image.
  //
  // 1. Downscale FIRST, to a fixed width regardless of camera resolution,
  //    with INTER_NEAREST - see this file's header doc comment for why
  //    nearest-neighbor (not area-averaging) is the deliberate choice here.
  // 2. Grayscale SECOND, on the downscaled image, so the RGBA→gray
  //    conversion touches a small fraction of the full frame.
  const double scale = width > kTargetProcessingWidth
                           ? static_cast<double>(kTargetProcessingWidth) / width
                           : 1.0;
  const cv::Size smallSize(static_cast<int>(width * scale + 0.5),
                           static_cast<int>(height * scale + 0.5));

  switch (layout) {
    case PixelLayout::GRAY8: {
      cv::Mat full(height, width, CV_8UC1, const_cast<uint8_t*>(data), bytesPerRow);
      if (scale < 1.0) {
        cv::resize(full, gState.small, smallSize, 0, 0, cv::INTER_NEAREST);
      } else {
        gState.small = full;
      }
      break;
    }
    case PixelLayout::BGRA8888:
    case PixelLayout::RGBA8888: {
      cv::Mat full(height, width, CV_8UC4, const_cast<uint8_t*>(data), bytesPerRow);
      if (scale < 1.0) {
        cv::resize(full, gState.smallColor, smallSize, 0, 0, cv::INTER_NEAREST);
      } else {
        gState.smallColor = full;
      }
      cv::cvtColor(gState.smallColor, gState.small,
                   layout == PixelLayout::BGRA8888 ? cv::COLOR_BGRA2GRAY : cv::COLOR_RGBA2GRAY);
      break;
    }
  }

  // Motion-blur bypass: a frame blurred by fast phone movement/rotation has
  // almost no high-frequency detail left for the adaptive-contrast edge
  // mask to find, so any contour still found in it is unreliable at best -
  // and, worse, the full detection pipeline below is the expensive part of
  // this function. Bail out immediately once we know the frame can't be
  // trusted, before touching lighting/saturation/contours at all, so a
  // burst of fast-motion frames costs a downscale + one Laplacian variance
  // each (well under 1ms) instead of the full pipeline.
  cv::Laplacian(gState.small, gState.laplacian, CV_64F);
  cv::Scalar blurMean, blurStddev;
  cv::meanStdDev(gState.laplacian, blurMean, blurStddev);
  const double blurVariance = blurStddev[0] * blurStddev[0];
  if (blurVariance < kMinBlurVariance) {
    const Alignment alignment = classifyAlignment(false, attitude.pitchDegrees, attitude.rollDegrees,
                                                    attitude.isFlatMode, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, width, height);
    applyAlignment(result, alignment);
    // Override with the motion-blur-specific status/message - status stays
    // NotCentered (it already is, from classifyAlignment's found=false
    // branch above), but the message/messageCode need to say why.
    result.messageCode = kMotionBlurMessageCode;
    result.message = kMessages[kMotionBlurMessageCode];
    result.isReadyForCapture = false;
    stabilizeStatus(gState, result);

    const auto elapsed = std::chrono::steady_clock::now() - startTime;
    result.latencyMs = std::chrono::duration<double, std::milli>(elapsed).count();

    gState.lastResult = result;
    return result;
  }

  result.lightingState = computeLightingState(gState.small, gState);

  // Color-saturation signal for the combined mask + scoreContour's linear-
  // shape penalty (see computeSaturationValue's doc comment) - computed
  // once per processed frame at gState.small's resolution, empty when no
  // color data is available for this call (degrades gracefully to the
  // pre-existing edge-only behavior).
  const cv::Mat saturationValue =
      computeSaturationValue(layout, gState.smallColor, chromaWidth, chromaHeight, chromaU, chromaV,
                              gState.small.size(), gState);

  const double minArea = kMinPresenceAreaFraction * gState.small.cols * gState.small.rows;

  // 3. Search inside the tracked ROI when we have one; fall back to the
  //    full frame if the object left it.
  std::optional<Detection> detection;
  cv::Point roiOffset(0, 0);
  if (gState.hasRoi) {
    const cv::Rect roi = gState.roi & cv::Rect(0, 0, gState.small.cols, gState.small.rows);
    if (roi.area() > 0) {
      const cv::Mat roiSaturation = saturationValue.empty() ? cv::Mat() : saturationValue(roi);
      detection = detectBestContour(gState.small(roi), minArea, gState, roiSaturation);
      roiOffset = roi.tl();
    }
  }
  if (!detection.has_value()) {
    detection = detectBestContour(gState.small, minArea, gState, saturationValue);
    roiOffset = {0, 0};
  }

  if (detection.has_value()) {
    result.found = true;
    // Map back: ROI offset, then undo the downscale.
    const double rawCentroidX = (detection->centroid.x + roiOffset.x) / scale;
    const double rawCentroidY = (detection->centroid.y + roiOffset.y) / scale;
#ifdef DEBUG
    result.contourArea = detection->area / (scale * scale);
#endif

    // EMA-smooth the reported centroid to damp frame-to-frame jitter in the
    // on-screen alignment arrow. The ROI above tracks the raw (unsmoothed)
    // detection, so a lagging smoothed point can never cause the search
    // window to fall behind a fast-moving object.
    if (!gState.hasSmoothedCentroid) {
      gState.smoothedCentroidX = rawCentroidX;
      gState.smoothedCentroidY = rawCentroidY;
      gState.hasSmoothedCentroid = true;
    } else {
      gState.smoothedCentroidX =
          kCentroidSmoothingAlpha * rawCentroidX + (1.0 - kCentroidSmoothingAlpha) * gState.smoothedCentroidX;
      gState.smoothedCentroidY =
          kCentroidSmoothingAlpha * rawCentroidY + (1.0 - kCentroidSmoothingAlpha) * gState.smoothedCentroidY;
    }
#ifdef DEBUG
    result.centroidX = gState.smoothedCentroidX;
    result.centroidY = gState.smoothedCentroidY;
#endif

    const cv::Rect boundingBoxInSmall = detection->boundingBox + roiOffset;
    result.boundingBoxX = boundingBoxInSmall.x / scale;
    result.boundingBoxY = boundingBoxInSmall.y / scale;
    result.boundingBoxWidth = boundingBoxInSmall.width / scale;
    result.boundingBoxHeight = boundingBoxInSmall.height / scale;

    gState.roi = expandedRoi(detection->boundingBox + roiOffset, gState.small.size());
    gState.hasRoi = gState.roi.area() > 0;
  } else {
    gState.hasRoi = false;
    // Object lost: next detection should snap to place, not lerp in from a
    // now-stale position.
    gState.hasSmoothedCentroid = false;
  }

  const Alignment alignment =
      classifyAlignment(result.found, attitude.pitchDegrees, attitude.rollDegrees, attitude.isFlatMode,
                         gState.smoothedCentroidX, gState.smoothedCentroidY, result.boundingBoxX, result.boundingBoxY,
                         result.boundingBoxWidth, result.boundingBoxHeight, width, height);
  applyAlignment(result, alignment);
  stabilizeStatus(gState, result);

  const auto elapsed = std::chrono::steady_clock::now() - startTime;
  result.latencyMs = std::chrono::duration<double, std::milli>(elapsed).count();

  gState.lastResult = result;
  return result;
}

bool cropPhotoToBoundingBox(const std::string& sourcePath,
                             const std::string& destPath,
                             double boxX,
                             double boxY,
                             double boxWidth,
                             double boxHeight,
                             int analysisFrameWidth,
                             int analysisFrameHeight) {
  if (analysisFrameWidth <= 0 || analysisFrameHeight <= 0 || boxWidth <= 0.0 || boxHeight <= 0.0) {
    return false;
  }

  const cv::Mat photo = cv::imread(sourcePath, cv::IMREAD_COLOR);
  if (photo.empty()) {
    return false;
  }

  // The box was detected in the analysis frame's pixel space, which is
  // generally a different resolution than the captured photo (though the
  // same aspect ratio/orientation, for a given camera device) - scale
  // proportionally into the photo's own pixel space.
  const double scaleX = static_cast<double>(photo.cols) / analysisFrameWidth;
  const double scaleY = static_cast<double>(photo.rows) / analysisFrameHeight;

  const cv::Rect photoBounds(0, 0, photo.cols, photo.rows);
  const cv::Rect cropRect = cv::Rect(static_cast<int>(boxX * scaleX), static_cast<int>(boxY * scaleY),
                                      static_cast<int>(boxWidth * scaleX), static_cast<int>(boxHeight * scaleY)) &
                             photoBounds;
  if (cropRect.area() <= 0) {
    return false;
  }

  return cv::imwrite(destPath, photo(cropRect));
}

} // namespace visionapp
