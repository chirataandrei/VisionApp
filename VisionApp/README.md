# VisionApp

React Native (0.86, TypeScript) camera app with a **custom C++ frame processor plugin**: every camera frame is handed to native C++ **by reference via JSI** (zero copies, nothing crosses the legacy bridge), analyzed with **OpenCV**, fused with **accelerometer/gyroscope** data, and the result drives a **Reanimated** alignment overlay on the UI thread.

## What it does

- Full-screen [VisionCamera](https://react-native-vision-camera.com) view (`pixelFormat="yuv"`).
- A frame processor worklet calls the native plugin `analyzeProduct` on every camera frame (no `fps` throttle set on `<Camera>`); only the OpenCV work inside the plugin is throttled, to every 3rd call.
- In C++ (`cpp/ProductAnalyzer.cpp`): grayscale (Y-plane) → downscale to ~320 px (`INTER_AREA`, area-averaging to avoid aliasing fabric texture into moiré) → blur → adaptive local-contrast threshold (polarity-agnostic; a pixel counts as foreground only if it stands out from its own local neighborhood mean, so smooth cast shadows fall below threshold while true edges don't) → morphological close (bridges a patterned garment's internal edges into one silhouette) → largest contour → EMA-smoothed **centroid** (damps frame-to-frame jitter in the on-screen arrow; the ROI tracker itself still follows the raw, unsmoothed detection). Performance: only every 3rd frame is processed (the rest answer from cache with fresh pitch/roll), and the search is restricted to an ROI around the previous detection (full-frame fallback when lost). Pipeline execution time is measured in C++ and shown on screen.
- In C++ (`cpp/SensorFusion.cpp`): complementary filter over raw accelerometer + gyroscope → **pitch/roll** in degrees.
- The plugin returns `{ found, centroid: {x, y}, contourArea, frameWidth, frameHeight, boundingBox: {x, y, width, height}, pitch, roll, exposureWarning }` to JS as a plain object (JSI conversion, no serialization). `exposureWarning` (`'none' | 'dark' | 'bright'`) comes from a brightness-histogram check in C++ (shadow/highlight clipping fractions).
- Reanimated overlay (`src/components/AlignmentOverlay.tsx`):
  - a 3D-styled **bubble level** driven by pitch/roll,
  - an **arrow** from the screen center to the detected object's centroid,
  - when the phone is flat (±3°) **and** the centroid is centered (<60 px), the borders **flash green** and a success **haptic** fires (rate-limited by `HAPTIC_COOLDOWN_MS`).
- **Auto-capture + manual shutter** (`App.tsx`): a `ShutterButton` for manual capture, plus automatic `camera.takePhoto()` once alignment holds for `AUTO_CAPTURE_ALIGNED_MS` (500 ms).
- **Vinted multi-photo workflow** (`src/components/WorkflowStepper.tsx`): steps through Poza de Fata → Poza de Spate → Eticheta Marime/Brand → Defecte/Detalii, advancing on each successful capture.
- **Auto-crop** (`src/native/ProductCropModule.ts` → native `ProductCropModule`): after capture, if a product was detected, crops the photo to its bounding box via `cpp/ProductAnalyzer.cpp`'s `cropPhotoToBoundingBox` (OpenCV `imread`/`imwrite` - no extra JS image library needed since OpenCV is already linked). The box is converted from the frame processor's raw sensor-space coordinates into the photo's upright pixel space (`src/utils/cameraTransforms.ts`'s `mapBoxToUpright`) before cropping.

## Project structure

`App.tsx` is a thin composition layer; the actual logic lives in:

- `src/hooks/useProductAlignment.ts` — owns the frame processor and the Reanimated shared values it drives, plus the latest JS-visible detection (for the info panel and, via a ref, for capture-time cropping).
- `src/hooks/useAutoCapture.ts` — starts/cancels the 500 ms alignment-hold timer that triggers an automatic capture.
- `src/hooks/usePhotoWorkflow.ts` — drives the Vinted multi-photo steps: takes the photo, crops it if a detection exists, stores it under the current step, advances.
- `src/utils/geometry.ts` — generic point/box primitives (`distance`, `boundingBoxOfPoints`); `distance` carries the `'worklet'` directive so `AlignmentOverlay` can call it from UI-thread derived values.
- `src/utils/cameraTransforms.ts` — orientation-aware frame ↔ screen/photo coordinate mapping (`mapFrameToScreen`, `mapBoxToUpright`), built on `geometry.ts`.
- `src/components/` — `AlignmentOverlay`, `ShutterButton`, `WorkflowStepper`, `QualityBadges` (presentational only; no business logic).
- `src/frameProcessors/productAnalyzer.ts` / `src/native/ProductCropModule.ts` — the JS-side native bridges described above.

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

