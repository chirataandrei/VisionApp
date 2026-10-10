# Vinted Start-up

A mobile app that helps Vinted sellers take great product photos effortlessly: the camera detects the garment in real time, gives live instructions, and once everything is aligned it takes the picture by itself and crops it to the product.

## Repository structure

| Folder | Contents |
|---|---|
| [`VisionApp/`](VisionApp/) | The React Native (TypeScript) app plus the native C++/OpenCV plugin |
| `VisionApp/cpp/` | Vision pipeline: `ProductAnalyzer`, `SensorFusion`, `SegmentationEngine` |
| `VisionApp/src/` | UI, hooks (auto-capture, photo workflow, alignment), frame processor |
| `VisionApp/android/`, `VisionApp/ios/` | Native projects |

Full technical documentation (pipeline, thresholds, design decisions, caveats) lives in [`VisionApp/README.md`](VisionApp/README.md).

## Features

- **Real-time detection** of the product, processed in C++ with OpenCV directly on camera frames (JSI, zero copies).
- **Live guidance**: one message at a time (e.g. "Center the garment", "Hold the phone straighter").
- **Sensor fusion** (accelerometer + gyroscope) for tilt and flat-lay / on-hanger mode.
- **Auto-capture + auto-crop** once the framing is correct and steady.
- **Vinted photo flow**: Front → Back → Label → Defects/Details.

## Quick start

Requirements: Node.js, JDK + Android SDK/NDK (Android) or Xcode + CocoaPods (iOS).

```sh
cd VisionApp
npm install

# iOS
cd ios && bundle install && bundle exec pod install && cd ..
npx react-native run-ios

# Android
npx react-native run-android
```

Start Metro separately with `npm start`. Run tests with `npm test`.

## Tech stack

React Native 0.86 · TypeScript · VisionCamera · Reanimated · C++20 · OpenCV 4.10 · LiteRT for ML segmentation (in preparation).

## Status

Under active development. Tested on an Android emulator; the build is configured for `arm64-v8a` and `x86_64`. Real-device benchmarks have not been recorded yet.
