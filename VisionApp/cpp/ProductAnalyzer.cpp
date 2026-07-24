#include "ProductAnalyzer.h"

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>

#include "SensorFusion.h"

namespace visionapp {

namespace {

constexpr double kCannyThresholdLow = 50.0;
constexpr double kCannyThresholdHigh = 150.0;
constexpr int kGaussianKernelSize = 5;
// Contours smaller than this fraction of the (downscaled) frame are noise.
constexpr double kMinContourAreaFraction = 0.001;

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
};
PipelineState gState;

/** blur → Canny → largest contour → centroid, on an already-gray image. */
std::optional<Detection> detectLargestContour(const cv::Mat& gray, double minArea) {
  cv::Mat blurred;
  cv::GaussianBlur(gray, blurred, cv::Size(kGaussianKernelSize, kGaussianKernelSize), 0);

  cv::Mat edges;
  cv::Canny(blurred, edges, kCannyThresholdLow, kCannyThresholdHigh);

  std::vector<std::vector<cv::Point>> contours;
  cv::findContours(edges, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);

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

/** Previous bounding box grown by the margin and clamped to the image. */
cv::Rect expandedRoi(const cv::Rect& box, const cv::Size& imageSize) {
  const int marginX = static_cast<int>(box.width * kRoiMarginFraction);
  const int marginY = static_cast<int>(box.height * kRoiMarginFraction);
  cv::Rect roi(box.x - marginX, box.y - marginY, box.width + 2 * marginX, box.height + 2 * marginY);
  return roi & cv::Rect(0, 0, imageSize.width, imageSize.height);
}

} // namespace

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
  // 1. Downscale FIRST, with nearest-neighbor sampling: unlike INTER_AREA
  //    (which averages, i.e. reads, every full-resolution pixel), it only
  //    reads the ~320-wide grid of target pixels, so the cost of everything
  //    below is independent of the camera resolution. The aliasing it
  //    introduces is absorbed by the Gaussian blur that precedes Canny.
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
        cv::resize(full, small, smallSize, 0, 0, cv::INTER_NEAREST);
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
        cv::resize(full, smallColor, smallSize, 0, 0, cv::INTER_NEAREST);
      } else {
        smallColor = full;
      }
      cv::cvtColor(smallColor, small,
                   layout == PixelLayout::BGRA8888 ? cv::COLOR_BGRA2GRAY : cv::COLOR_RGBA2GRAY);
      break;
    }
  }

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
    result.centroidX = (detection->centroid.x + roiOffset.x) / scale;
    result.centroidY = (detection->centroid.y + roiOffset.y) / scale;
    result.contourArea = detection->area / (scale * scale);

    gState.roi = expandedRoi(detection->boundingBox + roiOffset, small.size());
    gState.hasRoi = gState.roi.area() > 0;
  } else {
    gState.hasRoi = false;
  }

  const auto elapsed = std::chrono::steady_clock::now() - startTime;
  result.latencyMs = std::chrono::duration<double, std::milli>(elapsed).count();

  gState.lastResult = result;
  return result;
}

} // namespace visionapp
