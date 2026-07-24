import type { Orientation } from 'react-native-vision-camera';

import type { Box, Point } from './geometry';
import { boundingBoxOfPoints } from './geometry';

/**
 * Maps a point in raw frame pixel space to upright frame-pixel space, given
 * the Frame's reported `orientation` (see VisionCamera's `Frame.orientation`
 * docs: the rotation the frame's content has relative to the desired
 * up-right output), using a consistent clockwise rotation-by-degrees
 * convention: portrait: 0°, landscape-left: 90°, portrait-upside-down: 180°,
 * landscape-right: 270° (matching `Orientation`'s own documented degree
 * values), applied as a clockwise rotation of the raw buffer to make it
 * upright.
 */
export function mapPointToUpright(
  frameX: number,
  frameY: number,
  frameWidth: number,
  frameHeight: number,
  orientation: Orientation,
): Point {
  switch (orientation) {
    case 'portrait':
      return { x: frameX, y: frameY };
    case 'landscape-left':
      return { x: frameHeight - frameY, y: frameX };
    case 'portrait-upside-down':
      return { x: frameWidth - frameX, y: frameHeight - frameY };
    case 'landscape-right':
      return { x: frameY, y: frameWidth - frameX };
  }
}

/** Frame dimensions after the same rotation `mapPointToUpright` applies. */
export function uprightDimensions(
  frameWidth: number,
  frameHeight: number,
  orientation: Orientation,
): { width: number; height: number } {
  switch (orientation) {
    case 'portrait':
    case 'portrait-upside-down':
      return { width: frameWidth, height: frameHeight };
    case 'landscape-left':
    case 'landscape-right':
      return { width: frameHeight, height: frameWidth };
  }
}

/** Maps a point in raw frame pixel space to upright screen space. */
export function mapFrameToScreen(
  frameX: number,
  frameY: number,
  frameWidth: number,
  frameHeight: number,
  orientation: Orientation,
  screenWidth: number,
  screenHeight: number,
): Point {
  const upright = mapPointToUpright(frameX, frameY, frameWidth, frameHeight, orientation);
  const { width: uprightWidth, height: uprightHeight } = uprightDimensions(frameWidth, frameHeight, orientation);
  return {
    x: (upright.x / uprightWidth) * screenWidth,
    y: (upright.y / uprightHeight) * screenHeight,
  };
}

/**
 * Maps a bounding box in raw frame pixel space to upright frame-pixel space
 * (not further scaled to screen size), for cropping a captured photo to the
 * detected product. Photos are auto-rotated to upright by OpenCV's `imread`
 * (EXIF-aware) before cropping, so the box must be in that same upright
 * space - not the raw, possibly-rotated analysis frame space - before being
 * handed to the native crop module.
 */
export function mapBoxToUpright(
  box: Box,
  frameWidth: number,
  frameHeight: number,
  orientation: Orientation,
): { box: Box; uprightWidth: number; uprightHeight: number } {
  const corners = [
    mapPointToUpright(box.x, box.y, frameWidth, frameHeight, orientation),
    mapPointToUpright(box.x + box.width, box.y, frameWidth, frameHeight, orientation),
    mapPointToUpright(box.x, box.y + box.height, frameWidth, frameHeight, orientation),
    mapPointToUpright(box.x + box.width, box.y + box.height, frameWidth, frameHeight, orientation),
  ];
  const { width: uprightWidth, height: uprightHeight } = uprightDimensions(frameWidth, frameHeight, orientation);
  return { box: boundingBoxOfPoints(corners), uprightWidth, uprightHeight };
}
