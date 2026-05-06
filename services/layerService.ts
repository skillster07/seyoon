import { v4 as uuidv4 } from 'uuid';
import {
  DetectionBox,
  Layer,
  LayerComposition,
  Transform,
} from '../types';

export interface DetectedObject {
  label: string;
  box: DetectionBox;
  maskBase64?: string;
}

export interface DetectedText {
  text: string;
  box: DetectionBox;
  colorHint?: string;
  fontSizeHint?: number;
}

export function boxToPixels(
  box: DetectionBox,
  imgW: number,
  imgH: number,
): { x: number; y: number; w: number; h: number } {
  const x = (box.x0 / 1000) * imgW;
  const y = (box.y0 / 1000) * imgH;
  const w = ((box.x1 - box.x0) / 1000) * imgW;
  const h = ((box.y1 - box.y0) / 1000) * imgH;
  return { x, y, w, h };
}

export function loadImage(src: string): Promise<HTMLImageElement> {
  return new Promise((resolve, reject) => {
    const img = new Image();
    img.crossOrigin = 'anonymous';
    img.onload = () => resolve(img);
    img.onerror = (e) => reject(e);
    img.src = src;
  });
}

export async function cropImage(
  img: HTMLImageElement,
  x: number,
  y: number,
  w: number,
  h: number,
): Promise<string> {
  const canvas = document.createElement('canvas');
  canvas.width = Math.max(1, Math.round(w));
  canvas.height = Math.max(1, Math.round(h));
  const ctx = canvas.getContext('2d');
  if (!ctx) throw new Error('Failed to get 2D context');
  ctx.drawImage(img, x, y, w, h, 0, 0, canvas.width, canvas.height);
  return canvas.toDataURL('image/png');
}

function ensureDataUrl(maybeBase64: string, mime = 'image/png'): string {
  if (maybeBase64.startsWith('data:')) return maybeBase64;
  return `data:${mime};base64,${maybeBase64}`;
}

export async function applyMaskToImage(
  srcDataUrl: string,
  maskBase64: string,
): Promise<string> {
  const [src, mask] = await Promise.all([
    loadImage(srcDataUrl),
    loadImage(ensureDataUrl(maskBase64)),
  ]);

  const w = src.width;
  const h = src.height;

  const canvas = document.createElement('canvas');
  canvas.width = w;
  canvas.height = h;
  const ctx = canvas.getContext('2d');
  if (!ctx) throw new Error('Failed to get 2D context');

  ctx.drawImage(src, 0, 0, w, h);
  ctx.globalCompositeOperation = 'destination-in';
  ctx.drawImage(mask, 0, 0, w, h);
  ctx.globalCompositeOperation = 'source-over';

  return canvas.toDataURL('image/png');
}

function defaultTransform(
  x: number,
  y: number,
  w: number,
  h: number,
): Transform {
  return {
    x,
    y,
    width: w,
    height: h,
    rotation: 0,
    scaleX: 1,
    scaleY: 1,
    opacity: 1,
  };
}

export function detectionToObjectLayer(
  d: DetectedObject,
  imgW: number,
  imgH: number,
  croppedDataUrl: string,
  zIndex: number,
): Layer {
  const { x, y, w, h } = boxToPixels(d.box, imgW, imgH);
  return {
    id: uuidv4(),
    type: 'object',
    label: d.label || 'object',
    zIndex,
    visible: true,
    locked: false,
    transform: defaultTransform(x, y, w, h),
    imageDataUrl: croppedDataUrl,
    sourceBox: d.box,
    maskDataUrl: d.maskBase64
      ? ensureDataUrl(d.maskBase64)
      : undefined,
  };
}

export function detectionToTextLayer(
  d: DetectedText,
  imgW: number,
  imgH: number,
  zIndex: number,
): Layer {
  const { x, y, w, h } = boxToPixels(d.box, imgW, imgH);
  const fontSize = d.fontSizeHint
    ? (d.fontSizeHint / 1000) * imgH
    : Math.max(12, h * 0.7);
  return {
    id: uuidv4(),
    type: 'text',
    label: `text-${d.text.slice(0, 12) || 'empty'}`,
    zIndex,
    visible: true,
    locked: false,
    transform: defaultTransform(x, y, w, h),
    text: d.text,
    fontFamily: 'Pretendard, system-ui, sans-serif',
    fontSize,
    color: d.colorHint || '#ffffff',
    textAlign: 'left',
  };
}

export function makeBackgroundLayer(
  sourceImageUrl: string,
  width: number,
  height: number,
): Layer {
  return {
    id: uuidv4(),
    type: 'background',
    label: 'Background',
    zIndex: 0,
    visible: true,
    locked: true,
    transform: defaultTransform(0, 0, width, height),
    imageDataUrl: sourceImageUrl,
  };
}

export function newComposition(
  sourceImageUrl: string,
  width: number,
  height: number,
): LayerComposition {
  return {
    id: uuidv4(),
    sourceImageUrl,
    width,
    height,
    layers: [makeBackgroundLayer(sourceImageUrl, width, height)],
    createdAt: Date.now(),
  };
}
