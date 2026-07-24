cmake_minimum_required(VERSION 3.22.1)

# Setting externalNativeBuild.cmake.path in build.gradle replaces the CMake
# build the React Native Gradle Plugin would otherwise generate — so this file
# must also build libappmodules.so (RN's autolinked TurboModule registry) or
# every TurboModule lookup fails at runtime ("PlatformConstants could not be
# found"). ReactNative-application.cmake below provides the appmodules target;
# REACT_ANDROID_DIR is injected by the React Native Gradle Plugin.
project(appmodules)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(${REACT_ANDROID_DIR}/cmake-utils/ReactNative-application.cmake)

# --- VisionCamera frame processor plugin (OpenCV + sensor fusion) ---

# Shared (cross-platform) C++ pipeline lives at the app root in cpp/.
set(SHARED_CPP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../cpp")

# Provided by the org.opencv:opencv AAR via Gradle prefab support.
find_package(OpenCV REQUIRED CONFIG)

# The JNI source lives in plugin/ (not next to this file) because
# ReactNative-application.cmake globs sibling *.cpp into appmodules.
add_library(
  productanalyzer
  SHARED
  plugin/ProductAnalyzerJNI.cpp
  "${SHARED_CPP_DIR}/ProductAnalyzer.cpp"
  "${SHARED_CPP_DIR}/SensorFusion.cpp"
)

target_include_directories(productanalyzer PRIVATE "${SHARED_CPP_DIR}")

# CMake's Debug config doesn't define DEBUG by default (only Release defines
# NDEBUG) - define it explicitly so ProductAnalyzer.cpp's #ifdef DEBUG guards
# behave the same as Xcode's Debug configuration on iOS.
target_compile_definitions(productanalyzer PRIVATE $<$<CONFIG:Debug>:DEBUG>)

# 16 KB page-size devices (Android 15+) reject 4 KB-aligned ELF segments.
target_link_options(productanalyzer PRIVATE "-Wl,-z,max-page-size=16384")

find_library(log-lib log)

target_link_libraries(
  productanalyzer
  OpenCV::opencv_java4
  ${log-lib}
)
