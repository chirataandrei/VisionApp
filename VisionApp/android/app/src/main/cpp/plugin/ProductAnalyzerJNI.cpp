//
// JNI bridge between ProductAnalyzerPlugin.kt and the shared C++ pipeline
// (cpp/ProductAnalyzer.cpp, cpp/SensorFusion.cpp).
//
// The frame buffer arrives as a direct ByteBuffer wrapping the camera's
// Image plane — GetDirectBufferAddress yields the raw pixel pointer with
// zero copies.
//

#include <jni.h>

#include "ProductAnalyzer.h"
#include "SensorFusion.h"

extern "C" JNIEXPORT void JNICALL
Java_com_visionapp_frameprocessors_ProductAnalyzerPlugin_nativeUpdateAccelerometer(
    JNIEnv* /*env*/, jclass /*clazz*/, jdouble ax, jdouble ay, jdouble az) {
  visionapp::SensorFusion::instance().updateAccelerometer(ax, ay, az);
}

extern "C" JNIEXPORT void JNICALL
Java_com_visionapp_frameprocessors_ProductAnalyzerPlugin_nativeUpdateGyroscope(
    JNIEnv* /*env*/, jclass /*clazz*/, jdouble gx, jdouble gy, jdouble gz, jdouble timestampSeconds) {
  visionapp::SensorFusion::instance().updateGyroscope(gx, gy, gz, timestampSeconds);
}

extern "C" JNIEXPORT void JNICALL
Java_com_visionapp_frameprocessors_ProductAnalyzerPlugin_nativeResetPipeline(
    JNIEnv* /*env*/, jclass /*clazz*/) {
  visionapp::resetPipelineState();
}

// Returns [found, centroidX, centroidY, contourArea, frameWidth, frameHeight,
//          pitch, roll, latencyMs, processed, boundingBoxX, boundingBoxY,
//          boundingBoxWidth, boundingBoxHeight, exposureWarning].
extern "C" JNIEXPORT jdoubleArray JNICALL
Java_com_visionapp_frameprocessors_ProductAnalyzerPlugin_nativeAnalyzeFrame(
    JNIEnv* env, jclass /*clazz*/, jobject buffer, jint width, jint height, jint rowStride, jboolean isGrayscale) {
  const auto* data = static_cast<const uint8_t*>(env->GetDirectBufferAddress(buffer));

  const auto layout = isGrayscale ? visionapp::PixelLayout::GRAY8 : visionapp::PixelLayout::RGBA8888;
  const visionapp::AnalysisResult result =
      visionapp::analyzeFrame(data, width, height, static_cast<size_t>(rowStride), layout);

  const double values[15] = {
      result.found ? 1.0 : 0.0,
      result.centroidX,
      result.centroidY,
      result.contourArea,
      static_cast<double>(result.frameWidth),
      static_cast<double>(result.frameHeight),
      result.pitchDegrees,
      result.rollDegrees,
      result.latencyMs,
      result.processed ? 1.0 : 0.0,
      result.boundingBoxX,
      result.boundingBoxY,
      result.boundingBoxWidth,
      result.boundingBoxHeight,
      static_cast<double>(static_cast<int>(result.exposureWarning)),
  };
  jdoubleArray array = env->NewDoubleArray(15);
  env->SetDoubleArrayRegion(array, 0, 15, values);
  return array;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_visionapp_nativemodules_ProductCropModule_nativeCropToBoundingBox(
    JNIEnv* env, jclass /*clazz*/, jstring sourcePath, jstring destPath, jdouble boxX, jdouble boxY,
    jdouble boxWidth, jdouble boxHeight, jint analysisFrameWidth, jint analysisFrameHeight) {
  const char* sourceChars = env->GetStringUTFChars(sourcePath, nullptr);
  const char* destChars = env->GetStringUTFChars(destPath, nullptr);

  const bool success = visionapp::cropPhotoToBoundingBox(sourceChars, destChars, boxX, boxY, boxWidth, boxHeight,
                                                          analysisFrameWidth, analysisFrameHeight);

  env->ReleaseStringUTFChars(sourcePath, sourceChars);
  env->ReleaseStringUTFChars(destPath, destChars);
  return success ? JNI_TRUE : JNI_FALSE;
}
