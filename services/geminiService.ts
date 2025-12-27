import { GoogleGenAI, Type, Schema, Modality } from "@google/genai";
import { ScriptAnalysisResponse, Character, ShortsTemplate, TemplateScene, Scene } from "../types";
import { v4 as uuidv4 } from 'uuid';

// Helper to get client with current key
const getAiClient = () => {
  return new GoogleGenAI({ apiKey: process.env.API_KEY });
};

/**
 * Analyzes the raw script and breaks it down into scenes with visual prompts.
 */
export const analyzeScript = async (script: string): Promise<ScriptAnalysisResponse[]> => {
  const ai = getAiClient();
  const responseSchema: Schema = {
    type: Type.ARRAY,
    items: {
      type: Type.OBJECT,
      properties: {
        original_text: {
          type: Type.STRING,
          description: "이 샷(Shot)에 해당하는 대사나 지문입니다.",
        },
        visual_prompt: {
          type: Type.STRING,
          description: "이 샷에 대한 상세한 이미지 생성 프롬프트입니다. (영어)",
        },
      },
      required: ["original_text", "visual_prompt"],
    },
  };

  const response = await ai.models.generateContent({
    model: "gemini-2.5-flash",
    contents: `다음 텍스트는 "붉은 정원" 애니메이션의 에피소드 스크립트입니다. 
    이 텍스트를 "스토리보드 샷(Shot)" 단위로 잘게 쪼개어 JSON 데이터를 생성하십시오.
    
    [중요 규칙]
    1. 긴 대화나 지문은 시각적 구도, 화자, 또는 감정이 바뀔 때마다 반드시 새로운 샷으로 분리해야 합니다.
    2. 한 샷은 영상으로 만들었을 때 약 3~5초 분량이 되도록 짧게 구성하세요.
    3. 'visual_prompt'는 반드시 영어로 작성하며, 다음 키워드를 포함하여 시네마틱한 구도를 유도하세요:
       "cinematic shot, rule of thirds, depth of field, dynamic lighting, detailed background, anime style"
    
    스크립트:
    ${script}`,
    config: {
      responseMimeType: "application/json",
      responseSchema: responseSchema,
      systemInstruction: "당신은 로맨스 웹툰/애니메이션 스토리보드 감독입니다. 각 장면을 시각적으로 지루하지 않게 컷 단위로 쪼개고, 앵글과 조명을 상세히 묘사합니다.",
    },
  });

  const text = response.text;
  if (!text) {
    throw new Error("Gemini로부터 응답이 없습니다.");
  }

  try {
    // Sanitize JSON string (remove markdown code blocks if present)
    const cleanedText = text.replace(/```json\n?|```/g, '').trim();
    return JSON.parse(cleanedText) as ScriptAnalysisResponse[];
  } catch (e) {
    console.error("Failed to parse JSON", e);
    console.error("Raw text:", text);
    throw new Error("스크립트 분석 결과를 처리하는 데 실패했습니다. (JSON 파싱 오류)");
  }
};

/**
 * Generates a story script based on a topic and characters.
 */
export const generateStory = async (topic: string, characters: Character[]): Promise<string> => {
  const ai = getAiClient();
  
  const characterContext = characters.length > 0 
    ? `등장인물 정보:\n${characters.map(c => `- ${c.name} (${c.age}세): ${c.description}`).join('\n')}`
    : "등장인물: 자유롭게 설정";

  const prompt = `
    다음 주제로 "붉은 정원" 애니메이션의 에피소드 시나리오를 작성해줘.
    
    주제: ${topic}
    
    ${characterContext}
    
    형식:
    씬 넘버, 장소, 시간, 지문, 대사가 포함된 상세한 스크립트 형식으로 작성해.
    한국어로 작성해줘.
  `;

  const response = await ai.models.generateContent({
    model: "gemini-2.5-flash",
    contents: prompt,
    config: {
      systemInstruction: "당신은 로맨스 웹툰/애니메이션 전문 시나리오 작가입니다. 감정선이 살아있는 대사와 생생한 지문을 작성합니다.",
    }
  });

  const text = response.text;
  if (!text) throw new Error("스토리 생성에 실패했습니다.");
  return text;
};

