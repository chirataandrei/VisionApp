jest.mock('react-native-worklets', () => require('react-native-worklets/lib/module/mock'));
jest.mock('react-native-reanimated', () => require('react-native-reanimated/mock'));

jest.mock('react-native-vision-camera', () => ({
  Camera: () => null,
  useCameraDevice: () => undefined,
  useCameraPermission: () => ({ hasPermission: false, requestPermission: jest.fn() }),
  useFrameProcessor: fn => fn,
  runAtTargetFps: (_fps, fn) => fn(),
  VisionCameraProxy: { initFrameProcessorPlugin: () => null },
}));

jest.mock('react-native-worklets-core', () => ({
  useRunOnJS: fn => fn,
  Worklets: { createRunOnJS: fn => fn },
}));

jest.mock('react-native-haptic-feedback', () => ({
  __esModule: true,
  default: { trigger: jest.fn() },
  trigger: jest.fn(),
}));
