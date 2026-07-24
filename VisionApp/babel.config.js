module.exports = {
  presets: ['module:@react-native/babel-preset'],
  plugins: [
    // VisionCamera frame processor worklets
    ['react-native-worklets-core/plugin'],
    // Reanimated 4 worklets — must be listed last
    ['react-native-worklets/plugin'],
  ],
};
