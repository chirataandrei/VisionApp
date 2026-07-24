/**
 * @format
 */

import { AppRegistry, LogBox } from 'react-native';
import App from './App';
import { name as appName } from './app.json';

// Suppress RN's built-in yellow/red warning overlay ("Open debugger to view
// warnings" and friends) - it's a dev-only artifact that has no business
// showing up over the camera preview, on-device or otherwise. Any debug
// UI this app actually wants belongs behind the explicit ENABLE_DEBUG_UI
// flag in App.tsx instead, not RN's own uncontrollable LogBox.
LogBox.ignoreAllLogs();

AppRegistry.registerComponent(appName, () => App);
