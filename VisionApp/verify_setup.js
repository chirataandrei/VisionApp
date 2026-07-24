#!/usr/bin/env node
/**
 * verify_setup.js
 *
 * Strict setup verification for VisionApp's native C++ vision pipeline:
 *   1. React Native + react-native-vision-camera installed (package.json AND node_modules)
 *   2. CMakeLists.txt exists with proper OpenCV C++ configuration
 *   3. iOS Podfile + Android build.gradle configured for C++ JSI/frame-processor compilation
 *
 * Exits 1 if any check fails.
 */

const fs = require('fs');
const path = require('path');

const ROOT = __dirname;
const results = { pass: 0, fail: 0, warn: 0 };

function pass(msg) {
  results.pass++;
  console.log(`  \x1b[32m✔ PASS\x1b[0m  ${msg}`);
}
function fail(msg, hint) {
  results.fail++;
  console.log(`  \x1b[31m✖ FAIL\x1b[0m  ${msg}`);
  if (hint) console.log(`          ↳ ${hint}`);
}
function warn(msg, hint) {
  results.warn++;
  console.log(`  \x1b[33m⚠ WARN\x1b[0m  ${msg}`);
  if (hint) console.log(`          ↳ ${hint}`);
}
function section(title) {
  console.log(`\n\x1b[1m${title}\x1b[0m`);
}

function readIfExists(rel) {
  const abs = path.join(ROOT, rel);
  return fs.existsSync(abs) ? fs.readFileSync(abs, 'utf8') : null;
}

// ---------------------------------------------------------------------------
// 1. package.json dependencies
// ---------------------------------------------------------------------------
section('1. package.json — React Native & VisionCamera');

const pkgRaw = readIfExists('package.json');
let pkg = null;
if (!pkgRaw) {
  fail('package.json not found at project root');
} else {
  try {
    pkg = JSON.parse(pkgRaw);
  } catch (e) {
    fail(`package.json is not valid JSON: ${e.message}`);
  }
}

function checkDependency(name, { dev = false } = {}) {
  if (!pkg) return;
  const declared =
    (pkg.dependencies && pkg.dependencies[name]) ||
    (dev && pkg.devDependencies && pkg.devDependencies[name]);
  if (!declared) {
    fail(`"${name}" is not declared in package.json dependencies`, `Run: npm install ${name}`);
    return;
  }
  // Verify it is actually installed, not just declared.
  const installedPkgJson = path.join(ROOT, 'node_modules', name, 'package.json');
  if (!fs.existsSync(installedPkgJson)) {
    fail(`"${name}" declared (${declared}) but missing from node_modules`, 'Run: npm install');
    return;
  }
  const installedVersion = JSON.parse(fs.readFileSync(installedPkgJson, 'utf8')).version;
  pass(`"${name}" declared (${declared}) and installed (v${installedVersion})`);
}

checkDependency('react');
checkDependency('react-native');
checkDependency('react-native-vision-camera');
// Required by VisionCamera for JS/native frame processors:
checkDependency('react-native-worklets-core');

// ---------------------------------------------------------------------------
// 2. CMakeLists.txt — OpenCV C++ configuration
// ---------------------------------------------------------------------------
section('2. CMakeLists.txt — OpenCV C++ configuration');

const CMAKE_PATH = 'android/app/src/main/cpp/CMakeLists.txt';
const cmake = readIfExists(CMAKE_PATH);

