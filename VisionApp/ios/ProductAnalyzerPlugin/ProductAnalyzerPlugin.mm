//
//  ProductAnalyzerPlugin.mm
//  VisionApp
//
//  VisionCamera Frame Processor Plugin "analyzeProduct".
//
//  Bridges the camera frame (zero-copy) and CoreMotion sensor events into
//  the shared C++ pipeline (cpp/ProductAnalyzer.cpp, cpp/SensorFusion.cpp).
//

#import <CoreMedia/CMSampleBuffer.h>
#import <CoreMotion/CoreMotion.h>
#import <CoreVideo/CVPixelBuffer.h>
#import <Foundation/Foundation.h>

#import <VisionCamera/Frame.h>
#import <VisionCamera/FrameProcessorPlugin.h>
#import <VisionCamera/FrameProcessorPluginRegistry.h>
#import <VisionCamera/VisionCameraProxyHolder.h>

#import "ProductAnalyzer.h"
#import "SensorFusion.h"

@interface ProductAnalyzerPlugin : FrameProcessorPlugin
@end

@implementation ProductAnalyzerPlugin {
  CMMotionManager* _motionManager;
  NSOperationQueue* _sensorQueue;
}

- (instancetype)initWithProxy:(VisionCameraProxyHolder*)proxy withOptions:(NSDictionary* _Nullable)options {
  self = [super initWithProxy:proxy withOptions:options];
  if (self) {
    _sensorQueue = [[NSOperationQueue alloc] init];
    _sensorQueue.maxConcurrentOperationCount = 1;

    _motionManager = [[CMMotionManager alloc] init];
    _motionManager.accelerometerUpdateInterval = 1.0 / 100.0;
    _motionManager.gyroUpdateInterval = 1.0 / 100.0;

    if (_motionManager.isAccelerometerAvailable) {
      [_motionManager startAccelerometerUpdatesToQueue:_sensorQueue
                                            withHandler:^(CMAccelerometerData* _Nullable data, NSError* _Nullable error) {
                                              if (data == nil) {
                                                return;
                                              }
                                              // CoreMotion reads gravity as -1g on Z when the device lies flat;
                                              // negate to match the Android sensor convention used in C++.
                                              visionapp::SensorFusion::instance().updateAccelerometer(
                                                  -data.acceleration.x, -data.acceleration.y, -data.acceleration.z);
                                            }];
    }
    if (_motionManager.isGyroAvailable) {
      [_motionManager startGyroUpdatesToQueue:_sensorQueue
                                   withHandler:^(CMGyroData* _Nullable data, NSError* _Nullable error) {
                                     if (data == nil) {
                                       return;
                                     }
                                     visionapp::SensorFusion::instance().updateGyroscope(
                                         data.rotationRate.x, data.rotationRate.y, data.rotationRate.z, data.timestamp);
                                   }];
    }
  }
  return self;
}

- (void)dealloc {
  [_motionManager stopAccelerometerUpdates];
  [_motionManager stopGyroUpdates];
}

