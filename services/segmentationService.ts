import { GoogleGenAI, Type, Schema } from "@google/genai";
import { DetectionBox } from "../types";
import { DetectedObject, DetectedText, loadImage } from "./layerService";

const getAiClient = () => new GoogleGenAI({ apiKey: process.env.API_KEY });

interface ImageBytes {
  data: string;
  mimeType: string;
}

function splitDataUrl(dataUrl: string): ImageBytes {
  const match = dataUrl.match(/^data:([^;]+);base64,(.+)$/);
  if (!match) {
    return { data: dataUrl, mimeType: 'image/png' };
  }
  return { mimeType: match[1], data: match[2] };
}

function normalizeBox(arr: number[] | undefined): DetectionBox | null {
  if (!arr || arr.length !== 4) return null;
  const [y0, x0, y1, x1] = arr;
  if ([y0, x0, y1, x1].some((v) => typeof v !== 'number' || Number.isNaN(v))) return null;
  return {
    y0: Math.min(y0, y1),
    x0: Math.min(x0, x1),
    y1: Math.max(y0, y1),
    x1: Math.max(x0, x1),
  };
}

const detectionSchema: Schema = {
  type: Type.OBJECT,
  properties: {
    objects: {
      type: Type.ARRAY,
      items: {
        type: Type.OBJECT,
        properties: {
          label: { type: Type.STRING, description: "Short English label for the object (e.g. 'person', 'tree')." },
          box_2d: {
            type: Type.ARRAY,
            items: { type: Type.NUMBER },
            description: "Normalized [y0, x0, y1, x1] in 0-1000 range.",
          },
        },
        required: ["label", "box_2d"],
      },
    },
    texts: {
      type: Type.ARRAY,
      items: {
        type: Type.OBJECT,
        properties: {
          text: { type: Type.STRING, description: "Verbatim text content." },
          box_2d: {
            type: Type.ARRAY,
            items: { type: Type.NUMBER },
            description: "Normalized [y0, x0, y1, x1] in 0-1000 range.",
          },
          color_hint: { type: Type.STRING, description: "Best-guess hex color like #RRGGBB." },
          font_size_hint: { type: Type.NUMBER, description: "Approx font height in 0-1000 range relative to image height." },
        },
        required: ["text", "box_2d"],
      },
    },
  },
  required: ["objects", "texts"],
};

export async function detectObjectsAndText(
  imageDataUrl: string,
): Promise<{ objects: DetectedObject[]; texts: DetectedText[] }> {
  const ai = getAiClient();
  const { data, mimeType } = splitDataUrl(imageDataUrl);

  const response = await ai.models.generateContent({
    model: "gemini-2.5-flash",
    contents: {
      parts: [
        { inlineData: { data, mimeType } },
        {
          text:
            "Detect all distinct foreground objects/characters and any visible text in this image.\n" +
            "Return JSON with two keys: 'objects' and 'texts'.\n" +
            "- objects[]: { label (short English noun), box_2d: [y0,x0,y1,x1] normalized 0-1000 }\n" +
            "- texts[]: { text (verbatim), box_2d: [...], color_hint: '#RRGGBB' best guess, font_size_hint: number 0-1000 (font height relative to image height) }\n" +
            "Limit to at most 10 objects and 10 texts. Skip pure background. Be precise about box coordinates.",
        },
      ],
    },
    config: {
      responseMimeType: "application/json",
      responseSchema: detectionSchema,
    },
  });

  const text = response.text;
  if (!text) throw new Error("Gemini로부터 분석 응답이 없습니다.");

  let parsed: any;
  try {
    const cleaned = text.replace(/```json\n?|```/g, '').trim();
    parsed = JSON.parse(cleaned);
  } catch (e) {
    console.error("Detection JSON parse error", e, text);
    throw new Error("객체 감지 응답을 처리할 수 없습니다.");
  }

  const objects: DetectedObject[] = [];
  for (const o of parsed.objects ?? []) {
    const box = normalizeBox(o.box_2d);
    if (!box) continue;
    objects.push({ label: String(o.label || 'object'), box });
  }

  const texts: DetectedText[] = [];
  for (const t of parsed.texts ?? []) {
    const box = normalizeBox(t.box_2d);
    if (!box) continue;
    texts.push({
      text: String(t.text ?? ''),
      box,
      colorHint: typeof t.color_hint === 'string' ? t.color_hint : undefined,
      fontSizeHint: typeof t.font_size_hint === 'number' ? t.font_size_hint : undefined,
    });
  }

  return { objects, texts };
}

async function dataUrlToCroppedBase64(
  imageDataUrl: string,
  box: DetectionBox,
): Promise<{ data: string; mimeType: string; width: number; height: number }> {
  const img = await loadImage(imageDataUrl);
  const x = (box.x0 / 1000) * img.width;
  const y = (box.y0 / 1000) * img.height;
  const w = ((box.x1 - box.x0) / 1000) * img.width;
  const h = ((box.y1 - box.y0) / 1000) * img.height;

  const canvas = document.createElement('canvas');
  canvas.width = Math.max(1, Math.round(w));
  canvas.height = Math.max(1, Math.round(h));
  const ctx = canvas.getContext('2d');
  if (!ctx) throw new Error('Failed to get 2D context');
  ctx.drawImage(img, x, y, w, h, 0, 0, canvas.width, canvas.height);

  const dataUrl = canvas.toDataURL('image/png');
  const { data, mimeType } = splitDataUrl(dataUrl);
  return { data, mimeType, width: canvas.width, height: canvas.height };
}

const maskSchema: Schema = {
  type: Type.OBJECT,
  properties: {
    mask_png_base64: {
      type: Type.STRING,
      description: "Base64 PNG of a binary mask, white = object, black = background. No data URL prefix, no markdown.",
    },
  },
  required: ["mask_png_base64"],
};

export async function generateMaskForBox(
  imageDataUrl: string,
  box: DetectionBox,
  label: string,
): Promise<string> {
  const ai = getAiClient();
  const crop = await dataUrlToCroppedBase64(imageDataUrl, box);

  const response = await ai.models.generateContent({
    model: "gemini-2.5-flash",
    contents: {
      parts: [
        { inlineData: { data: crop.data, mimeType: crop.mimeType } },
        {
          text:
            `Generate a binary segmentation mask for the '${label}' subject in this cropped image.\n` +
            `Return JSON with key 'mask_png_base64' containing a base64-encoded PNG (no data URL prefix, no markdown) of the same size (${crop.width}x${crop.height}). ` +
            `White (255) marks pixels belonging to the '${label}' subject, black (0) marks everything else. Use crisp edges.`,
        },
      ],
    },
    config: {
      responseMimeType: "application/json",
      responseSchema: maskSchema,
    },
  });

  const text = response.text;
  if (!text) throw new Error("마스크 응답이 비어 있습니다.");

  let parsed: any;
  try {
    const cleaned = text.replace(/```json\n?|```/g, '').trim();
    parsed = JSON.parse(cleaned);
  } catch (e) {
    console.error("Mask JSON parse error", e, text);
    throw new Error("마스크 응답을 처리할 수 없습니다.");
  }

  const b64 = String(parsed.mask_png_base64 || '').trim();
  if (!b64) throw new Error("마스크 데이터가 비어 있습니다.");
  return b64;
}
