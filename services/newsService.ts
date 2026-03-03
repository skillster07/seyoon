import { GoogleGenAI, Type, Schema } from "@google/genai";
import { NewsItem } from "../types";
import { v4 as uuidv4 } from "uuid";

const getAiClient = () => {
  return new GoogleGenAI({ apiKey: process.env.API_KEY });
};

/**
 * Step 1: Search yesterday's news using Gemini with Google Search grounding.
 * Returns raw search context from grounded results.
 */
export const searchYesterdayNews = async (targetDate: string): Promise<string> => {
  const ai = getAiClient();

  const response = await ai.models.generateContent({
    model: "gemini-2.5-flash",
    contents: `오늘 날짜는 ${targetDate} 입니다.
어제(${getPreviousDate(targetDate)}) 한국에서 가장 큰 이슈가 되었던 뉴스, 사건, 사고, 정치, 경제, 사회, 연예, 스포츠 등 모든 분야에서
사람들이 가장 많이 관심을 가졌던 핫이슈 TOP 10을 찾아주세요.

각 뉴스에 대해 다음 정보를 포함해주세요:
- 제목
- 간단한 요약 (2-3문장)
- 어떤 분야인지 (정치, 경제, 사회, 연예, 스포츠 등)
- 왜 이슈가 되었는지

최신 실시간 정보를 기반으로 검색해주세요.`,
    config: {
      tools: [{ googleSearch: {} }],
    },
  });

  const text = response.text;
  if (!text) {
    throw new Error("뉴스 검색 결과가 없습니다.");
  }

  return text;
};

/**
 * Step 2: Analyze searched news and structure into TOP5 NewsItems.
 * Takes raw search results and creates structured data with narration scripts.
 */
export const analyzeAndStructureNews = async (
  rawNewsData: string,
  targetDate: string
): Promise<NewsItem[]> => {
  const ai = getAiClient();

  const responseSchema: Schema = {
    type: Type.ARRAY,
    items: {
      type: Type.OBJECT,
      properties: {
        rank: {
          type: Type.NUMBER,
          description: "뉴스 순위 (1-5)",
        },
        title: {
          type: Type.STRING,
          description: "뉴스 제목 (간결하고 임팩트 있게)",
        },
        summary: {
          type: Type.STRING,
          description: "뉴스 요약 (2-3문장, 핵심 내용만)",
        },
        narration_script: {
          type: Type.STRING,
          description:
            "뉴스 나레이션 대본 (앵커가 읽는 것처럼, 15-20초 분량, 자연스러운 한국어)",
        },
        keywords: {
          type: Type.ARRAY,
          items: { type: Type.STRING },
          description: "핵심 키워드 3-5개",
        },
        image_prompt: {
          type: Type.STRING,
          description:
            "뉴스 내용을 시각적으로 표현하는 이미지 생성 프롬프트 (영어). 뉴스 특성에 맞는 사실적이고 저널리스틱한 이미지. 특정 실존 인물의 얼굴은 포함하지 마세요.",
        },
        source: {
          type: Type.STRING,
          description: "뉴스 출처 (언론사명)",
        },
      },
      required: [
        "rank",
        "title",
        "summary",
        "narration_script",
        "keywords",
        "image_prompt",
      ],
    },
  };

  const response = await ai.models.generateContent({
    model: "gemini-2.5-flash",
    contents: `다음은 ${getPreviousDate(targetDate)} 한국 뉴스 검색 결과입니다:

${rawNewsData}

위 내용을 바탕으로 어제의 이슈 TOP 5를 선정하고, 각 뉴스에 대한 구조화된 데이터를 생성하세요.

[선정 기준]
1. 사회적 파급력과 관심도가 높은 순서
2. 다양한 분야가 골고루 포함되도록 (정치만 5개 X)
3. 시청자가 흥미를 느낄 수 있는 이슈 우선

[나레이션 대본 규칙]
1. 뉴스 앵커처럼 격식체로 작성 ("~입니다", "~했습니다")
2. 15-20초 분량 (약 60-80자)
3. 핵심 사실 중심으로 간결하게
4. 시작은 "N위, [제목]입니다." 형식으로

[이미지 프롬프트 규칙]
1. 영어로 작성
2. 사실적이고 저널리스틱한 스타일
3. "photorealistic, news illustration, editorial style" 포함
4. 실존 인물의 얼굴 묘사 금지 - 대신 실루엣, 상징물, 장소 등으로 표현
5. 16:9 비율에 적합한 구도

정확히 5개의 뉴스 항목을 반환하세요.`,
    config: {
      responseMimeType: "application/json",
      responseSchema: responseSchema,
      systemInstruction:
        "당신은 한국 뉴스 전문 편집자입니다. 어제의 핵심 이슈를 선별하고, 시청자에게 전달력 있는 나레이션 대본을 작성합니다.",
    },
  });

  const text = response.text;
  if (!text) {
    throw new Error("뉴스 분석 결과가 없습니다.");
  }

  const cleanedText = text.replace(/```json\n?|```/g, "").trim();
  const parsed = JSON.parse(cleanedText) as Array<{
    rank: number;
    title: string;
    summary: string;
    narration_script: string;
    keywords: string[];
    image_prompt: string;
    source?: string;
  }>;

  return parsed.map((item) => ({
    id: uuidv4(),
    rank: item.rank,
    title: item.title,
    summary: item.summary,
    narrationScript: item.narration_script,
    keywords: item.keywords,
    imagePrompt: item.image_prompt,
    source: item.source,
    isGeneratingImage: false,
    isGeneratingAudio: false,
    isGeneratingVideo: false,
  }));
};

/**
 * Generate intro/outro narration scripts.
 */
export const generateIntroOutroScripts = async (
  newsItems: NewsItem[],
  targetDate: string
): Promise<{ intro: string; outro: string }> => {
  const ai = getAiClient();

  const headlines = newsItems
    .map((n) => `${n.rank}위: ${n.title}`)
    .join("\n");
  const prevDate = getPreviousDate(targetDate);

  const response = await ai.models.generateContent({
    model: "gemini-2.5-flash",
    contents: `다음은 ${prevDate}의 이슈 TOP5입니다:

${headlines}

위 내용을 바탕으로 뉴스 영상의 인트로와 아웃트로 나레이션을 작성해주세요.

인트로: "${prevDate}, 어제 있었던 이슈 TOP5를 전해드립니다." 형태로 시작하는 10초 분량 (약 40자)
아웃트로: 마무리 멘트, 구독/좋아요 유도, 10초 분량 (약 40자)

JSON 형식으로 반환:
{"intro": "...", "outro": "..."}`,
    config: {
      responseMimeType: "application/json",
    },
  });

  const text = response.text;
  if (!text) throw new Error("인트로/아웃트로 생성 실패");

  const cleanedText = text.replace(/```json\n?|```/g, "").trim();
  return JSON.parse(cleanedText);
};

// Helper: Get previous date string
function getPreviousDate(dateStr: string): string {
  const date = new Date(dateStr);
  date.setDate(date.getDate() - 1);
  return date.toISOString().split("T")[0];
}

// Helper: Get today's date as YYYY-MM-DD
export function getTodayDate(): string {
  const now = new Date();
  return now.toISOString().split("T")[0];
}
