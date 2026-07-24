package com.visionapp.nativemodules

import com.facebook.react.bridge.Promise
import com.facebook.react.bridge.ReactApplicationContext
import com.facebook.react.bridge.ReactContextBaseJavaModule
import com.facebook.react.bridge.ReactMethod

/**
 * Crops a captured photo to the bounding box of the product contour that
 * cpp/ProductAnalyzer.cpp detected in the analysis frame it was based on,
 * using the shared OpenCV pipeline (native, since OpenCV is already linked
 * into this app for the frame processor plugin - reusing it avoids adding a
 * separate JS image-manipulation dependency).
 */
class ProductCropModule(reactContext: ReactApplicationContext) : ReactContextBaseJavaModule(reactContext) {

  override fun getName(): String = NAME

  @ReactMethod
  fun cropToBoundingBox(
    sourcePath: String,
    destPath: String,
    boxX: Double,
    boxY: Double,
    boxWidth: Double,
    boxHeight: Double,
    analysisFrameWidth: Double,
    analysisFrameHeight: Double,
    promise: Promise
  ) {
    try {
      val success =
        nativeCropToBoundingBox(
          sourcePath,
          destPath,
          boxX,
          boxY,
          boxWidth,
          boxHeight,
          analysisFrameWidth.toInt(),
          analysisFrameHeight.toInt()
        )
      promise.resolve(success)
    } catch (e: Exception) {
      promise.reject("crop_failed", e)
    }
  }

  private external fun nativeCropToBoundingBox(
    sourcePath: String,
    destPath: String,
    boxX: Double,
    boxY: Double,
    boxWidth: Double,
    boxHeight: Double,
    analysisFrameWidth: Int,
    analysisFrameHeight: Int
  ): Boolean

  companion object {
    const val NAME = "ProductCropModule"

    init {
      System.loadLibrary("productanalyzer")
    }
  }
}
