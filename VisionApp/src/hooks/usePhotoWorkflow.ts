import type { RefObject } from 'react';
import { useCallback, useState } from 'react';
import type { Camera } from 'react-native-vision-camera';

import type { DetectionSnapshot } from './useProductAlignment';
import { cropToDetection } from '../native/ProductCropModule';
import { mapBoxToUpright } from '../utils/cameraTransforms';

/** Sibling path for the cropped output, next to the original capture. */
function croppedPathFor(sourcePath: string): string {
  const dotIndex = sourcePath.lastIndexOf('.');
  return dotIndex === -1
    ? `${sourcePath}-cropped`
    : `${sourcePath.slice(0, dotIndex)}-cropped${sourcePath.slice(dotIndex)}`;
}

/**
 * Drives the Vinted multi-photo listing workflow: captures a photo (manual
 * or auto), crops it to the last detection's bounding box when one exists,
 * stores it under the current step's label, and advances to the next step.
 */
export function usePhotoWorkflow(
  camera: RefObject<Camera | null>,
  steps: readonly string[],
  lastDetectionRef: RefObject<DetectionSnapshot | null>,
) {
  const [isCapturing, setIsCapturing] = useState(false);
  const [stepIndex, setStepIndex] = useState(0);
  const [capturedPhotos, setCapturedPhotos] = useState<Record<string, string>>({});
  const isDone = stepIndex >= steps.length;

  const capturePhoto = useCallback(async () => {
    if (camera.current == null || isCapturing || isDone) {
      return;
    }
    setIsCapturing(true);
    try {
      const photo = await camera.current.takePhoto();
      let finalPath = photo.path;

      const lastDetection = lastDetectionRef.current;
      if (lastDetection != null && lastDetection.analysis.found) {
        const { analysis: detection, orientation } = lastDetection;
        const { box, uprightWidth, uprightHeight } = mapBoxToUpright(
          detection.boundingBox,
          detection.frameWidth,
          detection.frameHeight,
          orientation,
        );
        const destPath = croppedPathFor(photo.path);
        const cropped = await cropToDetection(photo.path, destPath, box, uprightWidth, uprightHeight);
        if (cropped) {
          finalPath = destPath;
        }
      }

      const stepLabel = steps[stepIndex];
      setCapturedPhotos(previous => ({ ...previous, [stepLabel]: finalPath }));
      setStepIndex(index => index + 1);
    } catch (error) {
      console.warn('Failed to capture photo', error);
    } finally {
      setIsCapturing(false);
    }
  }, [camera, isCapturing, isDone, lastDetectionRef, steps, stepIndex]);

  return { capturePhoto, isCapturing, stepIndex, isDone, capturedPhotos };
}
