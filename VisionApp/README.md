# VisionApp

React Native (0.86, TypeScript) camera app with a **custom C++ frame processor plugin**: every camera frame is handed to native C++ **by reference via JSI** (zero copies, nothing crosses the legacy bridge), analyzed with **OpenCV**, fused with **accelerometer/gyroscope** data, and the result drives a **Reanimated** alignment overlay on the UI thread.

## What it does

- Full-screen [VisionCamera](https://react-native-vision-camera.com) view (`pixelFormat="yuv"`).
- A frame processor worklet calls the native plugin `analyzeProduct` on every camera frame (no `fps` throttle set on `<Camera>`); only the OpenCV work inside the plugin is throttled, to every 3rd call. The worklet and both native bridge callbacks are fully synchronous end to end (no async/await, no dispatch queues, no coroutines) — VisionCamera never delivers frame N+1 until the call for frame N has returned, so a slow frame is naturally dropped rather than queued, not merely hoped for. See [Frame-buffer management](#frame-buffer-management) for the native-side defense-in-depth on top of that.
- **Vision pipeline** (`cpp/ProductAnalyzer.cpp`), tuned for 60 FPS on-device:
  1. Downscale to a fixed 240 px width (`INTER_NEAREST` — cost stays roughly constant regardless of camera resolution, at the cost of some aliasing the adaptive threshold below already tolerates) → grayscale.
  2. **Motion-blur bypass**: before touching lighting/saturation/contours at all, the variance of the Laplacian of the downscaled frame is checked (`cv::Laplacian` + `cv::meanStdDev`) — a frame blurred by fast phone movement or rotation has almost no high-frequency detail left, so this collapses toward zero. Below `kMinBlurVariance`, the entire detection pipeline is skipped and the call returns immediately with status `NOT_CENTERED`, message "Mișcare prea rapidă" — see [Motion-blur bypass](#motion-blur-bypass) below.
  3. Adaptive local-contrast threshold (polarity-agnostic; a pixel counts as foreground only if it stands out from its own local neighborhood mean, so smooth cast shadows fall below threshold while true edges don't), **OR'd with a color-saturation mask** when chroma data is available — see [Color saturation masking](#color-saturation-masking) below — so a solid-colored garment's interior (little internal edge signal on its own) still contributes to a complete silhouette.
  4. Morphological **open** (erases small isolated noise blobs — window reflections, sensor speckle — before they can bridge into the silhouette) then **close** (merges a patterned garment's internal edges into one silhouette).
  5. **Smart contour scoring**, not "largest contour wins". Every candidate contour clearing a minimum-area floor is first run through a **hard filter**: simplify it with `approxPolyDP`, and if the result is a convex 4–6-vertex polygon, skip it outright — a genuine straight-edged rectangular object (a TV, a picture frame, a door, a shelf) is exactly that, while a garment silhouette even when it happens to approximate to a similar vertex count is virtually never convex (necklines, sleeves, and hems carve concave notches into the outline). Whatever survives is scored `Area × AspectAreaRatio × (1 − EdgeLinearity)` and the highest-scoring one wins. `AspectAreaRatio` is the contour's *extent* (its own area ÷ its bounding box's area). `EdgeLinearity` softly penalizes a few-cornered-but-non-convex shape (an L-shape, a concave silhouette) that the hard filter didn't catch — ~90%, compounded to ~97% if it's *also* low-saturation.
  6. EMA-smoothed **centroid** (damps frame-to-frame jitter; the ROI tracker itself still follows the raw, unsmoothed detection so it can't lag a fast-moving object).
  7. A strict-priority **status/message hierarchy** (see below) reduces to `AnalysisStatus` + a Romanian instruction string + normalized `{dx, dy}` position and tilt vectors.
  8. A **debounce** (`stabilizeStatus`): the reported status/message only changes once the newly-classified value has been consistent for `kStatusConfirmFrames` (3) consecutive calls — tracked one level finer than the enum, on `messageCode` (e.g. "nothing detected" vs. "off-center" are both `NOT_CENTERED` but different messages) — so a single stray misclassified frame can't flicker the UI or restart the auto-capture countdown. This also covers the motion-blur bypass: a single blurry frame doesn't instantly flip the displayed message.
  - Performance: only every 3rd frame runs this pipeline (the rest answer from cache with fresh pitch/roll/tilt), the search is restricted to an ROI around the previous detection (full-frame fallback when lost), and every intermediate `cv::Mat` (blur/threshold/morphology/saturation/Laplacian scratch space) is a member of the persistent pipeline state, reused frame to frame instead of reallocated. Pipeline execution time is measured in C++ and always populated on `latencyMs`, regardless of build type.
- **Sensor fusion** (`cpp/SensorFusion.cpp`): a complementary filter over low-pass-filtered accelerometer + gyroscope samples produces **pitch/roll**. The accelerometer's gravity estimate is only trusted while the device is physically stable (`‖accel‖ ≈ 1.0g ± 0.1`, both platforms normalized to g's before calling in); outside that band the filter falls back to pure gyro integration, so a hand actively moving the phone can't corrupt the orientation estimate. `Attitude.isFlatMode` (flat-lay vs. on-hanger) is classified from pitch with a hysteresis band (enters hanger past 50°, returns to flat below 40°) so holding the phone near the ~45° boundary can't flap the UI between modes.
- **Status hierarchy** (`AnalysisStatus`, checked in this strict order — only the single most impactful problem is ever surfaced; the motion-blur bypass above preempts all of it):
  1. Nothing significant in frame (contour under 12% of frame area) → `NOT_CENTERED`, "Așează haina în cadru"
  2. Bounding box touches **two opposite** frame edges (left+right, or top+bottom — not merely any one edge, which on physical devices is routinely just sensor/lens noise or background clutter) at a generous 10%-of-frame margin, **and** covers over 60% of the frame area → `CUT_OFF_MARGINS`, "Îndepărtează camera"
  3. Centroid off-center by more than 20% on either axis independently → `NOT_CENTERED`, "Centrează haina"
  4. Phone tilted more than 7° (only checked once framing + centering already pass) → `PHONE_TILTED`, "Ține telefonul mai drept"
  5. All of the above pass → `OK`, "Perfect!"
  - `isReadyForCapture` is `true` only when status is `OK` **and** lighting is good — the one signal deliberately kept outside this hierarchy, since it's an independent image-derived property.
- The plugin returns a plain object (JSI conversion, no serialization, no bridge) to JS: `{ found, status, message, isReadyForCapture, lightingState, orientationMode, normalizedDx, normalizedDy, tilt: {dx, dy}, frameWidth, frameHeight, boundingBox: {x, y, width, height}, latencyMs, processed, debug? }`. `lightingState` (`'TOO_DARK' | 'OVEREXPOSED' | 'GOOD'`) comes from a brightness-histogram check in C++ (shadow/highlight clipping fractions). The `debug` field (raw pitch/roll/centroid/contour area) only exists in debug builds — see [Debug vs. release payload](#debug-vs-release-payload).
- **Reanimated overlay** (`src/components/AlignmentOverlay.tsx`), composing:
  - `AlignmentGuide` — a single, unified rounded dashed reticle marking a fixed target zone. Subtle white/gray by default; turns vibrant green with a brief one-shot scale pulse the instant `AnalysisStatus` is `OK` (a narrower, purely-alignment condition than `isReadyForCapture`, which also requires good lighting).
  - When `isReadyForCapture` becomes true, a success **haptic** fires (rate-limited by `HAPTIC_COOLDOWN_MS`) and `CaptureCountdownRing` (a progress ring around the shutter button) starts a lap that completes exactly when auto-capture fires; the ring resets instantly (not pauses) if alignment breaks mid-hold. A second, distinct haptic pulse fires at the exact moment auto-capture triggers.
  - `FeedbackBanner` — a single natural-language instruction near the shutter button, sourced directly from the native `message` (with a lighting-specific override checked first, since lighting sits outside the native priority hierarchy).
  - `WorkflowStepper` — a frosted-glass-style pill at the top showing the current step ("Poza de Spate · Pasul 2/4") and a dot-row progress indicator.
  - No blur library is installed in this project, so the "glass" look on `WorkflowStepper`/`FeedbackBanner` is approximated with a translucent fill, a light border highlight, and a soft shadow rather than a true backdrop blur.
- **Auto-capture + manual shutter** (`App.tsx`): a `ShutterButton` for manual capture, plus automatic `camera.takePhoto()` once `isReadyForCapture` holds for `AUTO_CAPTURE_ALIGNED_MS` (1000 ms).
- **Vinted multi-photo workflow** (`src/components/WorkflowStepper.tsx` + `src/hooks/usePhotoWorkflow.ts`): steps through Poza de Fata → Poza de Spate → Eticheta Marime/Brand → Defecte/Detalii, advancing on each successful capture.
- **Auto-crop** (`src/native/ProductCropModule.ts` → native `ProductCropModule`): after capture, if a product was detected, crops the photo to its bounding box via `cpp/ProductAnalyzer.cpp`'s `cropPhotoToBoundingBox` (OpenCV `imread`/`imwrite` — no extra JS image library needed since OpenCV is already linked). The box is converted from the frame processor's raw sensor-space coordinates into the photo's upright pixel space (`src/utils/cameraTransforms.ts`'s `mapBoxToUpright`) before cropping.

## Motion-blur bypass

Fast phone movement/rotation was freezing the app: OpenCV was running its full contour-detection pipeline on frames that were too motion-blurred for the result to mean anything, burning CPU budget on garbage input. `analyzeFrame` now checks the Laplacian variance of the downscaled grayscale frame immediately after the downscale step, before any of the expensive work (lighting histogram, saturation mask, morphology, contour scoring). Below `kMinBlurVariance` (60.0), all of that is skipped and the call returns in well under 1ms with a dedicated status: `NOT_CENTERED` / "Mișcare prea rapidă" (`messageCode` 5). This still goes through the normal debounce, so a single blurry frame doesn't instantly flip the displayed message — only a sustained blur does, and it clears the same way once motion stops.

## Frame-buffer management

Camera frame delivery is inherently synchronous and serial in this architecture (see [What it does](#what-it-does)), so `analyzeFrame` should never actually be re-entered while a previous call is still running. As defense-in-depth against that assumption ever being violated (a future VisionCamera version, a device-switch race, a bug), `analyzeFrame` opens with a non-blocking `ProcessingGuard` — a `std::atomic<bool>` compare-exchange, deliberately not a mutex, since a mutex would make a contended call **block and wait**, which is exactly the "camera thread stalls while frames back up" failure mode this exists to prevent. If acquisition fails, the frame is dropped immediately: no queueing, no waiting, a fresh empty `AnalysisResult{}` is returned without touching the (actively-being-mutated-by-the-other-call) pipeline state at all.

## Color saturation masking

Televisions, walls, and wood floors are usually neutral-toned even when they clear the edge-contrast threshold (a bezel, a reflection, plank seams). To help tell them apart from fabric, the pipeline builds a continuous 0–255 "how colorful is this pixel" map and ORs a thresholded version of it into the edge mask, plus feeds it into contour scoring's linear-shape penalty — compounding the penalty specifically when a candidate is *both* straight-edged *and* neutral-toned, without ever touching the score of an organic-shaped contour (a plain white/black/gray garment is never flagged as "linear" in the first place, so it's unaffected either way).

Where the color data comes from depends on the camera pixel format already in use (`pixelFormat="yuv"`, i.e. mostly `GRAY8`/Y-plane frames):
- **iOS** (biplanar CbCr): the plugin also reads plane 1 and passes it in as two `ChromaPlane`s (U at byte offset 0, V at offset 1, both `pixelStride 2`) — zero extra copying, since iOS's native layout already matches the shared contract.
- **Android** (`YUV_420_888`): planes 1/2 (U/V) are passed through with their own `rowStride`/`pixelStride`, since Android doesn't guarantee interleaving the way iOS does (it varies by device/vendor) — the shared C++ code deinterleaves and downsamples them itself.
- Either way, the chroma data is sampled **directly at the already-downscaled 240px working resolution** (nearest-neighbor, never materialized at native chroma resolution) to keep cost proportional to the rest of the pipeline rather than the camera's native resolution.
- If no chroma is available for a given call (or the layout is already `BGRA8888`/`RGBA8888`, where the real HSV `S` channel is used instead), the saturation signal is simply empty and detection degrades gracefully to edge-only — the pre-existing behavior.

## Debug vs. release payload

Raw numeric telemetry (exact pixel centroid, contour area in px², raw pitch/roll in degrees) never reaches a release build's JS layer — it's compiled out of `AnalysisResult` entirely behind `#ifdef DEBUG` in `cpp/ProductAnalyzer.h`, and the bridge plugins mirror that gate (`#ifdef DEBUG` in `ProductAnalyzerPlugin.mm`, `BuildConfig.DEBUG` in `ProductAnalyzerPlugin.kt`) by nesting it under an optional `debug` key in the JS payload. Android's CMake build defines `DEBUG` for the `Debug` config explicitly (`android/app/src/main/cpp/CMakeLists.txt`), matching Xcode's Debug configuration on iOS. Production UI is built entirely from the structured `status`/`message`/`isReadyForCapture`/normalized-vector fields — never from this raw telemetry.

On the JS side, two more things keep debug/dev artifacts off the on-screen UI:
- `index.js` calls `LogBox.ignoreAllLogs()` at the entry point, before `App.tsx` ever renders — React Native's own yellow/red warning overlay (the "Open debugger to view warnings" banner and friends) never appears over the camera preview, regardless of build type.
- `App.tsx` defines a single `ENABLE_DEBUG_UI` constant (`false` by default, independent of `__DEV__`) that gates the only on-screen debug element in the app: a small top-right `status · latencyMs` readout. Any future debug UI should be gated the same way rather than shown unconditionally.

## Project structure

`App.tsx` is a thin composition layer; the actual logic lives in:

- `src/hooks/useProductAlignment.ts` — owns the frame processor and the Reanimated shared values it drives (`found`, `isOk`, `perfect`), plus the latest JS-visible analysis (for the feedback banner and the debug readout) and, via a ref, the freshest detection for capture-time cropping.
- `src/hooks/useAutoCapture.ts` — starts/cancels the alignment-hold timer that triggers an automatic capture, firing a capture-moment haptic when it completes.
- `src/hooks/usePhotoWorkflow.ts` — drives the Vinted multi-photo steps: takes the photo, crops it if a detection exists, stores it under the current step, advances.
- `src/utils/geometry.ts` — generic point/box primitives (`Point`, `Box`, `boundingBoxOfPoints`).
- `src/utils/cameraTransforms.ts` — orientation-aware frame ↔ photo coordinate mapping (`mapPointToUpright`, `uprightDimensions`, `mapBoxToUpright`), used by the auto-crop path.
- `src/components/` — `AlignmentOverlay` (composes `AlignmentGuide`, owns the aligned-transition haptic), `AlignmentGuide`, `FeedbackBanner`, `CaptureCountdownRing`, `WorkflowStepper`, `ShutterButton` — presentational, driven by shared values/props from the hooks above.
- `src/frameProcessors/productAnalyzer.ts` / `src/native/ProductCropModule.ts` — the JS-side native bridges described above.

## Data flow

```
Camera HW frame ──(JSI HostObject, zero-copy)──▶ Frame Processor Plugin (native)
                                                  │  iOS: ProductAnalyzerPlugin.mm (Obj-C++, CVPixelBuffer plane 0 [+ plane 1 chroma])
                                                  │  Android: ProductAnalyzerPlugin.kt → JNI direct ByteBuffers (Y [+ U/V])
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

Both are classic (pre-TurboModule) native modules, which continue to work under React Native's New Architecture (enabled here, `newArchEnabled=true`) via its backward-compatibility interop layer — the same mechanism VisionCamera itself relies on for its frame processor plugin registration.

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

Requires the Android NDK + CMake; if missing, Android Gradle Plugin auto-installs them on first build (SDK licenses are already accepted on this machine). The OpenCV native libs come from `org.opencv:opencv:4.10.0` on Maven Central. `android/app/build.gradle` restricts `ndk.abiFilters` to `arm64-v8a` and `x86_64` (faster local debug builds) — re-add `armeabi-v7a`/`x86` if you need to test on a 32-bit device or an x86 (non-x86_64) emulator image.

## Known caveats

- **Build/test status, accurately**: Android has been built, installed, and run on an emulator (confirmed booting into the camera UI with no crash). The iOS Xcode project has real signing configured (`DEVELOPMENT_TEAM` + a real `PRODUCT_BUNDLE_IDENTIFIER` in `project.pbxproj`, not the RN template placeholder), consistent with a build to a physical iPhone having been set up outside this repo's automated tooling. Physical-device testing (on one platform or both — not something recorded here) is what surfaced the motion-blur freeze and the `CUT_OFF_MARGINS` false-positive bugs this README's [Motion-blur bypass](#motion-blur-bypass) section and the relaxed status-hierarchy margins address. Those specific fixes, and everything under [Frame-buffer management](#frame-buffer-management), have **not** been re-verified on physical hardware since being written — do a full on-device re-test pass on both platforms before shipping.
- **Chroma plane assumptions remain the highest-risk untested piece**: iOS's biplanar CbCr interleaving and Android's `YUV_420_888` per-plane `rowStride`/`pixelStride` handling (see [Color saturation masking](#color-saturation-masking)) are exactly the kind of byte-layout code that looks right on paper but needs a real device/format to confirm — verify against actual camera output on both platforms, ideally on more than one Android vendor given how much `pixelStride` varies device to device.
- **iOS simulator**: the `OpenCV` CocoaPod (4.3.x) ships no arm64-simulator slice — build for a physical device on Apple Silicon Macs.
- **Gyro axis signs** in the complementary filter follow Android sensor conventions (iOS values are adapted in the plugin). If pitch/roll feel inverted on some hardware, flip the signs in `cpp/SensorFusion.cpp`.
- Frame processors on RN 0.86 rely on VisionCamera v4's use of the bridgeless interop layer (`BridgelessCatalystInstance`) — verified present in RN 0.86, but it is deprecated API and may disappear in a future RN release.
- **Crop mapping assumption**: `cropPhotoToBoundingBox` scales the detected box from analysis-frame space into the captured photo's resolution assuming both share the same aspect ratio/orientation (the usual relationship between a VisionCamera device's frame-processor and photo streams) — not guaranteed identical on every device/OS combination.
- **16 KB page-size compatibility**: on the Android emulator, launching the app shows a system "Android App Compatibility" dialog listing several native libraries (including `libproductanalyzer.so` and `libopencv_java4.so`) as not 16 KB-aligned. The app still runs fine in Android's compatibility fallback mode, and the existing `-Wl,-z,max-page-size=16384` linker flag in `android/app/src/main/cpp/CMakeLists.txt` doesn't fully resolve it for this library - not urgent today, but newer Android versions are moving toward requiring 16 KB alignment outright.

## Tuning

| Constant | Where | Meaning |
|---|---|---|
| `kGaussianKernelSize` | `cpp/ProductAnalyzer.cpp` | Pre-threshold denoise blur strength |
| `kLocalContrastKernelSize` | `cpp/ProductAnalyzer.cpp` | Local-background blur radius for the adaptive contrast threshold |
| `kContrastThreshold` | `cpp/ProductAnalyzer.cpp` | Minimum local contrast to count as a foreground edge |
| `kMorphOpenKernelSize` | `cpp/ProductAnalyzer.cpp` | Noise-blob erosion strength, applied before the close step |
| `kMorphCloseKernelSize` | `cpp/ProductAnalyzer.cpp` | How aggressively nearby edges are merged into one silhouette |
| `kCentroidSmoothingAlpha` | `cpp/ProductAnalyzer.cpp` | Centroid EMA: responsiveness vs. jitter damping |
| `kMinBlurVariance` | `cpp/ProductAnalyzer.cpp` | Laplacian-variance floor below which a frame is treated as too motion-blurred to process |
| `kApproxPolyEpsilonFraction` | `cpp/ProductAnalyzer.cpp` | `approxPolyDP` simplification tolerance for the straight-edge (TV/furniture) check |
| `kLinearShapeMinVertices` / `kLinearShapeMaxVertices` | `cpp/ProductAnalyzer.cpp` | Vertex-count range that reads as "straight-edged rectangular object" (hard-filtered outright if also convex) |
| `kLinearShapeEdgeLinearity` | `cpp/ProductAnalyzer.cpp` | Soft score penalty for a few-cornered-but-non-convex shape |
| `kSaturationThreshold` | `cpp/ProductAnalyzer.cpp` | Saturation value (0-255) above which a pixel counts as "colorful" |
| `kLinearShapeSaturationPenaltyThreshold` / `kLinearShapeLowSaturationEdgeLinearity` | `cpp/ProductAnalyzer.cpp` | Compounded penalty for a straight-edged **and** neutral-toned contour |
| `kMinPresenceAreaFraction` | `cpp/ProductAnalyzer.cpp` | Minimum contour/bounding-box area (fraction of frame) to count as "significant" |
| `kFramingEdgeMarginFraction` | `cpp/ProductAnalyzer.cpp` | How close to a frame edge counts as "touching" it (10%) |
| `kCutOffMinAreaFraction` | `cpp/ProductAnalyzer.cpp` | Minimum area for a box touching two opposite edges to actually read as cut off (60%) |
| `kCenterThresholdNorm` | `cpp/ProductAnalyzer.cpp` | Per-axis centering tolerance (normalized) |
| `kLevelThresholdDeg` | `cpp/ProductAnalyzer.cpp` | Phone-tilt tolerance (degrees) |
| `kStatusConfirmFrames` | `cpp/ProductAnalyzer.cpp` | Consecutive frames required before the reported status/message changes |
| `kProcessEveryNthFrame` | `cpp/ProductAnalyzer.cpp` | Frame analysis rate (OpenCV work vs. cached detection) |
| `kTargetProcessingWidth` | `cpp/ProductAnalyzer.cpp` | Downscaled working resolution |
| `kShadowClipBin`, `kHighlightClipBin`, `kExposureClipFraction` | `cpp/ProductAnalyzer.cpp` | Lighting-warning sensitivity |
| `kGyroWeight` | `cpp/SensorFusion.cpp` | Complementary filter: gyro vs. accel trust, while the device is stable |
| `kAccelLowPassAlpha` | `cpp/SensorFusion.cpp` | Accelerometer sample smoothing before it's used at all |
| `kStableMagnitudeToleranceG` | `cpp/SensorFusion.cpp` | How far from 1g the device can read and still be considered "stable" (accel trusted) |
| `kHangerPitchEnterDeg` / `kFlatPitchEnterDeg` | `cpp/SensorFusion.cpp` | Hysteresis band for flat-lay vs. on-hanger mode switching |
| `HAPTIC_COOLDOWN_MS` | `src/components/AlignmentOverlay.tsx` | Minimum time between success haptics |
| `AUTO_CAPTURE_ALIGNED_MS` | `App.tsx` | How long `isReadyForCapture` must hold before auto-capture fires |
| `ENABLE_DEBUG_UI` | `App.tsx` | Shows/hides the on-screen `status · latencyMs` debug readout |

| `PHOTO_STEPS` | `App.tsx` | Multi-photo workflow steps and their order |

Template docs from `@react-native-community/cli` are kept in `docs-react-native-template.md`.
