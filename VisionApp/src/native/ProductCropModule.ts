import { NativeModules } from 'react-native';

interface ProductCropNativeModule {
  cropToBoundingBox(
    sourcePath: string,
    destPath: string,
    boxX: number,
    boxY: number,
    boxWidth: number,
    boxHeight: number,
    analysisFrameWidth: number,
    analysisFrameHeight: number,
  ): Promise<boolean>;
}

const ProductCropModule = NativeModules.ProductCropModule as ProductCropNativeModule | undefined;

export interface BoundingBox {
  x: number;
  y: number;
  width: number;
  height: number;
}

/**
 * Crops the photo at `sourcePath` to `box` (given in the same pixel space as
 * `analysisFrameWidth` x `analysisFrameHeight` - i.e. a ProductAnalysis'
 * `boundingBox` together with its `frameWidth`/`frameHeight`) and writes the
 * result as a JPEG to `destPath`, via the native OpenCV pipeline
 * (cpp/ProductAnalyzer.cpp's cropPhotoToBoundingBox).
 *
 * The box is scaled proportionally from analysis-frame space into the
 * photo's own resolution, assuming both share the same aspect ratio and
 * orientation - true for a given camera device's frame-processor and photo
 * streams, but not verified across every device/OS combination.
 *
 * Resolves to whether the crop succeeded (false if the source photo can't be
 * read, or the box has no area once clamped to the photo bounds); never
 * throws for those cases; only rejects on an unexpected native error.
 */
export async function cropToDetection(
  sourcePath: string,
  destPath: string,
  box: BoundingBox,
  analysisFrameWidth: number,
  analysisFrameHeight: number,
): Promise<boolean> {
  if (ProductCropModule == null) {
    throw new Error('Native module "ProductCropModule" is not registered!');
  }
  return ProductCropModule.cropToBoundingBox(
    sourcePath,
    destPath,
    box.x,
    box.y,
    box.width,
    box.height,
    analysisFrameWidth,
    analysisFrameHeight,
  );
}
