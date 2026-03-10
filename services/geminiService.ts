import { GoogleGenAI, Modality } from "@google/genai";

const getAiClient = () => {
  return new GoogleGenAI({ apiKey: process.env.API_KEY });
};

/**
 * Retry a function with exponential backoff on 429 errors.
 */
async function withRetry<T>(
  fn: () => Promise<T>,
  maxRetries: number = 3,
  baseDelay: number = 2000
): Promise<T> {
  for (let attempt = 0; attempt <= maxRetries; attempt++) {
    try {
      return await fn();
    } catch (e: any) {
      const is429 =
        e?.status === 429 ||
        e?.message?.includes("429") ||
        e?.message?.includes("RESOURCE_EXHAUSTED");

      if (is429 && attempt < maxRetries) {
        const delay = baseDelay * Math.pow(2, attempt);
        console.warn(
          `Rate limited (attempt ${attempt + 1}/${maxRetries + 1}), retrying in ${delay}ms...`
        );
        await new Promise((resolve) => setTimeout(resolve, delay));
        continue;
      }
      throw e;
    }
  }
  throw new Error("Max retries exceeded");
}

/**
 * Generate a news-style image from a prompt.
 * Tries Gemini models with retry, falls back to Pollinations.ai,
 * then generates a placeholder as last resort.
 */
export const generateNewsImage = async (prompt: string): Promise<string> => {
  const ai = getAiClient();

  const enhancedPrompt = `${prompt}, photorealistic, news editorial illustration, professional photography, 16:9 composition, high quality, detailed, dramatic lighting`;

  // Try 1: imagen-3.0-generate-002 with retry
  try {
    return await withRetry(async () => {
      const response = await ai.models.generateImages({
        model: "imagen-3.0-generate-002",
        prompt: enhancedPrompt,
        config: {
          numberOfImages: 1,
        },
      });

      const imageBytes = response.generatedImages?.[0]?.image?.imageBytes;
      if (imageBytes) {
        return `data:image/png;base64,${imageBytes}`;
      }
      throw new Error("No image data in response");
    });
  } catch (e: any) {
    console.warn("Imagen 3.0 failed:", e.message);
  }

  // Try 2: gemini-2.5-flash-image with retry
  try {
    return await withRetry(async () => {
      const response = await ai.models.generateContent({
        model: "gemini-2.5-flash-image",
        contents: {
          parts: [{ text: enhancedPrompt }],
        },
        config: {
          imageConfig: {
            aspectRatio: "16:9",
            numberOfImages: 1,
          },
        },
      });

      if (
        response.candidates &&
        response.candidates[0].content &&
        response.candidates[0].content.parts
      ) {
        for (const part of response.candidates[0].content.parts) {
          if (part.inlineData && part.inlineData.data) {
            const base64Data = part.inlineData.data;
            const mimeType = part.inlineData.mimeType || "image/png";
            return `data:${mimeType};base64,${base64Data}`;
          }
        }
      }
      throw new Error("No image data in response");
    });
  } catch (e: any) {
    console.warn("Gemini flash-image failed:", e.message);
  }

  // Try 3: Pollinations.ai (free, no API key)
  try {
    return await generateImagePollinations(enhancedPrompt);
  } catch (e: any) {
    console.warn("Pollinations.ai failed:", e.message);
  }

  // Final fallback: Canvas placeholder
  return generatePlaceholderImage(prompt);
};

/**
 * Generate image using Pollinations.ai (free, no API key required).
 */
async function generateImagePollinations(prompt: string): Promise<string> {
  const encodedPrompt = encodeURIComponent(prompt);
  const url = `https://image.pollinations.ai/prompt/${encodedPrompt}?width=1280&height=720&nologo=true`;

  const controller = new AbortController();
  const timeout = setTimeout(() => controller.abort(), 60000); // 60s timeout

  try {
    const response = await fetch(url, { signal: controller.signal });
    if (!response.ok) throw new Error(`Pollinations HTTP ${response.status}`);

    const blob = await response.blob();
    return new Promise<string>((resolve, reject) => {
      const reader = new FileReader();
      reader.onloadend = () => resolve(reader.result as string);
      reader.onerror = reject;
      reader.readAsDataURL(blob);
    });
  } finally {
    clearTimeout(timeout);
  }
}

