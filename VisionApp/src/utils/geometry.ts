export interface Point {
  x: number;
  y: number;
}

export interface Box {
  x: number;
  y: number;
  width: number;
  height: number;
}

/**
 * Euclidean distance for a (dx, dy) offset. Carries the `'worklet'`
 * directive so it can be called from Reanimated UI-thread derived
 * values/animated styles as well as plain JS code.
 */
export function distance(dx: number, dy: number): number {
  'worklet';
  return Math.sqrt(dx * dx + dy * dy);
}

/** Smallest axis-aligned box containing all of `points`. */
export function boundingBoxOfPoints(points: readonly Point[]): Box {
  const xs = points.map(point => point.x);
  const ys = points.map(point => point.y);
  const minX = Math.min(...xs);
  const minY = Math.min(...ys);
  const maxX = Math.max(...xs);
  const maxY = Math.max(...ys);
  return { x: minX, y: minY, width: maxX - minX, height: maxY - minY };
}
