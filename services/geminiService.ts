import { GoogleGenAI, Modality } from "@google/genai";

const getAiClient = () => {
  return new GoogleGenAI({ apiKey: process.env.API_KEY });
};

/**
 * Generate a news-style image from a prompt using Gemini image generation.
 */
export const generateNewsImage = async (prompt: string): Promise<string> => {
  const ai = getAiClient();

  const enhancedPrompt = `${prompt}, photorealistic, news editorial illustration, professional photography, 16:9 composition, high quality, detailed, dramatic lighting`;

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

  throw new Error("이미지 생성 실패: 응답에서 이미지 데이터를 찾을 수 없습니다.");
};

/**
 * Generate TTS audio from narration text using Gemini TTS.
 */
export const generateTTS = async (
  text: string,
  voiceName: string = "Kore"
): Promise<string> => {
  const ai = getAiClient();

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
  if (!base64Audio) throw new Error("TTS 생성 실패");

  const binaryString = atob(base64Audio);
  const len = binaryString.length;
  const bytes = new Uint8Array(len);
  for (let i = 0; i < len; i++) {
    bytes[i] = binaryString.charCodeAt(i);
  }

  const wavBlob = pcmToWav(bytes, 24000);
  return URL.createObjectURL(wavBlob);
};

/**
 * Generate video from image using Veo.
 */
export const generateNewsVideo = async (
  prompt: string,
  imageBase64: string
): Promise<{ url: string; assetContext: any }> => {
  const ai = getAiClient();
  const rawBase64 = imageBase64.replace(/^data:image\/\w+;base64,/, "");

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
