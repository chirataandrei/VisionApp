#pragma once

#include <mutex>

namespace visionapp {

/**
 * Fuses raw accelerometer + gyroscope samples into pitch/roll (degrees)
 * using a complementary filter with dynamic accelerometer weighting (see
 * updateAccelerometer), plus a hysteresis-filtered flat-lay/on-hanger mode
 * classification (see getAttitude).
 *
 * Axis convention is the Android sensor coordinate system:
 *   X → right edge of screen, Y → top edge of screen, Z → out of the screen.
 * Device flat on a table, screen up → pitch = 0, roll = 0.
 * (The iOS layer negates CoreMotion accelerometer values to match.)
 *
 * Accelerometer units: g's (1.0g = standard gravity, ~9.81 m/s²) - unlike
 * pitch/roll (computed via atan2 ratios, so scale-invariant), the dynamic
 * weighting below gates on the sample's absolute magnitude relative to 1g,
 * so both platform bridges must normalize to this unit before calling
 * (CoreMotion's CMAccelerometerData already reports g's; Android's
 * SensorEvent reports m/s² and must be divided by SensorManager.GRAVITY_EARTH
 * first - see ProductAnalyzerPlugin.kt).
 *
 * Thread-safe: sensor callbacks and the camera frame thread may call
 * concurrently.
 */
class SensorFusion {
 public:
  static SensorFusion& instance();

  /** Accelerometer sample, in g's - see class doc comment. */
  void updateAccelerometer(double ax, double ay, double az);

  /** Gyroscope sample in rad/s, with the event timestamp in seconds. */
  void updateGyroscope(double gx, double gy, double gz, double timestampSeconds);

  struct Attitude {
    double pitchDegrees;
    double rollDegrees;
    /**
     * True for flat-lay (phone roughly horizontal), false for on-hanger
     * (phone roughly vertical) - hysteresis-filtered around the ~45° pitch
     * boundary (see getAttitude) so holding the phone right at that angle
     * can't cause the UI to flap between the two guidance modes.
     */
    bool isFlatMode;
  };

  Attitude getAttitude();

 private:
  SensorFusion() = default;

  std::mutex _mutex;
  double _pitch = 0.0; // degrees
  double _roll = 0.0;  // degrees
  double _lastGyroTimestamp = -1.0;
  bool _hasAccel = false;

  // Low-pass-filtered accelerometer sample (see updateAccelerometer), in
  // g's. Smooths high-frequency sensor noise/vibration out of both the
  // magnitude gate and the gravity-direction estimate.
  double _filteredAx = 0.0;
  double _filteredAy = 0.0;
  double _filteredAz = 0.0;

  // Hysteresis state for the flat/hanger mode classification - see
  // getAttitude().
  bool _isFlatMode = true;
};

} // namespace visionapp
