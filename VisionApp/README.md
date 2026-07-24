# VisionApp

React Native (0.86, TypeScript) camera app with a **custom C++ frame processor plugin**: every camera frame is handed to native C++ **by reference via JSI** (zero copies, nothing crosses the legacy bridge), analyzed with **OpenCV**, fused with **accelerometer/gyroscope** data, and the result drives a **Reanimated** alignment overlay on the UI thread.

## What it does

- Full-screen [VisionCamera](https://react-native-vision-camera.com) view (`pixelFormat="yuv"`).
- A frame processor worklet calls the native plugin `analyzeProduct` on every camera frame (no `fps` throttle set on `<Camera>`); only the OpenCV work inside the plugin is throttled, to every 3rd call.
- In C++ (`cpp/ProductAnalyzer.cpp`): grayscale (Y-plane) → downscale to ~320 px → Gaussian blur → Canny edges → largest contour → **centroid**. Performance: only every 3rd frame is processed (the rest answer from cache with fresh pitch/roll), and the search is restricted to an ROI around the previous detection (full-frame fallback when lost). Pipeline execution time is measured in C++ and shown on screen.
- In C++ (`cpp/SensorFusion.cpp`): complementary filter over raw accelerometer + gyroscope → **pitch/roll** in degrees.
- The plugin returns `{ found, centroid: {x, y}, contourArea, frameWidth, frameHeight, pitch, roll }` to JS as a plain object (JSI conversion, no serialization).
- Reanimated overlay (`src/components/AlignmentOverlay.tsx`):
  - a 3D-styled **bubble level** driven by pitch/roll,
  - an **arrow** from the screen center to the detected object's centroid,
  - when the phone is flat (±3°) **and** the centroid is centered (<60 px), the borders **flash green** and a success **haptic** fires.

## Data flow

```
Camera HW frame ──(JSI HostObject, zero-copy)──▶ Frame Processor Plugin (native)
                                                  │  iOS: ProductAnalyzerPlugin.mm (Obj-C++, CVPixelBuffer plane 0)
                                                  │  Android: ProductAnalyzerPlugin.kt → JNI direct ByteBuffer
                                                  ▼
Accel/Gyro ──▶ SensorFusion.cpp ──▶ ProductAnalyzer.cpp (OpenCV pipeline)
                                                  │
                                    result dict/map ──(JSI)──▶ frame processor worklet
                                                  │
                              useRunOnJS ──▶ Reanimated shared values ──▶ overlay (UI thread)
```

Shared, platform-independent C++ lives in `cpp/`. The iOS plugin is packaged as a local CocoaPod (`ProductAnalyzerPlugin.podspec`); the Android plugin builds via CMake (`android/app/src/main/cpp/CMakeLists.txt`) and links the official OpenCV AAR through Gradle **prefab**.

## Building

```sh
npm install
```

### iOS

```sh
cd ios
bundle install          # once — installs CocoaPods
bundle exec pod install # pulls VisionCamera, Reanimated, OpenCV (~200 MB), the local plugin pod
cd ..
npx react-native run-ios --device   # see simulator caveat below
```

### Android

```sh
npx react-native run-android
```

Requires the Android NDK + CMake; if missing, Android Gradle Plugin auto-installs them on first build (SDK licenses are already accepted on this machine). The OpenCV native libs come from `org.opencv:opencv:4.10.0` on Maven Central.

## Known caveats

- **iOS simulator**: the `OpenCV` CocoaPod (4.3.x) ships no arm64-simulator slice — build for a physical device on Apple Silicon Macs.
- **Centroid → screen mapping** (`App.tsx`) is naive: it assumes a portrait UI over a landscape sensor frame. Rear/front rotation differences per device are not handled.
- **Gyro axis signs** in the complementary filter follow Android sensor conventions (iOS values are adapted in the plugin). If pitch/roll feel inverted on some hardware, flip the signs in `cpp/SensorFusion.cpp`.
- Frame processors on RN 0.86 rely on VisionCamera v4's use of the bridgeless interop layer (`BridgelessCatalystInstance`) — verified present in RN 0.86, but it is deprecated API and may disappear in a future RN release.

## Tuning

| Constant | Where | Meaning |
|---|---|---|
| `kCannyThresholdLow/High` | `cpp/ProductAnalyzer.cpp` | Edge sensitivity |
| `kGaussianKernelSize` | `cpp/ProductAnalyzer.cpp` | Blur strength |
| `kMinContourAreaFraction` | `cpp/ProductAnalyzer.cpp` | Noise rejection (fraction of frame area) |
| `kGyroWeight` | `cpp/SensorFusion.cpp` | Complementary filter: gyro vs accel trust |
| `LEVEL_THRESHOLD_DEG`, `CENTER_THRESHOLD_PX` | `src/components/AlignmentOverlay.tsx` | Alignment tolerances |
| `kProcessEveryNthFrame` | `cpp/ProductAnalyzer.cpp` | Frame analysis rate (OpenCV work vs. cached detection) |

Template docs from `@react-native-community/cli` are kept in `docs-react-native-template.md`.