/**
 * Generates an image based on a text prompt using Gemini 2.5 Flash Image model.
 */
export const generateImageFromPrompt = async (prompt: string): Promise<string> => {
  const ai = getAiClient();
  try {
    // Force style consistency and cinematic composition
    const cinematicSuffix = ", cinematic composition, rule of thirds, dynamic angle, depth of field, perfect lighting, 8k resolution, masterpiece";
    const styleSuffix = ", modern romance webtoon style, anime style, highly detailed";
    
    let finalPrompt = prompt;
    if (!finalPrompt.toLowerCase().includes('cinematic')) finalPrompt += cinematicSuffix;
    if (!finalPrompt.toLowerCase().includes('anime style')) finalPrompt += styleSuffix;

    const response = await ai.models.generateContent({
      model: "gemini-2.5-flash-image",
      contents: {
        parts: [
          { text: finalPrompt }
        ]
      },
      config: {
        imageConfig: {
          aspectRatio: "16:9"
        }
      }
    });

    if (response.candidates && response.candidates[0].content && response.candidates[0].content.parts) {
      for (const part of response.candidates[0].content.parts) {
        if (part.inlineData && part.inlineData.data) {
          const base64Data = part.inlineData.data;
          const mimeType = part.inlineData.mimeType || 'image/png';
          return `data:${mimeType};base64,${base64Data}`;
        }
      }
    }

    throw new Error("응답에서 이미지 데이터를 찾을 수 없습니다.");
  } catch (error) {
    console.error("Image generation failed:", error);
    throw error;
  }
};

/**
 * Generates a video using Veo 3.1 based on an image and a prompt.
 * Returns the video URL and the raw operation response (for extension).
 */
export const generateVideo = async (prompt: string, imageBase64: string): Promise<{url: string, assetContext: any}> => {
  const ai = getAiClient();
  const rawBase64 = imageBase64.replace(/^data:image\/\w+;base64,/, "");
  
  try {
    let operation = await ai.models.generateVideos({
      model: 'veo-3.1-fast-generate-preview',
      prompt: `${prompt}, cinematic movement, high quality, fluid motion`,
      image: {
        imageBytes: rawBase64,
        mimeType: 'image/png',
      },
      config: {
        numberOfVideos: 1,
        resolution: '720p',
        aspectRatio: '16:9'
      }
    });

    // Poll for completion
    while (!operation.done) {
      await new Promise(resolve => setTimeout(resolve, 5000));
      operation = await ai.operations.getVideosOperation({operation: operation});
    }

    const downloadLink = operation.response?.generatedVideos?.[0]?.video?.uri;
    if (!downloadLink) throw new Error("비디오 생성 실패: 다운로드 링크가 없습니다.");

    const videoResponse = await fetch(`${downloadLink}&key=${process.env.API_KEY}`);
    const videoBlob = await videoResponse.blob();
    const url = URL.createObjectURL(videoBlob);
    const assetContext = operation.response?.generatedVideos?.[0]?.video;

    return { url, assetContext };

  } catch (error) {
    console.error("Video generation failed:", error);
    throw error;
  }
};

/**
 * Extends a video by 5-7 seconds.
 */
export const extendVideo = async (prompt: string, previousVideoAsset: any): Promise<{url: string, assetContext: any}> => {
  const ai = getAiClient();
  
  if (!previousVideoAsset) {
    throw new Error("이전 비디오 정보가 없어 연장할 수 없습니다.");
  }

  try {
    let operation = await ai.models.generateVideos({
      model: 'veo-3.1-generate-preview', // Must use this model for extension
      prompt: `${prompt}, continue the scene seamlessly, cinematic`,
      video: previousVideoAsset, 
      config: {
        numberOfVideos: 1,
        resolution: '720p',
        aspectRatio: '16:9'
      }
    });

    while (!operation.done) {
      await new Promise(resolve => setTimeout(resolve, 5000));
      operation = await ai.operations.getVideosOperation({operation: operation});
    }

    const downloadLink = operation.response?.generatedVideos?.[0]?.video?.uri;
    if (!downloadLink) throw new Error("비디오 연장 실패");

    const videoResponse = await fetch(`${downloadLink}&key=${process.env.API_KEY}`);
    const videoBlob = await videoResponse.blob();
    const url = URL.createObjectURL(videoBlob);
    const assetContext = operation.response?.generatedVideos?.[0]?.video;

    return { url, assetContext };

  } catch (error) {
    console.error("Video extension failed:", error);
    throw error;
  }
};


