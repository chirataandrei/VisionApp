#include "ProductAnalyzer.h"

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <chrono>
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
// Structuring element used to close gaps between the many small edges a
// patterned/textured garment produces, merging them into one silhouette.
constexpr int kMorphCloseKernelSize = 9;
// Contours smaller than this fraction of the (downscaled) frame are noise.
constexpr double kMinContourAreaFraction = 0.001;
// EMA smoothing factor for the reported centroid (0 < a <= 1): higher tracks
// the raw detection more closely, lower damps jitter more but adds lag.
constexpr double kCentroidSmoothingAlpha = 0.35;

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
// Edge detection runs on an image downscaled to roughly this width.
constexpr int kTargetProcessingWidth = 320;
// The tracking ROI is the previous bounding box grown by this fraction.
constexpr double kRoiMarginFraction = 0.3;

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
};
PipelineState gState;

/**
 * blur → adaptive local-contrast threshold → morphological close → largest
 * contour → centroid, on an already-gray image.
 *
 * Unlike Canny (a fixed gradient-magnitude threshold, which still fires on
 * the soft gradient a cast shadow leaves behind even after blurring), each
 * pixel here is compared to its own local neighborhood mean: true object
 * edges have high local contrast, while a shadow - smooth and gradual -
 * averages into its own background and stays below threshold regardless of
 * how dark it is. This is also polarity-agnostic, unlike a binary threshold
 * that has to assume which side (light or dark) is the foreground.
 */
std::optional<Detection> detectLargestContour(const cv::Mat& gray, double minArea) {
  cv::Mat denoised;
  cv::GaussianBlur(gray, denoised, cv::Size(kGaussianKernelSize, kGaussianKernelSize), 0);

  cv::Mat localMean;
  cv::GaussianBlur(denoised, localMean, cv::Size(kLocalContrastKernelSize, kLocalContrastKernelSize), 0);

  cv::Mat contrast;
  cv::absdiff(denoised, localMean, contrast);

  cv::Mat mask;
  cv::threshold(contrast, mask, kContrastThreshold, 255, cv::THRESH_BINARY);

  // Bridge the many small edges a patterned/textured garment produces into
  // one connected silhouette, so its outer boundary is the largest contour
  // instead of dozens of fragments.
  const cv::Mat kernel =
      cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(kMorphCloseKernelSize, kMorphCloseKernelSize));
  cv::morphologyEx(mask, mask, cv::MORPH_CLOSE, kernel);

  std::vector<std::vector<cv::Point>> contours;
  cv::findContours(mask, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);

  double largestArea = 0.0;
  const std::vector<cv::Point>* largest = nullptr;
  for (const auto& contour : contours) {
    const double area = cv::contourArea(contour);
    if (area > largestArea) {
      largestArea = area;
      largest = &contour;
    }
  }
  if (largest == nullptr || largestArea < minArea) {
    return std::nullopt;
  }

  const cv::Moments moments = cv::moments(*largest);
  if (moments.m00 <= 0.0) {
    return std::nullopt;
  }

  Detection detection;
  detection.centroid = {moments.m10 / moments.m00, moments.m01 / moments.m00};
  detection.area = largestArea;
  detection.boundingBox = cv::boundingRect(*largest);
  return detection;
}

/**
 * Classifies exposure from the grayscale image's brightness histogram: too
 * dark if a large fraction of pixels sit in the shadow bins, too bright if a
 * large fraction sit in the highlight bins.
 */
ExposureWarning computeExposureWarning(const cv::Mat& gray) {
  cv::Mat histogram;
  const int histSize = kExposureHistogramBins;
  const float range[] = {0.0f, 256.0f};
  const float* histRange = range;
  cv::calcHist(&gray, 1, nullptr, cv::Mat(), histogram, 1, &histSize, &histRange);

  double shadowCount = 0.0;
  double highlightCount = 0.0;
  for (int bin = 0; bin < kShadowClipBin; bin++) {
    shadowCount += histogram.at<float>(bin);
  }
  for (int bin = kHighlightClipBin; bin < histSize; bin++) {
    highlightCount += histogram.at<float>(bin);
  }

  const double totalPixels = static_cast<double>(gray.total());
  if (shadowCount / totalPixels > kExposureClipFraction) {
    return ExposureWarning::TooDark;
  }
  if (highlightCount / totalPixels > kExposureClipFraction) {
    return ExposureWarning::TooBright;
  }
  return ExposureWarning::None;
}

/** Previous bounding box grown by the margin and clamped to the image. */
cv::Rect expandedRoi(const cv::Rect& box, const cv::Size& imageSize) {
  const int marginX = static_cast<int>(box.width * kRoiMarginFraction);
  const int marginY = static_cast<int>(box.height * kRoiMarginFraction);
  cv::Rect roi(box.x - marginX, box.y - marginY, box.width + 2 * marginX, box.height + 2 * marginY);
  return roi & cv::Rect(0, 0, imageSize.width, imageSize.height);
}

} // namespace

