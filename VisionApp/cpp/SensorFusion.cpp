#include "SensorFusion.h"

#include <cmath>

namespace visionapp {

namespace {
constexpr double kRadToDeg = 180.0 / M_PI;
// Complementary filter: trust the gyro integration short-term (no noise),
// let the accelerometer's gravity estimate correct long-term drift - but
// only while the device is stable (see kStableMagnitudeToleranceG below);
// this is the weight used in that case, not a fixed constant applied
// unconditionally.
constexpr double kGyroWeight = 0.96;
// Smoothing factor for the raw accelerometer sample low-pass filter (0 < a
// <= 1): lower damps high-frequency sensor noise/vibration more but adds
// lag. Applied before the sample is used for anything else, so both the
// magnitude gate and the gravity-direction estimate see the smoothed value.
constexpr double kAccelLowPassAlpha = 0.2;
// Expected accelerometer magnitude when the device is stationary, in g's -
// see SensorFusion.h for the unit contract callers must uphold.
constexpr double kGravityMagnitudeG = 1.0;
// Half-width of the "stable" band around 1g. Outside this band the device
// is being actively moved, so the accelerometer's gravity direction is
// contaminated by real linear acceleration and can't be trusted at all -
// see updateAccelerometer.
constexpr double kStableMagnitudeToleranceG = 0.1;

// Hysteresis band around the flat/hanger pitch boundary (~45°): entering
// Hanger requires pitch to climb past the HIGH threshold, returning to Flat
// requires it to drop past the (lower) LOW threshold - so holding the phone
// right at 45° can't cause the reported mode to flap back and forth.
constexpr double kHangerPitchEnterDeg = 50.0;
constexpr double kFlatPitchEnterDeg = 40.0;
} // namespace

SensorFusion& SensorFusion::instance() {
  static SensorFusion fusion;
  return fusion;
}

void SensorFusion::updateAccelerometer(double ax, double ay, double az) {
  std::lock_guard<std::mutex> lock(_mutex);

  if (!_hasAccel) {
    // First sample: seed the low-pass filter and snap pitch/roll directly
    // to the (unfiltered, single-sample) accelerometer estimate.
    _filteredAx = ax;
    _filteredAy = ay;
    _filteredAz = az;
    _pitch = std::atan2(_filteredAy, std::sqrt(_filteredAx * _filteredAx + _filteredAz * _filteredAz)) * kRadToDeg;
    _roll = std::atan2(-_filteredAx, _filteredAz) * kRadToDeg;
    _hasAccel = true;
    return;
  }

  // Low-pass the raw sample first, so both the magnitude gate and the
  // gravity-direction estimate below see a value damped of high-frequency
  // sensor noise/vibration rather than a single noisy instant.
  _filteredAx = kAccelLowPassAlpha * ax + (1.0 - kAccelLowPassAlpha) * _filteredAx;
  _filteredAy = kAccelLowPassAlpha * ay + (1.0 - kAccelLowPassAlpha) * _filteredAy;
  _filteredAz = kAccelLowPassAlpha * az + (1.0 - kAccelLowPassAlpha) * _filteredAz;

  // Gravity-derived attitude (see header for axis convention):
  //   pitch: top edge raised → Y gains a vertical component
  //   roll:  right edge lowered → X gains a vertical component
  const double accelPitch = std::atan2(_filteredAy, std::sqrt(_filteredAx * _filteredAx + _filteredAz * _filteredAz)) *
                             kRadToDeg;
  const double accelRoll = std::atan2(-_filteredAx, _filteredAz) * kRadToDeg;

  // Dynamic weighting: the accelerometer's gravity estimate is only
  // meaningful while the device is stable (net acceleration ≈ 1g). Under
  // active motion, "down" as measured by the accelerometer is contaminated
  // by real linear acceleration and would inject large orientation errors
  // if blended in - so outside the stable band, trust the gyro completely
  // (weight 1.0) instead of the fixed kGyroWeight.
  const double magnitude =
      std::sqrt(_filteredAx * _filteredAx + _filteredAy * _filteredAy + _filteredAz * _filteredAz);
  const bool isStable = std::abs(magnitude - kGravityMagnitudeG) <= kStableMagnitudeToleranceG;
  const double gyroWeight = isStable ? kGyroWeight : 1.0;

  _pitch = gyroWeight * _pitch + (1.0 - gyroWeight) * accelPitch;
  _roll = gyroWeight * _roll + (1.0 - gyroWeight) * accelRoll;
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

  // Hysteresis: only flip _isFlatMode once pitch has solidly crossed into
  // the new mode's territory, not merely past the nominal ~45° midpoint -
  // see kHangerPitchEnterDeg/kFlatPitchEnterDeg.
  const double absPitch = std::abs(_pitch);
  if (_isFlatMode && absPitch >= kHangerPitchEnterDeg) {
    _isFlatMode = false;
  } else if (!_isFlatMode && absPitch <= kFlatPitchEnterDeg) {
    _isFlatMode = true;
  }

  return {_pitch, _roll, _isFlatMode};
}

} // namespace visionapp
