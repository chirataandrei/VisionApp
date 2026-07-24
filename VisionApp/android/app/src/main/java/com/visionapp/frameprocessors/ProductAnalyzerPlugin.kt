package com.visionapp.frameprocessors

import android.content.Context
import android.graphics.ImageFormat
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import com.mrousavy.camera.frameprocessors.Frame
import com.mrousavy.camera.frameprocessors.FrameProcessorPlugin
import com.mrousavy.camera.frameprocessors.VisionCameraProxy
import java.nio.ByteBuffer

/**
 * VisionCamera Frame Processor Plugin "analyzeProduct".
 *
 * Feeds raw accelerometer/gyroscope events and the camera frame's pixel
 * buffer (zero-copy direct ByteBuffer) into the shared C++ pipeline via JNI.
 */
class ProductAnalyzerPlugin(proxy: VisionCameraProxy, @Suppress("UNUSED_PARAMETER") options: Map<String, Any>?) :
  FrameProcessorPlugin(), SensorEventListener {

  init {
    val sensorManager = proxy.context.getSystemService(Context.SENSOR_SERVICE) as SensorManager
    sensorManager.getDefaultSensor(Sensor.TYPE_ACCELEROMETER)?.let {
      sensorManager.registerListener(this, it, SensorManager.SENSOR_DELAY_GAME)
    }
    sensorManager.getDefaultSensor(Sensor.TYPE_GYROSCOPE)?.let {
      sensorManager.registerListener(this, it, SensorManager.SENSOR_DELAY_GAME)
    }
  }

  override fun onSensorChanged(event: SensorEvent) {
    when (event.sensor.type) {
      Sensor.TYPE_ACCELEROMETER ->
        nativeUpdateAccelerometer(event.values[0].toDouble(), event.values[1].toDouble(), event.values[2].toDouble())
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

  override fun callback(frame: Frame, params: Map<String, Any>?): Any? {
    val image = frame.image
    val plane = image.planes[0]
    // For YUV_420_888 frames, plane 0 is the luma (Y) plane — already grayscale.
    val isGrayscale = image.format == ImageFormat.YUV_420_888

    val values = nativeAnalyzeFrame(plane.buffer, frame.width, frame.height, plane.rowStride, isGrayscale)

    return hashMapOf(
      "found" to (values[0] != 0.0),
      "centroid" to hashMapOf("x" to values[1], "y" to values[2]),
      "contourArea" to values[3],
      "frameWidth" to values[4].toInt(),
      "frameHeight" to values[5].toInt(),
      "pitch" to values[6],
      "roll" to values[7],
      "latencyMs" to values[8],
      "processed" to (values[9] != 0.0)
    )
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
    private external fun nativeAnalyzeFrame(
      buffer: ByteBuffer,
      width: Int,
      height: Int,
      rowStride: Int,
      isGrayscale: Boolean
    ): DoubleArray
  }
}
