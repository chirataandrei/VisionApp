package com.visionapp.frameprocessors

import android.content.Context
import android.graphics.ImageFormat
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import com.facebook.react.bridge.LifecycleEventListener
import com.mrousavy.camera.frameprocessors.Frame
import com.mrousavy.camera.frameprocessors.FrameProcessorPlugin
import com.mrousavy.camera.frameprocessors.VisionCameraProxy
import com.visionapp.BuildConfig
import java.nio.ByteBuffer

/**
 * VisionCamera Frame Processor Plugin "analyzeProduct".
 *
 * Feeds raw accelerometer/gyroscope events and the camera frame's pixel
 * buffer (zero-copy direct ByteBuffer) into the shared C++ pipeline via JNI.
 */
class ProductAnalyzerPlugin(proxy: VisionCameraProxy, @Suppress("UNUSED_PARAMETER") options: Map<String, Any>?) :
  FrameProcessorPlugin(), SensorEventListener, LifecycleEventListener {

  private val reactContext = proxy.context
  private val sensorManager = reactContext.getSystemService(Context.SENSOR_SERVICE) as SensorManager

  init {
    sensorManager.getDefaultSensor(Sensor.TYPE_ACCELEROMETER)?.let {
      sensorManager.registerListener(this, it, SensorManager.SENSOR_DELAY_GAME)
    }
    sensorManager.getDefaultSensor(Sensor.TYPE_GYROSCOPE)?.let {
      sensorManager.registerListener(this, it, SensorManager.SENSOR_DELAY_GAME)
    }
    // FrameProcessorPlugin has no destroy/dispose hook, so this is the only
    // reliable signal to unregister the sensor listener (RN reload, activity
    // destroy) - without it, this plugin instance and its listener registration
    // leak for the lifetime of the SensorManager (i.e. the process).
    reactContext.addLifecycleEventListener(this)
  }

  override fun onHostResume() = Unit
  override fun onHostPause() = Unit

  override fun onHostDestroy() {
    sensorManager.unregisterListener(this)
    reactContext.removeLifecycleEventListener(this)
  }

  override fun onSensorChanged(event: SensorEvent) {
    when (event.sensor.type) {
      Sensor.TYPE_ACCELEROMETER ->
        // Android reports TYPE_ACCELEROMETER in m/s² (~9.81 magnitude at
        // rest); SensorFusion's dynamic accelerometer weighting gates on
        // magnitude relative to 1g, so normalize to g's here to match what
        // CoreMotion already reports on iOS - see SensorFusion.h.
        nativeUpdateAccelerometer(
          (event.values[0] / SensorManager.GRAVITY_EARTH).toDouble(),
          (event.values[1] / SensorManager.GRAVITY_EARTH).toDouble(),
          (event.values[2] / SensorManager.GRAVITY_EARTH).toDouble()
        )
      Sensor.TYPE_GYROSCOPE ->
        nativeUpdateGyroscope(
          event.values[0].toDouble(),
          event.values[1].toDouble(),
          event.values[2].toDouble(),
          event.timestamp / 1_000_000_000.0
        )
    }
  }

  override fun onAccuracyChanged(sensor: Sensor?, accuracy: Int) = Unit

  private fun statusLabel(code: Int): String =
    when (code) {
      1 -> "CUT_OFF_MARGINS"
      2 -> "PHONE_TILTED"
      3 -> "OK"
      else -> "NOT_CENTERED"
    }

  private fun lightingStateLabel(code: Int): String =
    when (code) {
      0 -> "TOO_DARK"
      1 -> "OVEREXPOSED"
      else -> "GOOD"
    }

  private fun orientationModeLabel(code: Int): String = if (code == 1) "HANGER" else "FLAT"

  // Mirrors kMessages in ProductAnalyzer.cpp - the single source of truth is
  // there; JNI can't marshal that std::string across, so messageCode (a
  // small int) crosses instead and gets looked up here. Keep in sync.
  private fun messageForCode(code: Int): String =
    when (code) {
      1 -> "Îndepărtează camera"
      2 -> "Centrează haina"
      3 -> "Ține telefonul mai drept"
      4 -> "Perfect!"
      else -> "Așează haina în cadru"
    }

  // Indices into nativeAnalyzeFrame's returned array - see the doc comment
  // above ProductAnalyzerJNI.cpp's implementation for the full layout.
  override fun callback(frame: Frame, params: Map<String, Any>?): Any? {
    if (params?.get("reset") == true) {
      nativeResetPipeline()
    }

    val image = frame.image
    val plane = image.planes[0]
    // For YUV_420_888 frames, plane 0 is the luma (Y) plane — already grayscale.
    val isGrayscale = image.format == ImageFormat.YUV_420_888

    // Planes 1/2 (U/V), when present, feed the native color-saturation mask
    // that helps distinguish garments from neutral-toned backgrounds (a TV,
    // a wall, a wood floor). YUV_420_888 doesn't guarantee U/V are
    // interleaved like iOS's biplanar CbCr - pixelStride varies by
    // device/vendor - so each plane's own stride is passed through rather
    // than assumed. Chroma dimensions are the standard 4:2:0 half-rounded-up
    // luma dimensions; Image.Plane itself doesn't expose a width/height.
    val uPlane = if (isGrayscale) image.planes.getOrNull(1) else null
    val vPlane = if (isGrayscale) image.planes.getOrNull(2) else null
    val chromaWidth = if (uPlane != null) (frame.width + 1) / 2 else 0
    val chromaHeight = if (uPlane != null) (frame.height + 1) / 2 else 0

    val values =
      nativeAnalyzeFrame(
        plane.buffer,
        frame.width,
        frame.height,
        plane.rowStride,
        isGrayscale,
        uPlane?.buffer,
        uPlane?.rowStride ?: 0,
        uPlane?.pixelStride ?: 1,
        vPlane?.buffer,
        vPlane?.rowStride ?: 0,
        vPlane?.pixelStride ?: 1,
        chromaWidth,
        chromaHeight
      )

    // Only the derived state/vectors below are meant for production UI; the
    // exact pixel/degree readings (array indices 18-22) are gated behind
    // BuildConfig.DEBUG so a release build never ships raw sensor/vision
    // telemetry to JS - mirrors ProductAnalyzerPlugin.mm's #ifdef DEBUG on
    // iOS.
    val payload =
      hashMapOf<String, Any>(
        "found" to (values[0] != 0.0),
        "status" to statusLabel(values[1].toInt()),
        "message" to messageForCode(values[2].toInt()),
        "isReadyForCapture" to (values[3] != 0.0),
        "lightingState" to lightingStateLabel(values[4].toInt()),
        "orientationMode" to orientationModeLabel(values[5].toInt()),
        "normalizedDx" to values[6],
        "normalizedDy" to values[7],
        "tilt" to hashMapOf("dx" to values[8], "dy" to values[9]),
        "frameWidth" to values[10].toInt(),
        "frameHeight" to values[11].toInt(),
        "latencyMs" to values[12],
        "processed" to (values[13] != 0.0),
        "boundingBox" to
          hashMapOf(
            "x" to values[14],
            "y" to values[15],
            "width" to values[16],
            "height" to values[17]
          )
      )

    if (BuildConfig.DEBUG) {
      payload["debug"] =
        hashMapOf(
          "centroid" to hashMapOf("x" to values[18], "y" to values[19]),
          "contourArea" to values[20],
          "pitch" to values[21],
          "roll" to values[22]
        )
    }

    return payload
  }

  companion object {
    init {
      System.loadLibrary("productanalyzer")
    }

    @JvmStatic
    private external fun nativeUpdateAccelerometer(ax: Double, ay: Double, az: Double)

    @JvmStatic
    private external fun nativeUpdateGyroscope(gx: Double, gy: Double, gz: Double, timestampSeconds: Double)

    @JvmStatic
    private external fun nativeResetPipeline()

    @JvmStatic
    private external fun nativeAnalyzeFrame(
      buffer: ByteBuffer,
      width: Int,
      height: Int,
      rowStride: Int,
      isGrayscale: Boolean,
      uBuffer: ByteBuffer?,
      uRowStride: Int,
      uPixelStride: Int,
      vBuffer: ByteBuffer?,
      vRowStride: Int,
      vPixelStride: Int,
      chromaWidth: Int,
      chromaHeight: Int
    ): DoubleArray
  }
}