void resetPipelineState() {
  gState = PipelineState{};
}

AnalysisResult analyzeFrame(const uint8_t* data,
                            int width,
                            int height,
                            size_t bytesPerRow,
                            PixelLayout layout) {
  const auto attitude = SensorFusion::instance().getAttitude();

  // Frame skipping: refresh only pitch/roll on skipped frames and return the
  // cached detection without touching the pixels.
  const bool shouldProcess = gState.frameCounter % kProcessEveryNthFrame == 0;
  gState.frameCounter++;
  if (!shouldProcess) {
    AnalysisResult result = gState.lastResult;
    result.pitchDegrees = attitude.pitchDegrees;
    result.rollDegrees = attitude.rollDegrees;
    result.processed = false;
    return result;
  }

  const auto startTime = std::chrono::steady_clock::now();

  AnalysisResult result;
  result.frameWidth = width;
  result.frameHeight = height;
  result.pitchDegrees = attitude.pitchDegrees;
  result.rollDegrees = attitude.rollDegrees;
  result.processed = true;

  if (data == nullptr || width <= 0 || height <= 0) {
    gState.lastResult = result;
    return result;
  }

  // The Mat wraps the camera buffer in place (zero-copy); for YUV frames the
  // Y plane already is the grayscale image.
  //
  // 1. Downscale FIRST, with INTER_AREA (proper area-averaging): it reads
  //    every full-resolution source pixel, so cost scales with camera
  //    resolution again - but unlike INTER_NEAREST (which only samples a
  //    ~320-wide grid and skips the rest), it doesn't alias fine fabric
  //    patterns/textures into moiré noise that would otherwise survive the
  //    Gaussian blur and register as spurious contours.
  // 2. Grayscale SECOND, on the downscaled image, so the RGBA→gray
  //    conversion touches ~0.15 MP instead of the full frame.
  const double scale = width > kTargetProcessingWidth
                           ? static_cast<double>(kTargetProcessingWidth) / width
                           : 1.0;
  const cv::Size smallSize(static_cast<int>(width * scale + 0.5),
                           static_cast<int>(height * scale + 0.5));

  cv::Mat small;
  switch (layout) {
    case PixelLayout::GRAY8: {
      cv::Mat full(height, width, CV_8UC1, const_cast<uint8_t*>(data), bytesPerRow);
      if (scale < 1.0) {
        cv::resize(full, small, smallSize, 0, 0, cv::INTER_AREA);
      } else {
        small = full;
      }
      break;
    }
    case PixelLayout::BGRA8888:
    case PixelLayout::RGBA8888: {
      cv::Mat full(height, width, CV_8UC4, const_cast<uint8_t*>(data), bytesPerRow);
      cv::Mat smallColor;
      if (scale < 1.0) {
        cv::resize(full, smallColor, smallSize, 0, 0, cv::INTER_AREA);
      } else {
        smallColor = full;
      }
      cv::cvtColor(smallColor, small,
                   layout == PixelLayout::BGRA8888 ? cv::COLOR_BGRA2GRAY : cv::COLOR_RGBA2GRAY);
      break;
    }
  }

  result.exposureWarning = computeExposureWarning(small);

  const double minArea = kMinContourAreaFraction * small.cols * small.rows;

  // 3. Search inside the tracked ROI when we have one; fall back to the
  //    full frame if the object left it.
  std::optional<Detection> detection;
  cv::Point roiOffset(0, 0);
  if (gState.hasRoi) {
    const cv::Rect roi = gState.roi & cv::Rect(0, 0, small.cols, small.rows);
    if (roi.area() > 0) {
      detection = detectLargestContour(small(roi), minArea);
      roiOffset = roi.tl();
    }
  }
  if (!detection.has_value()) {
    detection = detectLargestContour(small, minArea);
    roiOffset = {0, 0};
  }

  if (detection.has_value()) {
    result.found = true;
    // Map back: ROI offset, then undo the downscale.
    const double rawCentroidX = (detection->centroid.x + roiOffset.x) / scale;
    const double rawCentroidY = (detection->centroid.y + roiOffset.y) / scale;
    result.contourArea = detection->area / (scale * scale);

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
    result.centroidX = gState.smoothedCentroidX;
    result.centroidY = gState.smoothedCentroidY;

    const cv::Rect boundingBoxInSmall = detection->boundingBox + roiOffset;
    result.boundingBoxX = boundingBoxInSmall.x / scale;
    result.boundingBoxY = boundingBoxInSmall.y / scale;
    result.boundingBoxWidth = boundingBoxInSmall.width / scale;
    result.boundingBoxHeight = boundingBoxInSmall.height / scale;

    gState.roi = expandedRoi(detection->boundingBox + roiOffset, small.size());
    gState.hasRoi = gState.roi.area() > 0;
  } else {
    gState.hasRoi = false;
    // Object lost: next detection should snap to place, not lerp in from a
    // now-stale position.
    gState.hasSmoothedCentroid = false;
  }

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
