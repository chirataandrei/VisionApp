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

// Returns [found, status, messageCode, isReadyForCapture, lightingState,
//          orientationMode, normalizedDx, normalizedDy, tiltDx, tiltDy,
//          frameWidth, frameHeight, latencyMs, processed, boundingBoxX,
//          boundingBoxY, boundingBoxWidth, boundingBoxHeight,
//          debugCentroidX, debugCentroidY, debugContourArea, debugPitch,
//          debugRoll].
//
// This array is an internal native<->native boundary (JNI can only marshal
// primitives/arrays, not the AnalysisResult struct - notably not
// AnalysisResult::message, a std::string, so messageCode is transmitted
// instead and ProductAnalyzerPlugin.kt keeps its own copy of the five
// message strings to look it up, mirroring kMessages in ProductAnalyzer.cpp)
// - it is NOT what reaches JS. ProductAnalyzerPlugin.kt decides which of
// these values actually enter the JS-visible map, gating the trailing
// "debug*" slots behind BuildConfig.DEBUG the same way
// ProductAnalyzerPlugin.mm gates them behind #ifdef DEBUG on iOS. The
// debug* slots are always present in this array (zero-filled in release
// builds) so the array length - and Kotlin's parsing of it - doesn't vary
// by build type.
extern "C" JNIEXPORT jdoubleArray JNICALL
Java_com_visionapp_frameprocessors_ProductAnalyzerPlugin_nativeAnalyzeFrame(
    JNIEnv* env, jclass /*clazz*/, jobject buffer, jint width, jint height, jint rowStride, jboolean isGrayscale,
    jobject uBuffer, jint uRowStride, jint uPixelStride, jobject vBuffer, jint vRowStride, jint vPixelStride,
    jint chromaWidth, jint chromaHeight) {
  const auto* data = static_cast<const uint8_t*>(env->GetDirectBufferAddress(buffer));

  const auto layout = isGrayscale ? visionapp::PixelLayout::GRAY8 : visionapp::PixelLayout::RGBA8888;

  // U/V planes are only meaningful (and only ever passed by
  // ProductAnalyzerPlugin.kt) for the YUV_420_888/GRAY8 path - see
  // ChromaPlane's doc comment for why Android needs its own pixelStride
  // (unlike iOS's always-interleaved biplanar CbCr, YUV_420_888's chroma
  // planes may be fully planar or interleaved depending on device/vendor).
  visionapp::ChromaPlane chromaU;
  visionapp::ChromaPlane chromaV;
  if (uBuffer != nullptr && vBuffer != nullptr) {
    chromaU = {static_cast<const uint8_t*>(env->GetDirectBufferAddress(uBuffer)), static_cast<size_t>(uRowStride),
               uPixelStride};
    chromaV = {static_cast<const uint8_t*>(env->GetDirectBufferAddress(vBuffer)), static_cast<size_t>(vRowStride),
               vPixelStride};
  }

  const visionapp::AnalysisResult result = visionapp::analyzeFrame(
      data, width, height, static_cast<size_t>(rowStride), layout, chromaWidth, chromaHeight, chromaU, chromaV);

#ifdef DEBUG
  const double debugCentroidX = result.centroidX;
  const double debugCentroidY = result.centroidY;
  const double debugContourArea = result.contourArea;
  const double debugPitch = result.pitchDegrees;
  const double debugRoll = result.rollDegrees;
#else
  const double debugCentroidX = 0.0;
  const double debugCentroidY = 0.0;
  const double debugContourArea = 0.0;
  const double debugPitch = 0.0;
  const double debugRoll = 0.0;
#endif

  const double values[23] = {
      result.found ? 1.0 : 0.0,
      static_cast<double>(static_cast<int>(result.status)),
      static_cast<double>(result.messageCode),
      result.isReadyForCapture ? 1.0 : 0.0,
      static_cast<double>(static_cast<int>(result.lightingState)),
      static_cast<double>(static_cast<int>(result.orientationMode)),
      result.normalizedDx,
      result.normalizedDy,
      result.tilt.dx,
      result.tilt.dy,
      static_cast<double>(result.frameWidth),
      static_cast<double>(result.frameHeight),
      result.latencyMs,
      result.processed ? 1.0 : 0.0,
      result.boundingBoxX,
      result.boundingBoxY,
      result.boundingBoxWidth,
      result.boundingBoxHeight,
      debugCentroidX,
      debugCentroidY,
      debugContourArea,
      debugPitch,
      debugRoll,
  };
  jdoubleArray array = env->NewDoubleArray(23);
  env->SetDoubleArrayRegion(array, 0, 23, values);
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