- (id _Nullable)callback:(Frame*)frame withArguments:(NSDictionary* _Nullable)arguments {
  if ([arguments[@"reset"] boolValue]) {
    visionapp::resetPipelineState();
  }

  CVPixelBufferRef pixelBuffer = CMSampleBufferGetImageBuffer(frame.buffer);
  if (pixelBuffer == nil) {
    return nil;
  }

  CVPixelBufferLockBaseAddress(pixelBuffer, kCVPixelBufferLock_ReadOnly);

  visionapp::AnalysisResult result;
  if (CVPixelBufferIsPlanar(pixelBuffer)) {
    // YUV 4:2:0 — plane 0 is the luma (Y) plane, i.e. already grayscale.
    const auto* data = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pixelBuffer, 0));
    const int width = (int)CVPixelBufferGetWidthOfPlane(pixelBuffer, 0);
    const int height = (int)CVPixelBufferGetHeightOfPlane(pixelBuffer, 0);
    const size_t bytesPerRow = CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, 0);

    // Plane 1, when present, is interleaved CbCr (biplanar 4:2:0 - the
    // format VisionCamera's pixelFormat="yuv" delivers on iOS): U (Cb) and
    // V (Cr) share one physical buffer, U at byte offset 0 and V at byte
    // offset 1, both effectively pixelStride 2 - see ChromaPlane's doc
    // comment. Feeds the native color-saturation mask that helps
    // distinguish garments from neutral-toned backgrounds (a TV, a wall, a
    // wood floor).
    int chromaWidth = 0;
    int chromaHeight = 0;
    visionapp::ChromaPlane chromaU;
    visionapp::ChromaPlane chromaV;
    if (CVPixelBufferGetPlaneCount(pixelBuffer) > 1) {
      const auto* chromaBase = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pixelBuffer, 1));
      chromaWidth = (int)CVPixelBufferGetWidthOfPlane(pixelBuffer, 1);
      chromaHeight = (int)CVPixelBufferGetHeightOfPlane(pixelBuffer, 1);
      const size_t chromaRowStride = CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, 1);
      chromaU = {chromaBase, chromaRowStride, 2};
      chromaV = {chromaBase + 1, chromaRowStride, 2};
    }

    result = visionapp::analyzeFrame(data, width, height, bytesPerRow, visionapp::PixelLayout::GRAY8, chromaWidth,
                                      chromaHeight, chromaU, chromaV);
  } else {
    // Non-planar RGB camera output is 32BGRA on iOS.
    const auto* data = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddress(pixelBuffer));
    const int width = (int)CVPixelBufferGetWidth(pixelBuffer);
    const int height = (int)CVPixelBufferGetHeight(pixelBuffer);
    const size_t bytesPerRow = CVPixelBufferGetBytesPerRow(pixelBuffer);
    result = visionapp::analyzeFrame(data, width, height, bytesPerRow, visionapp::PixelLayout::BGRA8888);
  }

  CVPixelBufferUnlockBaseAddress(pixelBuffer, kCVPixelBufferLock_ReadOnly);

  NSString* status;
  switch (result.status) {
    case visionapp::AnalysisStatus::CutOffMargins:
      status = @"CUT_OFF_MARGINS";
      break;
    case visionapp::AnalysisStatus::PhoneTilted:
      status = @"PHONE_TILTED";
      break;
    case visionapp::AnalysisStatus::Ok:
      status = @"OK";
      break;
    case visionapp::AnalysisStatus::NotCentered:
    default:
      status = @"NOT_CENTERED";
      break;
  }

  NSString* lightingState;
  switch (result.lightingState) {
    case visionapp::LightingState::TooDark:
      lightingState = @"TOO_DARK";
      break;
    case visionapp::LightingState::Overexposed:
      lightingState = @"OVEREXPOSED";
      break;
    case visionapp::LightingState::Good:
    default:
      lightingState = @"GOOD";
      break;
  }

  NSString* orientationMode = result.orientationMode == visionapp::OrientationMode::Hanger ? @"HANGER" : @"FLAT";

  // Converted to a plain JS object by VisionCamera via JSI (no bridge).
  // Only the derived state/vectors below are meant for production UI; the
  // exact pixel/degree readings are gated behind #ifdef DEBUG so a release
  // build never ships raw sensor/vision telemetry to JS - see
  // ProductAnalyzer.h's AnalysisResult doc comment.
  NSMutableDictionary* payload = [@{
    @"found" : @(result.found),
    @"status" : status,
    @"message" : @(result.message.c_str()),
    @"isReadyForCapture" : @(result.isReadyForCapture),
    @"lightingState" : lightingState,
    @"orientationMode" : orientationMode,
    @"normalizedDx" : @(result.normalizedDx),
    @"normalizedDy" : @(result.normalizedDy),
    @"tilt" : @{
      @"dx" : @(result.tilt.dx),
      @"dy" : @(result.tilt.dy),
    },
    @"frameWidth" : @(result.frameWidth),
    @"frameHeight" : @(result.frameHeight),
    @"latencyMs" : @(result.latencyMs),
    @"processed" : @(result.processed),
    @"boundingBox" : @{
      @"x" : @(result.boundingBoxX),
      @"y" : @(result.boundingBoxY),
      @"width" : @(result.boundingBoxWidth),
      @"height" : @(result.boundingBoxHeight),
    },
  } mutableCopy];

#ifdef DEBUG
  payload[@"debug"] = @{
    @"centroid" : @{
      @"x" : @(result.centroidX),
      @"y" : @(result.centroidY),
    },
    @"contourArea" : @(result.contourArea),
    @"pitch" : @(result.pitchDegrees),
    @"roll" : @(result.rollDegrees),
  };
#endif

  return payload;
}

VISION_EXPORT_FRAME_PROCESSOR(ProductAnalyzerPlugin, analyzeProduct)

@end