/**
 * Generate a placeholder image using Canvas API when all services fail.
 */
function generatePlaceholderImage(prompt: string): string {
  const canvas = document.createElement("canvas");
  canvas.width = 1280;
  canvas.height = 720;
  const ctx = canvas.getContext("2d")!;

  const gradient = ctx.createLinearGradient(0, 0, 1280, 720);
  gradient.addColorStop(0, "#1a1a2e");
  gradient.addColorStop(0.5, "#16213e");
  gradient.addColorStop(1, "#0f3460");
  ctx.fillStyle = gradient;
  ctx.fillRect(0, 0, 1280, 720);

  ctx.strokeStyle = "rgba(255, 255, 255, 0.03)";
  ctx.lineWidth = 1;
  for (let i = 0; i < 1280; i += 40) {
    ctx.beginPath();
    ctx.moveTo(i, 0);
    ctx.lineTo(i, 720);
    ctx.stroke();
  }
  for (let i = 0; i < 720; i += 40) {
    ctx.beginPath();
    ctx.moveTo(0, i);
    ctx.lineTo(1280, i);
    ctx.stroke();
  }

  const radialGradient = ctx.createRadialGradient(640, 300, 50, 640, 300, 350);
  radialGradient.addColorStop(0, "rgba(233, 69, 96, 0.15)");
  radialGradient.addColorStop(1, "rgba(233, 69, 96, 0)");
  ctx.fillStyle = radialGradient;
  ctx.fillRect(0, 0, 1280, 720);

  ctx.fillStyle = "rgba(233, 69, 96, 0.8)";
  ctx.beginPath();
  ctx.arc(640, 260, 50, 0, Math.PI * 2);
  ctx.fill();
  ctx.fillStyle = "#fff";
  ctx.font = "bold 36px sans-serif";
  ctx.textAlign = "center";
  ctx.fillText("NEWS", 640, 272);

  const shortPrompt =
    prompt.length > 60 ? prompt.substring(0, 60) + "..." : prompt;
  ctx.fillStyle = "rgba(255, 255, 255, 0.9)";
  ctx.font = "bold 28px sans-serif";
  ctx.fillText(shortPrompt, 640, 400);

  ctx.fillStyle = "rgba(255, 255, 255, 0.4)";
  ctx.font = "16px sans-serif";
  ctx.fillText("API 할당량 초과 - 플레이스홀더 이미지", 640, 500);

  return canvas.toDataURL("image/png");
}

/**
 * TTS fallback URL prefix. URLs starting with this use browser Speech API.
 */
export const BROWSER_TTS_PREFIX = "browser-tts:";

/**
 * Check if an audio URL is a browser TTS fallback.
 */
export const isBrowserTTS = (url: string): boolean =>
  url.startsWith(BROWSER_TTS_PREFIX);

/**
 * Play text using the browser's built-in Speech Synthesis API.
 */
export const playBrowserTTS = (
  url: string
): SpeechSynthesisUtterance | null => {
  if (!isBrowserTTS(url)) return null;
  const text = decodeURIComponent(url.slice(BROWSER_TTS_PREFIX.length));

  window.speechSynthesis.cancel();
  const utterance = new SpeechSynthesisUtterance(text);
  utterance.lang = "ko-KR";
  utterance.rate = 0.95;
  utterance.pitch = 1.0;

  const voices = window.speechSynthesis.getVoices();
  const koreanVoice = voices.find((v) => v.lang.startsWith("ko"));
  if (koreanVoice) utterance.voice = koreanVoice;

  window.speechSynthesis.speak(utterance);
  return utterance;
};

/**
 * Stop any ongoing browser TTS playback.
 */
export const stopBrowserTTS = () => {
  window.speechSynthesis.cancel();
};

/**
 * Generate TTS audio from narration text.
 * Tries Gemini TTS with retry, falls back to browser Speech Synthesis API.
 */