Cropping is a *separate* native module (not the frame processor plugin), since it runs after an async `takePhoto()` on the JS thread, with no live `Frame` object available at that point:
- Android: `com.visionapp.nativemodules.ProductCropModule` (+ `ProductCropPackage`, registered manually in `MainApplication.kt`), calling the same `productanalyzer` shared library via a new JNI export.
- iOS: `ios/ProductAnalyzerPlugin/ProductCropModule.mm`, an `RCTBridgeModule` calling `cropPhotoToBoundingBox` directly (already globbed by the podspec, no extra registration needed).

Both are classic (pre-TurboModule) native modules, which continue to work under React Native's New Architecture (enabled here, `newArchEnabled=true`) via its backward-compatibility interop layer - the same mechanism VisionCamera itself relies on for its frame processor plugin registration.

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
- **Centroid → screen mapping** (`src/utils/cameraTransforms.ts`, `mapFrameToScreen`) uses VisionCamera's `Frame.orientation` and handles all four sensor orientations via a consistent clockwise rotation-by-degrees convention, but the exact rotation direction per orientation hasn't been verified against physical hardware for both platforms — check it on-device and adjust the per-case mapping in `mapPointToUpright` if the arrow points the wrong way.
- **Gyro axis signs** in the complementary filter follow Android sensor conventions (iOS values are adapted in the plugin). If pitch/roll feel inverted on some hardware, flip the signs in `cpp/SensorFusion.cpp`.
- Frame processors on RN 0.86 rely on VisionCamera v4's use of the bridgeless interop layer (`BridgelessCatalystInstance`) — verified present in RN 0.86, but it is deprecated API and may disappear in a future RN release.
- **Crop mapping assumption**: `cropPhotoToBoundingBox` scales the detected box from analysis-frame space into the captured photo's resolution assuming both share the same aspect ratio/orientation (the usual relationship between a VisionCamera device's frame-processor and photo streams) - not guaranteed identical on every device/OS combination.
- **Native module code in this phase (`ProductCropModule` on both platforms, plus the new JNI export and Kotlin/Obj-C++ wiring) has not been built or run on a device** — there was no toolchain available to compile it in this environment. Review it and do a full on-device build + test pass (both platforms) before relying on auto-crop, auto-capture, or the exposure warning in production.

## Tuning

| Constant | Where | Meaning |
|---|---|---|
| `kGaussianKernelSize` | `cpp/ProductAnalyzer.cpp` | Pre-threshold denoise blur strength |
| `kLocalContrastKernelSize` | `cpp/ProductAnalyzer.cpp` | Local-background blur radius for the adaptive contrast threshold |
| `kContrastThreshold` | `cpp/ProductAnalyzer.cpp` | Minimum local contrast to count as a foreground edge |
| `kMorphCloseKernelSize` | `cpp/ProductAnalyzer.cpp` | How aggressively nearby edges are merged into one silhouette |
| `kMinContourAreaFraction` | `cpp/ProductAnalyzer.cpp` | Noise rejection (fraction of frame area) |
| `kCentroidSmoothingAlpha` | `cpp/ProductAnalyzer.cpp` | Centroid EMA: responsiveness vs. jitter damping |
| `kGyroWeight` | `cpp/SensorFusion.cpp` | Complementary filter: gyro vs accel trust |
| `LEVEL_THRESHOLD_DEG`, `CENTER_THRESHOLD_PX` | `src/components/AlignmentOverlay.tsx` | Alignment tolerances |
| `HAPTIC_COOLDOWN_MS` | `src/components/AlignmentOverlay.tsx` | Minimum time between success haptics |
| `kProcessEveryNthFrame` | `cpp/ProductAnalyzer.cpp` | Frame analysis rate (OpenCV work vs. cached detection) |
| `kShadowClipBin`, `kHighlightClipBin`, `kExposureClipFraction` | `cpp/ProductAnalyzer.cpp` | Exposure warning sensitivity |
| `AUTO_CAPTURE_ALIGNED_MS` | `App.tsx` | How long alignment must hold before auto-capture fires |
| `PHOTO_STEPS` | `App.tsx` | Multi-photo workflow steps and their order |

Template docs from `@react-native-community/cli` are kept in `docs-react-native-template.md`.
