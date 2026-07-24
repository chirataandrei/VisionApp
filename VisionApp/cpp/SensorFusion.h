#pragma once

#include <mutex>

namespace visionapp {

/**
 * Fuses raw accelerometer + gyroscope samples into pitch/roll (degrees)
 * using a complementary filter.
 *
 * Axis convention is the Android sensor coordinate system:
 *   X → right edge of screen, Y → top edge of screen, Z → out of the screen.
 * Device flat on a table, screen up → pitch = 0, roll = 0.
 * (The iOS layer negates CoreMotion accelerometer values to match.)
 *
 * Thread-safe: sensor callbacks and the camera frame thread may call
 * concurrently.
 */
class SensorFusion {
 public:
  static SensorFusion& instance();

  /** Accelerometer sample. Units don't matter (only ratios are used). */
  void updateAccelerometer(double ax, double ay, double az);

  /** Gyroscope sample in rad/s, with the event timestamp in seconds. */
  void updateGyroscope(double gx, double gy, double gz, double timestampSeconds);

  struct Attitude {
    double pitchDegrees;
    double rollDegrees;
  };

  Attitude getAttitude();

 private:
  SensorFusion() = default;

  std::mutex _mutex;
  double _pitch = 0.0; // degrees
  double _roll = 0.0;  // degrees
  double _lastGyroTimestamp = -1.0;
  bool _hasAccel = false;
};

} // namespace visionapp