if (cmake === null) {
  fail(`${CMAKE_PATH} not found`, 'Create it and reference it from android/app/build.gradle (externalNativeBuild.cmake.path)');
} else {
  pass(`${CMAKE_PATH} exists`);

  const cmakeChecks = [
    [/cmake_minimum_required\s*\(\s*VERSION/i, 'cmake_minimum_required(VERSION ...) declared'],
    [/project\s*\(/i, 'project(...) declared'],
    [/set\s*\(\s*CMAKE_CXX_STANDARD\s+(17|20|23)\s*\)/i, 'C++ standard set (CMAKE_CXX_STANDARD 17/20/23)'],
    [/find_package\s*\(\s*OpenCV\s+REQUIRED/i, 'find_package(OpenCV REQUIRED ...) present'],
    [/add_library\s*\([\s\S]*?\bSHARED\b/i, 'add_library(... SHARED ...) builds a shared native lib'],
    [/target_link_libraries\s*\([\s\S]*?(OpenCV::|\$\{OpenCV_LIBS\}|opencv_java)/i, 'target_link_libraries links against OpenCV'],
  ];
  for (const [re, label] of cmakeChecks) {
    if (re.test(cmake)) pass(label);
    else fail(`CMakeLists.txt missing: ${label}`);
  }

  // Every source file referenced by add_library must exist on disk.
  const cmakeDir = path.join(ROOT, path.dirname(CMAKE_PATH));
  const addLib = cmake.match(/add_library\s*\(([\s\S]*?)\)/i);
  if (addLib) {
    const sources = addLib[1]
      .split(/\s+/)
      .map(t => t.replace(/^"|"$/g, ''))
      .filter(t => /\.(cpp|cc|cxx|c|mm)$/i.test(t));
    for (const src of sources) {
      const resolved = path.resolve(
        cmakeDir,
        src.replace(/\$\{CMAKE_CURRENT_SOURCE_DIR\}/g, '.').replace(/\$\{SHARED_CPP_DIR\}/g, path.join(cmakeDir, '../../../../../cpp'))
      );
      if (fs.existsSync(resolved)) pass(`source file resolves: ${src}`);
      else fail(`source file listed in add_library does not exist: ${src}`, `Expected at ${resolved}`);
    }
  }
}

// Shared C++ pipeline sources at the app root.
for (const f of ['cpp/ProductAnalyzer.cpp', 'cpp/ProductAnalyzer.h', 'cpp/SensorFusion.cpp', 'cpp/SensorFusion.h']) {
  if (fs.existsSync(path.join(ROOT, f))) pass(`shared C++ source exists: ${f}`);
  else fail(`shared C++ source missing: ${f}`);
}

// ---------------------------------------------------------------------------
// 3a. iOS Podfile — C++ plugin/JSI compilation
// ---------------------------------------------------------------------------
section('3a. iOS Podfile — C++ plugin configuration');

const podfile = readIfExists('ios/Podfile');
if (podfile === null) {
  fail('ios/Podfile not found');
} else {
  pass('ios/Podfile exists');

  if (/use_react_native!/.test(podfile)) pass('use_react_native! present (RN pods + JSI runtime)');
  else fail('Podfile missing use_react_native! — React Native pods (and JSI) will not be installed');

  if (/use_native_modules!/.test(podfile)) pass('use_native_modules! present (autolinks VisionCamera pod)');
  else fail('Podfile missing use_native_modules! — react-native-vision-camera will not be autolinked');

  const podRef = podfile.match(/pod\s+['"]ProductAnalyzerPlugin['"]\s*,\s*:path\s*=>\s*['"]([^'"]+)['"]/);
  if (podRef) {
    pass(`local C++ plugin pod referenced: ProductAnalyzerPlugin (:path => '${podRef[1]}')`);
  } else {
    fail("Podfile does not reference the local C++ plugin pod", "Add: pod 'ProductAnalyzerPlugin', :path => '..'");
  }
}

// The podspec is where the actual C++ compilation settings live on iOS.
const podspec = readIfExists('ProductAnalyzerPlugin.podspec');
if (podspec === null) {
  fail('ProductAnalyzerPlugin.podspec not found at project root');
} else {
  pass('ProductAnalyzerPlugin.podspec exists');

  const podspecChecks = [
    [/CLANG_CXX_LANGUAGE_STANDARD["']?\s*=>?\s*["']c\+\+(17|20|23)/i, 'podspec sets CLANG_CXX_LANGUAGE_STANDARD (c++17/20/23)'],
    [/source_files\s*=[\s\S]*?cpp\/\*\*/, 'podspec compiles shared cpp/ sources'],
    [/s\.dependency\s+["']VisionCamera["']/, 'podspec depends on VisionCamera (frame processor plugin API)'],
    [/s\.dependency\s+["']OpenCV["']/, 'podspec depends on OpenCV'],
  ];
  for (const [re, label] of podspecChecks) {
    if (re.test(podspec)) pass(label);
    else fail(`podspec missing: ${label}`);
  }
}

// Was `pod install` actually run after the plugin was added?
const podLock = readIfExists('ios/Podfile.lock');
if (podLock === null) {
  warn('ios/Podfile.lock not found — pods have never been installed', 'Run: cd ios && pod install');
} else {
  for (const podName of ['ProductAnalyzerPlugin', 'VisionCamera', 'OpenCV']) {
    if (new RegExp(`^\\s+- ${podName} \\(`, 'm').test(podLock)) pass(`Podfile.lock includes ${podName}`);
    else fail(`Podfile.lock does not include ${podName}`, 'Run: cd ios && pod install');
  }
}

// ---------------------------------------------------------------------------
// 3b. Android build.gradle — CMake / C++ compilation
// ---------------------------------------------------------------------------
section('3b. Android build.gradle — CMake / C++ configuration');

const appGradle = readIfExists('android/app/build.gradle');
if (appGradle === null) {
  fail('android/app/build.gradle not found');
} else {
  pass('android/app/build.gradle exists');

  const gradleChecks = [
    [/externalNativeBuild\s*\{[\s\S]*?cmake\s*\{[\s\S]*?path\s+["']src\/main\/cpp\/CMakeLists\.txt["']/, 'externalNativeBuild.cmake.path points to src/main/cpp/CMakeLists.txt'],
    [/cppFlags\s+["'].*-std=c\+\+(17|20|23)/, 'cppFlags set a modern C++ standard (-std=c++17/20/23)'],
    [/arguments\s+["'].*ANDROID_STL=c\+\+_shared/, 'ANDROID_STL=c++_shared (required to share libc++ with React Native)'],
    [/prefab\s+true/, 'buildFeatures.prefab enabled (exposes OpenCV AAR to CMake find_package)'],
    [/pickFirst\s+["'].*libc\+\+_shared\.so["']/, 'packagingOptions pickFirst for libc++_shared.so (RN & OpenCV both ship it)'],
    [/["']org\.opencv:opencv:[\d.]+["']/, 'OpenCV Android AAR declared in dependencies'],
    [/ndkVersion\s/, 'ndkVersion configured'],
  ];
  for (const [re, label] of gradleChecks) {
    if (re.test(appGradle)) pass(label);
    else fail(`android/app/build.gradle missing: ${label}`);
  }
}

const rootGradle = readIfExists('android/build.gradle');
if (rootGradle === null) {
  fail('android/build.gradle not found');
} else {
  pass('android/build.gradle exists');
  if (/ndkVersion\s*=/.test(rootGradle)) pass('root build.gradle defines ndkVersion (needed for CMake/NDK builds)');
  else fail('root build.gradle does not define ndkVersion');
  if (/mavenCentral\(\)/.test(rootGradle)) pass('mavenCentral() repository available (hosts org.opencv:opencv AAR)');
  else fail('mavenCentral() repository missing — org.opencv:opencv cannot be resolved');
}

// ---------------------------------------------------------------------------
// Summary
// ---------------------------------------------------------------------------
console.log('\n' + '─'.repeat(60));
console.log(
  `\x1b[1mSummary:\x1b[0m ${results.pass} passed, ${results.fail} failed, ${results.warn} warnings`
);
if (results.fail > 0) {
  console.log('\x1b[31mSetup verification FAILED — fix the items above.\x1b[0m');
  process.exit(1);
} else if (results.warn > 0) {
  console.log('\x1b[33mSetup verification passed with warnings.\x1b[0m');
} else {
  console.log('\x1b[32mSetup verification passed — all checks green.\x1b[0m');
}
