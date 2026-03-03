import React, { useState, useCallback } from "react";
import { v4 as uuidv4 } from "uuid";
import {
  NewsItem,
  PipelineState,
  PipelineStep,
  ProjectData,
  AIStudio,
} from "./types";
import {
  searchYesterdayNews,
  analyzeAndStructureNews,
  generateIntroOutroScripts,
  getTodayDate,
} from "./services/newsService";
import {
  generateNewsImage,
  generateTTS,
  generateNewsVideo,
} from "./services/geminiService";
import { PipelineProgress } from "./components/PipelineProgress";
import { NewsCard } from "./components/NewsCard";
import { PreviewPlayer } from "./components/PreviewPlayer";
import {
  Newspaper,
  Rocket,
  ImagePlus,
  Volume2,
  Play,
  Download,
  RefreshCw,
  Calendar,
  Zap,
  Eye,
  Loader2,
} from "lucide-react";

const App: React.FC = () => {
  // Pipeline
  const [pipeline, setPipeline] = useState<PipelineState>({
    step: PipelineStep.IDLE,
    progress: 0,
    message: "",
  });

  // Project Data
  const [project, setProject] = useState<ProjectData | null>(null);
  const [newsItems, setNewsItems] = useState<NewsItem[]>([]);
  const [introScript, setIntroScript] = useState("");
  const [outroScript, setOutroScript] = useState("");
  const [introAudioUrl, setIntroAudioUrl] = useState<string | undefined>();
  const [outroAudioUrl, setOutroAudioUrl] = useState<string | undefined>();

  // UI
  const [showPreview, setShowPreview] = useState(false);
  const [targetDate, setTargetDate] = useState(getTodayDate());

  // === Pipeline Step 1: Search News ===
  const handleSearchNews = useCallback(async () => {
    setPipeline({
      step: PipelineStep.SEARCHING_NEWS,
      progress: 10,
      message: "Google 검색을 통해 어제의 뉴스를 수집하고 있습니다...",
    });

    try {
      const rawNews = await searchYesterdayNews(targetDate);

      setPipeline({
        step: PipelineStep.ANALYZING,
        progress: 40,
        message: "AI가 TOP5 이슈를 선정하고 나레이션 대본을 작성 중입니다...",
      });

      const items = await analyzeAndStructureNews(rawNews, targetDate);

      setPipeline({
        step: PipelineStep.ANALYZING,
        progress: 60,
        message: "인트로/아웃트로 대본을 생성 중입니다...",
      });

      const { intro, outro } = await generateIntroOutroScripts(
        items,
        targetDate
      );

      setNewsItems(items);
      setIntroScript(intro);
      setOutroScript(outro);
      setProject({
        id: uuidv4(),
        title: `어제의 이슈 TOP5 - ${getPreviousDate(targetDate)}`,
        date: targetDate,
        createdAt: Date.now(),
        newsItems: items,
        introScript: intro,
        outroScript: outro,
      });

      setPipeline({
        step: PipelineStep.REVIEW,
        progress: 70,
        message:
          "뉴스 분석이 완료되었습니다. 내용을 검토하고 미디어를 생성하세요.",
      });
    } catch (e: any) {
      console.error(e);
      setPipeline({
        step: PipelineStep.IDLE,
        progress: 0,
        message: "",
        error: e.message || "뉴스 검색에 실패했습니다.",
      });
    }
  }, [targetDate]);

  // === Update news item ===
  const handleUpdateItem = useCallback((updated: NewsItem) => {
    setNewsItems((prev) =>
      prev.map((n) => (n.id === updated.id ? updated : n))
    );
  }, []);

  // === Generate Image for single item ===
  const handleGenerateImage = useCallback(
    async (id: string, prompt: string) => {
      setNewsItems((prev) =>
        prev.map((n) =>
          n.id === id
            ? { ...n, isGeneratingImage: true, error: undefined }
            : n
        )
      );

      try {
        const imageUrl = await generateNewsImage(prompt);
        setNewsItems((prev) =>
          prev.map((n) =>
            n.id === id ? { ...n, imageUrl, isGeneratingImage: false } : n
          )
        );
      } catch (e: any) {
        setNewsItems((prev) =>
          prev.map((n) =>
            n.id === id
              ? {
                  ...n,
                  isGeneratingImage: false,
                  error: "이미지 생성 실패: " + e.message,
                }
              : n
          )
        );
      }
    },
    []
  );

  // === Generate Audio for single item ===
  const handleGenerateAudio = useCallback(
    async (id: string, text: string) => {
      setNewsItems((prev) =>
        prev.map((n) =>
          n.id === id
            ? { ...n, isGeneratingAudio: true, error: undefined }
            : n
        )
      );

      try {
        const audioUrl = await generateTTS(text);
        setNewsItems((prev) =>
          prev.map((n) =>
            n.id === id ? { ...n, audioUrl, isGeneratingAudio: false } : n
          )
        );
      } catch (e: any) {
        setNewsItems((prev) =>
          prev.map((n) =>
            n.id === id
              ? {
                  ...n,
                  isGeneratingAudio: false,
                  error: "TTS 생성 실패: " + e.message,
                }
              : n
          )
        );
      }
    },
    []
  );

  // === Generate Video for single item ===
  const handleGenerateVideo = useCallback(
    async (id: string) => {
      const item = newsItems.find((n) => n.id === id);
      if (!item || !item.imageUrl) return;

      setNewsItems((prev) =>
        prev.map((n) =>
          n.id === id
            ? { ...n, isGeneratingVideo: true, error: undefined }
            : n
        )
      );

      try {
        const { url, assetContext } = await generateNewsVideo(
          item.imagePrompt,
          item.imageUrl
        );
        setNewsItems((prev) =>
          prev.map((n) =>
            n.id === id
              ? {
                  ...n,
                  videoUrl: url,
                  videoAssetContext: assetContext,
                  isGeneratingVideo: false,
                }
              : n
          )
        );
      } catch (e: any) {
        setNewsItems((prev) =>
          prev.map((n) =>
            n.id === id
              ? {
                  ...n,
                  isGeneratingVideo: false,
                  error: "영상 생성 실패: " + e.message,
                }
              : n
          )
        );
      }
    },
    [newsItems]
  );

  // === Batch Generate: All Images ===
  const handleGenerateAllImages = useCallback(async () => {
    const itemsWithoutImages = newsItems.filter((n) => !n.imageUrl);
    for (const item of itemsWithoutImages) {
      await handleGenerateImage(item.id, item.imagePrompt);
    }
  }, [newsItems, handleGenerateImage]);

  // === Batch Generate: All TTS ===
  const handleGenerateAllAudio = useCallback(async () => {
    const itemsWithoutAudio = newsItems.filter((n) => !n.audioUrl);

    // Also generate intro/outro audio
    if (introScript && !introAudioUrl) {
      try {
        const url = await generateTTS(introScript);
        setIntroAudioUrl(url);
      } catch (e) {
        console.error("Intro TTS failed:", e);
      }
    }

    for (const item of itemsWithoutAudio) {
      await handleGenerateAudio(item.id, item.narrationScript);
    }

    if (outroScript && !outroAudioUrl) {
      try {
        const url = await generateTTS(outroScript);
        setOutroAudioUrl(url);
      } catch (e) {
        console.error("Outro TTS failed:", e);
      }
    }
  }, [
    newsItems,
    introScript,
    outroScript,
    introAudioUrl,
    outroAudioUrl,
    handleGenerateAudio,
  ]);

  // === Batch Generate: All Media ===
  const handleGenerateAllMedia = useCallback(async () => {
    setPipeline({
      step: PipelineStep.GENERATING_MEDIA,
      progress: 75,
      message: "모든 이미지를 생성 중입니다...",
    });

    await handleGenerateAllImages();

    setPipeline({
      step: PipelineStep.GENERATING_MEDIA,
      progress: 85,
      message: "모든 TTS 음성을 생성 중입니다...",
    });

    await handleGenerateAllAudio();

    setPipeline({
      step: PipelineStep.COMPLETE,
      progress: 100,
      message: "모든 미디어 생성이 완료되었습니다!",
    });
  }, [handleGenerateAllImages, handleGenerateAllAudio]);

  // === Reset ===
  const handleReset = () => {
    setPipeline({ step: PipelineStep.IDLE, progress: 0, message: "" });
    setNewsItems([]);
    setProject(null);
    setIntroScript("");
    setOutroScript("");
    setIntroAudioUrl(undefined);
    setOutroAudioUrl(undefined);
    setShowPreview(false);
  };

  const isIdle = pipeline.step === PipelineStep.IDLE;
  const hasNews = newsItems.length > 0;

  return (
    <div className="min-h-screen bg-[#0a0f1e] text-gray-100 font-sans">
      {/* Header */}
      <header className="bg-gray-900/80 backdrop-blur-md border-b border-gray-800 sticky top-0 z-50">
        <div className="max-w-6xl mx-auto px-6 h-16 flex items-center justify-between">
          <div className="flex items-center gap-3">
            <div className="bg-gradient-to-br from-red-600 to-orange-500 p-2 rounded-lg shadow-lg">
              <Newspaper className="text-white" size={22} />
            </div>
            <div>
              <h1 className="text-lg font-bold text-white leading-none">
                뉴스 비디오 자동화
              </h1>
              <p className="text-xs text-gray-500">
                어제의 이슈 TOP5 콘텐츠 생성기
              </p>
            </div>
          </div>

          <div className="flex items-center gap-3">
            {hasNews && (
              <>
                <button
                  onClick={() => setShowPreview(!showPreview)}
                  className="flex items-center gap-1.5 px-4 py-2 bg-gray-800 hover:bg-gray-700 text-gray-300 rounded-lg text-sm transition-colors border border-gray-700"
                >
                  {showPreview ? <Eye size={16} /> : <Play size={16} />}
                  {showPreview ? "편집 모드" : "미리보기"}
                </button>
                <button
                  onClick={handleReset}
                  className="flex items-center gap-1.5 px-4 py-2 bg-gray-800 hover:bg-gray-700 text-gray-300 rounded-lg text-sm transition-colors border border-gray-700"
                >
                  <RefreshCw size={16} /> 초기화
                </button>
              </>
            )}
          </div>
        </div>
      </header>

      <main className="max-w-6xl mx-auto px-6 py-8">
        {/* Pipeline Progress */}
        {!isIdle && (
          <div className="mb-8">
            <PipelineProgress state={pipeline} />
          </div>
        )}

        {/* === IDLE: Start Screen === */}
        {isIdle && !hasNews && (
          <div className="flex flex-col items-center justify-center min-h-[70vh]">
            <div className="text-center mb-10">
              <div className="inline-flex items-center justify-center w-20 h-20 rounded-2xl bg-gradient-to-br from-red-600 to-orange-500 mb-6 shadow-2xl shadow-red-500/20">
                <Zap className="text-white" size={40} />
              </div>
              <h2 className="text-4xl font-black text-white mb-3">
                어제의 이슈 TOP5
              </h2>
              <p className="text-gray-400 text-lg max-w-md mx-auto">
                AI가 어제의 핫이슈를 검색하고, 뉴스 영상 콘텐츠를 자동으로
                생성합니다.
              </p>
            </div>

            <div className="bg-gray-800/50 rounded-2xl p-8 border border-gray-700 w-full max-w-lg">
              <div className="flex items-center gap-3 mb-6">
                <Calendar size={20} className="text-blue-400" />
                <label className="text-sm font-medium text-gray-300">
                  기준 날짜 (오늘 기준으로 어제 뉴스를 검색합니다)
                </label>
              </div>
              <input
                type="date"
                value={targetDate}
                onChange={(e) => setTargetDate(e.target.value)}
                className="w-full bg-gray-900 border border-gray-600 rounded-xl px-4 py-3 text-white mb-6 focus:border-blue-500 focus:outline-none focus:ring-2 focus:ring-blue-500/20"
              />

              {pipeline.error && (
                <div className="mb-4 p-3 bg-red-900/30 border border-red-800 rounded-lg text-red-300 text-sm">
                  {pipeline.error}
                </div>
              )}

              <button
                onClick={handleSearchNews}
                className="w-full flex items-center justify-center gap-3 px-8 py-4 bg-gradient-to-r from-red-600 to-orange-500 hover:from-red-500 hover:to-orange-400 text-white rounded-xl font-bold text-lg shadow-lg shadow-red-500/30 hover:shadow-red-500/50 transition-all transform hover:scale-[1.02]"
              >
                <Rocket size={24} />
                어제의 이슈 검색 시작
              </button>

              <div className="mt-6 grid grid-cols-3 gap-4 text-center">
                <div className="bg-gray-900/50 rounded-lg p-3">
                  <p className="text-2xl font-bold text-blue-400">1</p>
                  <p className="text-xs text-gray-500 mt-1">뉴스 검색</p>
                </div>
                <div className="bg-gray-900/50 rounded-lg p-3">
                  <p className="text-2xl font-bold text-purple-400">2</p>
                  <p className="text-xs text-gray-500 mt-1">AI 분석</p>
                </div>
                <div className="bg-gray-900/50 rounded-lg p-3">
                  <p className="text-2xl font-bold text-green-400">3</p>
                  <p className="text-xs text-gray-500 mt-1">영상 생성</p>
                </div>
              </div>
            </div>
          </div>
        )}

        {/* === SEARCHING / ANALYZING === */}
        {(pipeline.step === PipelineStep.SEARCHING_NEWS ||
          pipeline.step === PipelineStep.ANALYZING) &&
          !hasNews && (
            <div className="flex flex-col items-center justify-center min-h-[50vh]">
              <Loader2 size={48} className="animate-spin text-blue-400 mb-4" />
              <p className="text-lg text-gray-300">{pipeline.message}</p>
            </div>
          )}

        {/* === REVIEW / GENERATING / COMPLETE: News Cards === */}
        {hasNews && !showPreview && (
          <div className="space-y-6">
            {/* Intro/Outro Scripts */}
            <div className="bg-gray-800/50 rounded-xl border border-gray-700 p-5">
              <h3 className="text-sm font-bold text-gray-400 uppercase tracking-wider mb-3">
                인트로 / 아웃트로
              </h3>
              <div className="grid grid-cols-2 gap-4">
                <div>
                  <label className="text-xs text-gray-500 mb-1 block">
                    오프닝 나레이션
                  </label>
                  <textarea
                    value={introScript}
                    onChange={(e) => setIntroScript(e.target.value)}
                    className="w-full bg-gray-900 rounded-lg p-3 text-sm text-gray-300 resize-none h-20 border border-gray-700 focus:border-blue-500 focus:outline-none"
                  />
                  {introAudioUrl && (
                    <audio
                      src={introAudioUrl}
                      controls
                      className="w-full h-8 mt-1"
                      style={{ filter: "invert(1)" }}
                    />
                  )}
                </div>
                <div>
                  <label className="text-xs text-gray-500 mb-1 block">
                    클로징 나레이션
                  </label>
                  <textarea
                    value={outroScript}
                    onChange={(e) => setOutroScript(e.target.value)}
                    className="w-full bg-gray-900 rounded-lg p-3 text-sm text-gray-300 resize-none h-20 border border-gray-700 focus:border-blue-500 focus:outline-none"
                  />
                  {outroAudioUrl && (
                    <audio
                      src={outroAudioUrl}
                      controls
                      className="w-full h-8 mt-1"
                      style={{ filter: "invert(1)" }}
                    />
                  )}
                </div>
              </div>
            </div>

            {/* Batch Action Buttons */}
            <div className="flex gap-3 items-center">
              <button
                onClick={handleGenerateAllMedia}
                className="flex items-center gap-2 px-6 py-2.5 bg-gradient-to-r from-blue-600 to-purple-600 hover:from-blue-500 hover:to-purple-500 text-white rounded-lg font-medium shadow-lg shadow-blue-500/20 transition-all"
              >
                <Zap size={18} /> 전체 미디어 일괄 생성
              </button>
              <button
                onClick={handleGenerateAllImages}
                className="flex items-center gap-2 px-4 py-2.5 bg-blue-600/20 hover:bg-blue-600/30 text-blue-400 border border-blue-600/40 rounded-lg text-sm transition-colors"
              >
                <ImagePlus size={16} /> 전체 이미지 생성
              </button>
              <button
                onClick={handleGenerateAllAudio}
                className="flex items-center gap-2 px-4 py-2.5 bg-purple-600/20 hover:bg-purple-600/30 text-purple-400 border border-purple-600/40 rounded-lg text-sm transition-colors"
              >
                <Volume2 size={16} /> 전체 TTS 생성
              </button>
            </div>

            {/* News Cards */}
            <div className="space-y-4">
              {newsItems.map((item) => (
                <NewsCard
                  key={item.id}
                  item={item}
                  onUpdateItem={handleUpdateItem}
                  onGenerateImage={handleGenerateImage}
                  onGenerateAudio={handleGenerateAudio}
                  onGenerateVideo={handleGenerateVideo}
                />
              ))}
            </div>
          </div>
        )}

        {/* === PREVIEW MODE === */}
        {hasNews && showPreview && (
          <div className="max-w-4xl mx-auto">
            <h2 className="text-xl font-bold text-white mb-4 flex items-center gap-2">
              <Eye size={20} className="text-blue-400" />
              콘텐츠 미리보기
            </h2>
            <PreviewPlayer
              newsItems={newsItems}
              introAudioUrl={introAudioUrl}
              outroAudioUrl={outroAudioUrl}
            />
          </div>
        )}
      </main>
    </div>
  );
};

function getPreviousDate(dateStr: string): string {
  const date = new Date(dateStr);
  date.setDate(date.getDate() - 1);
  return date.toISOString().split("T")[0];
}

export default App;
