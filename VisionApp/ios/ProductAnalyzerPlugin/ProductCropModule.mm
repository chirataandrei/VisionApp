//
//  ProductCropModule.mm
//  VisionApp
//
//  Crops a captured photo to the bounding box of the product contour that
//  cpp/ProductAnalyzer.cpp detected in the analysis frame it was based on,
//  using the shared OpenCV pipeline (native, since OpenCV is already linked
//  into this app for the frame processor plugin - reusing it avoids adding a
//  separate JS image-manipulation dependency).
//

#import <React/RCTBridgeModule.h>

#import "ProductAnalyzer.h"

@interface ProductCropModule : NSObject <RCTBridgeModule>
@end

@implementation ProductCropModule

RCT_EXPORT_MODULE(ProductCropModule)

RCT_EXPORT_METHOD(cropToBoundingBox:(NSString*)sourcePath
                  destPath:(NSString*)destPath
                  boxX:(double)boxX
                  boxY:(double)boxY
                  boxWidth:(double)boxWidth
                  boxHeight:(double)boxHeight
                  analysisFrameWidth:(double)analysisFrameWidth
                  analysisFrameHeight:(double)analysisFrameHeight
                  resolve:(RCTPromiseResolveBlock)resolve
                  reject:(RCTPromiseRejectBlock)reject) {
  const bool success =
      visionapp::cropPhotoToBoundingBox(sourcePath.UTF8String, destPath.UTF8String, boxX, boxY, boxWidth, boxHeight,
                                         static_cast<int>(analysisFrameWidth), static_cast<int>(analysisFrameHeight));
  resolve(@(success));
}

+ (BOOL)requiresMainQueueSetup {
  return NO;
}

@end
