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
    result = visionapp::analyzeFrame(data, width, height, bytesPerRow, visionapp::PixelLayout::GRAY8);
  } else {
    // Non-planar RGB camera output is 32BGRA on iOS.
    const auto* data = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddress(pixelBuffer));
    const int width = (int)CVPixelBufferGetWidth(pixelBuffer);
    const int height = (int)CVPixelBufferGetHeight(pixelBuffer);
    const size_t bytesPerRow = CVPixelBufferGetBytesPerRow(pixelBuffer);
    result = visionapp::analyzeFrame(data, width, height, bytesPerRow, visionapp::PixelLayout::BGRA8888);
  }

  CVPixelBufferUnlockBaseAddress(pixelBuffer, kCVPixelBufferLock_ReadOnly);

  NSString* exposureWarning;
  switch (result.exposureWarning) {
    case visionapp::ExposureWarning::TooDark:
      exposureWarning = @"dark";
      break;
    case visionapp::ExposureWarning::TooBright:
      exposureWarning = @"bright";
      break;
    case visionapp::ExposureWarning::None:
    default:
      exposureWarning = @"none";
      break;
  }

  // Converted to a plain JS object by VisionCamera via JSI (no bridge).
  return @{
    @"found" : @(result.found),
    @"centroid" : @{
      @"x" : @(result.centroidX),
      @"y" : @(result.centroidY),
    },
    @"contourArea" : @(result.contourArea),
    @"frameWidth" : @(result.frameWidth),
    @"frameHeight" : @(result.frameHeight),
    @"pitch" : @(result.pitchDegrees),
    @"roll" : @(result.rollDegrees),
    @"latencyMs" : @(result.latencyMs),
    @"processed" : @(result.processed),
    @"boundingBox" : @{
      @"x" : @(result.boundingBoxX),
      @"y" : @(result.boundingBoxY),
      @"width" : @(result.boundingBoxWidth),
      @"height" : @(result.boundingBoxHeight),
    },
    @"exposureWarning" : exposureWarning,
  };
}

VISION_EXPORT_FRAME_PROCESSOR(ProductAnalyzerPlugin, analyzeProduct)

@end
