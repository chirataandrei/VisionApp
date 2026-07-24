#include "SensorFusion.h"

#include <cmath>

namespace visionapp {

namespace {
constexpr double kRadToDeg = 180.0 / M_PI;
// Complementary filter: trust the gyro integration short-term (no noise),
// let the accelerometer's gravity estimate correct long-term drift.
constexpr double kGyroWeight = 0.96;
} // namespace

SensorFusion& SensorFusion::instance() {
  static SensorFusion fusion;
  return fusion;
}

void SensorFusion::updateAccelerometer(double ax, double ay, double az) {
  // Gravity-derived attitude (see header for axis convention):
  //   pitch: top edge raised → Y gains a vertical component
  //   roll:  right edge lowered → X gains a vertical component
  const double accelPitch = std::atan2(ay, std::sqrt(ax * ax + az * az)) * kRadToDeg;
  const double accelRoll = std::atan2(-ax, az) * kRadToDeg;

  std::lock_guard<std::mutex> lock(_mutex);
  if (!_hasAccel) {
    // First sample: snap directly to the accelerometer estimate.
    _pitch = accelPitch;
    _roll = accelRoll;
    _hasAccel = true;
    return;
  }
  _pitch = kGyroWeight * _pitch + (1.0 - kGyroWeight) * accelPitch;
  _roll = kGyroWeight * _roll + (1.0 - kGyroWeight) * accelRoll;
}

void SensorFusion::updateGyroscope(double gx, double gy, double gz, double timestampSeconds) {
  std::lock_guard<std::mutex> lock(_mutex);
  if (_lastGyroTimestamp >= 0.0) {
    const double dt = timestampSeconds - _lastGyroTimestamp;
    if (dt > 0.0 && dt < 0.5) {
      // Integrate angular velocity: pitch rotates about X, roll about Y.
      _pitch += gx * dt * kRadToDeg;
      _roll += gy * dt * kRadToDeg;
    }
  }
  _lastGyroTimestamp = timestampSeconds;
}

SensorFusion::Attitude SensorFusion::getAttitude() {
  std::lock_guard<std::mutex> lock(_mutex);
  return {_pitch, _roll};
}

} // namespace visionapp