export const generateTTS = async (
  text: string,
  voiceName: string = "Kore"
): Promise<string> => {
  const ai = getAiClient();

  try {
    return await withRetry(async () => {
      const response = await ai.models.generateContent({
        model: "gemini-2.5-flash-preview-tts",
        contents: [{ parts: [{ text }] }],
        config: {
          responseModalities: [Modality.AUDIO],
          speechConfig: {
            voiceConfig: {
              prebuiltVoiceConfig: { voiceName },
            },
          },
        },
      });

      const base64Audio =
        response.candidates?.[0]?.content?.parts?.[0]?.inlineData?.data;
      if (!base64Audio) throw new Error("TTS 응답에 오디오 없음");

      const binaryString = atob(base64Audio);
      const len = binaryString.length;
      const bytes = new Uint8Array(len);
      for (let i = 0; i < len; i++) {
        bytes[i] = binaryString.charCodeAt(i);
      }

      const wavBlob = pcmToWav(bytes, 24000);
      return URL.createObjectURL(wavBlob);
    });
  } catch (e: any) {
    console.warn("Gemini TTS failed, using browser Speech API:", e.message);
    return `${BROWSER_TTS_PREFIX}${encodeURIComponent(text)}`;
  }
};

/**
 * Generate video from image using Veo with retry.
 */
export const generateNewsVideo = async (
  prompt: string,
  imageBase64: string
): Promise<{ url: string; assetContext: any }> => {
  // Check if the image is a placeholder (not from API)
  if (imageBase64.includes("API 할당량 초과")) {
    throw new Error(
      "API 이미지로 생성된 경우에만 영상 변환이 가능합니다. 이미지 API 할당량이 복구된 후 다시 시도해주세요."
    );
  }

  const ai = getAiClient();
  const rawBase64 = imageBase64.replace(/^data:image\/\w+;base64,/, "");

  return await withRetry(async () => {
    let operation = await ai.models.generateVideos({
      model: "veo-3.1-fast-generate-preview",
      prompt: `${prompt}, slow subtle camera movement, professional news broadcast quality`,
      image: {
        imageBytes: rawBase64,
        mimeType: "image/png",
      },
      config: {
        numberOfVideos: 1,
        resolution: "720p",
        aspectRatio: "16:9",
      },
    });

    while (!operation.done) {
      await new Promise((resolve) => setTimeout(resolve, 5000));
      operation = await ai.operations.getVideosOperation({
        operation: operation,
      });
    }

    const downloadLink =
      operation.response?.generatedVideos?.[0]?.video?.uri;
    if (!downloadLink) throw new Error("비디오 생성 실패");

    const videoResponse = await fetch(
      `${downloadLink}&key=${process.env.API_KEY}`
    );
    const videoBlob = await videoResponse.blob();
    const url = URL.createObjectURL(videoBlob);
    const assetContext = operation.response?.generatedVideos?.[0]?.video;

    return { url, assetContext };
  });
};

// --- PCM to WAV Helper ---
function pcmToWav(pcmData: Uint8Array, sampleRate: number): Blob {
  const numChannels = 1;
  const bitsPerSample = 16;
  const byteRate = (sampleRate * numChannels * bitsPerSample) / 8;
  const blockAlign = (numChannels * bitsPerSample) / 8;
  const dataSize = pcmData.length;
  const headerSize = 44;
  const totalSize = headerSize + dataSize;

  const buffer = new ArrayBuffer(totalSize);
  const view = new DataView(buffer);

  writeString(view, 0, "RIFF");
  view.setUint32(4, 36 + dataSize, true);
  writeString(view, 8, "WAVE");
  writeString(view, 12, "fmt ");
  view.setUint32(16, 16, true);
  view.setUint16(20, 1, true);
  view.setUint16(22, numChannels, true);
  view.setUint32(24, sampleRate, true);
  view.setUint32(28, byteRate, true);
  view.setUint16(32, blockAlign, true);
  view.setUint16(34, bitsPerSample, true);
  writeString(view, 36, "data");
  view.setUint32(40, dataSize, true);

  const dataView = new Uint8Array(buffer, headerSize);
  dataView.set(pcmData);

  return new Blob([buffer], { type: "audio/wav" });
}

function writeString(view: DataView, offset: number, string: string) {
  for (let i = 0; i < string.length; i++) {
    view.setUint8(offset + i, string.charCodeAt(i));
  }
}