/**
 * Generates speech (dialogue) using Gemini TTS.
 */
export const generateSpeech = async (text: string, voiceName: string = 'Kore'): Promise<string> => {
  const ai = getAiClient();
  try {
    const response = await ai.models.generateContent({
      model: "gemini-2.5-flash-preview-tts",
      contents: [{ parts: [{ text: text }] }],
      config: {
        responseModalities: [Modality.AUDIO],
        speechConfig: {
            voiceConfig: {
              prebuiltVoiceConfig: { voiceName: voiceName },
            },
        },
      },
    });

    const base64Audio = response.candidates?.[0]?.content?.parts?.[0]?.inlineData?.data;
    if (!base64Audio) throw new Error("오디오 생성 실패");

    const binaryString = atob(base64Audio);
    const len = binaryString.length;
    const bytes = new Uint8Array(len);
    for (let i = 0; i < len; i++) {
      bytes[i] = binaryString.charCodeAt(i);
    }
    
    const wavBlob = pcmToWav(bytes, 24000); 
    return URL.createObjectURL(wavBlob);

  } catch (error) {
    console.error("Speech generation failed:", error);
    throw error;
  }
};

/**
 * Simulates BGM generation.
 */
export const generateBgm = async (mood: string): Promise<string> => {
  await new Promise(resolve => setTimeout(resolve, 2000));
  return "https://cdn.pixabay.com/download/audio/2022/05/27/audio_1808fbf07a.mp3?filename=lofi-study-112191.mp3"; 
};

// --- HELPER: PCM to WAV ---
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

  writeString(view, 0, 'RIFF');
  view.setUint32(4, 36 + dataSize, true);
  writeString(view, 8, 'WAVE');
  writeString(view, 12, 'fmt ');
  view.setUint32(16, 16, true);
  view.setUint16(20, 1, true);
  view.setUint16(22, numChannels, true);
  view.setUint32(24, sampleRate, true);
  view.setUint32(28, byteRate, true);
  view.setUint16(32, blockAlign, true);
  view.setUint16(34, bitsPerSample, true);
  writeString(view, 36, 'data');
  view.setUint32(40, dataSize, true);

  const dataView = new Uint8Array(buffer, headerSize);
  dataView.set(pcmData);

  return new Blob([buffer], { type: 'audio/wav' });
}

function writeString(view: DataView, offset: number, string: string) {
  for (let i = 0; i < string.length; i++) {
    view.setUint8(offset + i, string.charCodeAt(i));
  }
}

// ============================================
// Shorts Template Functions
// ============================================

/**
 * Generates a vertical (9:16) image for shorts/reels content.
 */
export const generateShortsImage = async (prompt: string): Promise<string> => {
  const ai = getAiClient();
  try {
    // Add cinematic dark-fantasy maritime style for sea monster content
    const styleSuffix = ", cinematic composition, volumetric lighting, dramatic atmosphere, high contrast, 8k resolution, realistic, no watermark, no text, no logo";

    let finalPrompt = prompt;
    if (!finalPrompt.toLowerCase().includes('cinematic')) finalPrompt += styleSuffix;

    const response = await ai.models.generateContent({
      model: "gemini-2.5-flash-image",
      contents: {
        parts: [
          { text: finalPrompt }
        ]
      },
      config: {
        imageConfig: {
          aspectRatio: "9:16"  // Vertical for shorts
        }
      }
    });

    if (response.candidates && response.candidates[0].content && response.candidates[0].content.parts) {
      for (const part of response.candidates[0].content.parts) {
        if (part.inlineData && part.inlineData.data) {
          const base64Data = part.inlineData.data;
          const mimeType = part.inlineData.mimeType || 'image/png';
          return `data:${mimeType};base64,${base64Data}`;
        }
      }
    }

    throw new Error("응답에서 이미지 데이터를 찾을 수 없습니다.");
  } catch (error) {
    console.error("Shorts image generation failed:", error);
    throw error;
  }
};

