module.exports = {
  preset: '@react-native/jest-preset',
  setupFiles: ['./jest.setup.js'],
  transformIgnorePatterns: [
    'node_modules/(?!(react-native|@react-native|react-native-reanimated|react-native-worklets|react-native-worklets-core|react-native-vision-camera|react-native-haptic-feedback|react-native-safe-area-context)/)',
  ],
};