/**
 * Generates a vertical video for shorts/reels using Veo.
 */
export const generateShortsVideo = async (prompt: string, imageBase64: string): Promise<{url: string, assetContext: any}> => {
  const ai = getAiClient();
  const rawBase64 = imageBase64.replace(/^data:image\/\w+;base64,/, "");

  try {
    let operation = await ai.models.generateVideos({
      model: 'veo-3.1-fast-generate-preview',
      prompt: `${prompt}, vertical video 9:16 aspect ratio, cinematic movement, high quality, fluid motion, dramatic lighting`,
      image: {
        imageBytes: rawBase64,
        mimeType: 'image/png',
      },
      config: {
        numberOfVideos: 1,
        resolution: '720p',
        aspectRatio: '9:16'  // Vertical for shorts
      }
    });

    // Poll for completion
    while (!operation.done) {
      await new Promise(resolve => setTimeout(resolve, 5000));
      operation = await ai.operations.getVideosOperation({operation: operation});
    }

    const downloadLink = operation.response?.generatedVideos?.[0]?.video?.uri;
    if (!downloadLink) throw new Error("비디오 생성 실패: 다운로드 링크가 없습니다.");

    const videoResponse = await fetch(`${downloadLink}&key=${process.env.API_KEY}`);
    const videoBlob = await videoResponse.blob();
    const url = URL.createObjectURL(videoBlob);
    const assetContext = operation.response?.generatedVideos?.[0]?.video;

    return { url, assetContext };

  } catch (error) {
    console.error("Shorts video generation failed:", error);
    throw error;
  }
};

/**
 * Helper to get flag emoji from country code
 */
export function getFlagEmoji(countryCode: string): string {
  const codePoints = countryCode
    .toUpperCase()
    .split('')
    .map(char => 127397 + char.charCodeAt(0));
  return String.fromCodePoint(...codePoints);
}

/**
 * Converts a ShortsTemplate to an array of Scene objects.
 * This flattens the template's scenes/shots structure into individual scenes.
 */
export const convertTemplateToScenes = (template: ShortsTemplate): Scene[] => {
  const scenes: Scene[] = [];
  const globalStyle = template.global_style;
  const negativePrompt = globalStyle.negative_prompt.join(', ');
  const visualTone = globalStyle.visual_tone.join(', ');
  const vfxEffects = globalStyle.vfx.join(', ');

  template.scenes.forEach((templateScene: TemplateScene) => {
    templateScene.shots.forEach((shot) => {
      // Build enhanced prompt with global style context
      const enhancedPrompt = [
        shot.prompt,
        `visual style: ${visualTone}`,
        `VFX: ${vfxEffects}`,
        `camera: ${shot.camera}`,
        `vertical 9:16 aspect ratio for shorts/reels`,
        `theme: ${templateScene.theme_monster}`,
        `Negative prompt: ${negativePrompt}`
      ].join(', ');

      const scene: Scene = {
        id: uuidv4(),
        originalText: `[${templateScene.country.name_ko} ${getFlagEmoji(templateScene.country.flag)}] ${templateScene.theme_monster} - ${shot.shot_id}`,
        visualPrompt: enhancedPrompt,
        duration: shot.duration_s,
        transition: 'fade',
        imageVersions: [],
        videoVersions: [],
        audioVersions: [],
        isGeneratingImage: false,
        isGeneratingVideo: false,
        isGeneratingAudio: false
      };
      scenes.push(scene);
    });
  });

  return scenes;
};