import React, { useState } from "react";
import { NewsItem } from "../types";
import {
  isBrowserTTS,
  playBrowserTTS,
  stopBrowserTTS,
} from "../services/geminiService";
import {
  ImagePlus,
  Volume2,
  Video,
  Edit3,
  Check,
  X,
  Loader2,
  ChevronDown,
  ChevronUp,
  Hash,
  Play,
  Square,
} from "lucide-react";

interface Props {
  item: NewsItem;
  onUpdateItem: (updated: NewsItem) => void;
  onGenerateImage: (id: string, prompt: string) => void;
  onGenerateAudio: (id: string, text: string) => void;
  onGenerateVideo: (id: string) => void;
}

export const NewsCard: React.FC<Props> = ({
  item,
  onUpdateItem,
  onGenerateImage,
  onGenerateAudio,
  onGenerateVideo,
}) => {
  const [isEditing, setIsEditing] = useState(false);
  const [editScript, setEditScript] = useState(item.narrationScript);
  const [editPrompt, setEditPrompt] = useState(item.imagePrompt);
  const [showDetails, setShowDetails] = useState(false);
  const [isSpeaking, setIsSpeaking] = useState(false);

  const handleBrowserTTS = () => {
    if (isSpeaking) {
      stopBrowserTTS();
      setIsSpeaking(false);
      return;
    }
    if (item.audioUrl && isBrowserTTS(item.audioUrl)) {
      const utterance = playBrowserTTS(item.audioUrl);
      if (utterance) {
        setIsSpeaking(true);
        utterance.onend = () => setIsSpeaking(false);
        utterance.onerror = () => setIsSpeaking(false);
      }
    }
  };

  const handleSaveEdit = () => {
    onUpdateItem({
      ...item,
      narrationScript: editScript,
      imagePrompt: editPrompt,
    });
    setIsEditing(false);
  };

  const handleCancelEdit = () => {
    setEditScript(item.narrationScript);
    setEditPrompt(item.imagePrompt);
    setIsEditing(false);
  };

  const rankColors: Record<number, string> = {
    1: "from-red-600 to-orange-500",
    2: "from-orange-500 to-amber-500",
    3: "from-amber-500 to-yellow-500",
    4: "from-blue-500 to-cyan-500",
    5: "from-cyan-500 to-teal-500",
  };

  return (
    <div className="bg-gray-800 rounded-xl border border-gray-700 overflow-hidden hover:border-gray-600 transition-colors">
      {/* Header */}
      <div className="flex items-start gap-4 p-5">
        <div
          className={`flex-shrink-0 w-12 h-12 rounded-xl bg-gradient-to-br ${
            rankColors[item.rank] || "from-gray-600 to-gray-500"
          } flex items-center justify-center text-white font-bold text-xl shadow-lg`}
        >
          {item.rank}
        </div>

        <div className="flex-1 min-w-0">
          <h3 className="text-lg font-bold text-white mb-1 leading-tight">
            {item.title}
          </h3>
          <p className="text-sm text-gray-400 leading-relaxed">
            {item.summary}
          </p>
          <div className="flex flex-wrap gap-1.5 mt-2">
            {item.keywords.map((kw, i) => (
              <span
                key={i}
                className="inline-flex items-center gap-0.5 text-xs bg-gray-700 text-gray-300 px-2 py-0.5 rounded-full"
              >
                <Hash size={10} />
                {kw}
              </span>
            ))}
          </div>
          {item.source && (
            <span className="text-xs text-gray-500 mt-1 block">
              출처: {item.source}
            </span>
          )}
        </div>
      </div>

      {/* Media Row */}
      <div className="flex gap-4 px-5 pb-4">
        {/* Image Preview */}
        <div className="w-64 h-36 flex-shrink-0 rounded-lg overflow-hidden bg-gray-900 border border-gray-700 relative">
          {item.imageUrl ? (
            <img
              src={item.imageUrl}
              alt={item.title}
              className="w-full h-full object-cover"
            />
          ) : (
            <div className="w-full h-full flex items-center justify-center text-gray-600">
              <ImagePlus size={32} />
            </div>
          )}
          {item.isGeneratingImage && (
            <div className="absolute inset-0 bg-black/60 flex items-center justify-center">
              <Loader2 size={24} className="animate-spin text-blue-400" />
            </div>
          )}
        </div>

        {/* Controls */}
        <div className="flex-1 flex flex-col justify-between">
          {/* Narration Script */}
          {isEditing ? (
            <div className="space-y-2">
              <label className="text-xs font-medium text-gray-400">
                나레이션 대본
              </label>
              <textarea
                value={editScript}
                onChange={(e) => setEditScript(e.target.value)}
                className="w-full h-16 bg-gray-900 rounded-lg p-2 text-sm text-white resize-none border border-gray-600 focus:border-blue-500 focus:outline-none"
              />
              <label className="text-xs font-medium text-gray-400">
                이미지 프롬프트
              </label>
              <textarea
                value={editPrompt}
                onChange={(e) => setEditPrompt(e.target.value)}
                className="w-full h-12 bg-gray-900 rounded-lg p-2 text-xs text-gray-300 resize-none border border-gray-600 focus:border-blue-500 focus:outline-none font-mono"
              />
              <div className="flex gap-2">
                <button
                  onClick={handleSaveEdit}
                  className="px-3 py-1 bg-green-600 hover:bg-green-500 text-white rounded-lg text-xs flex items-center gap-1"
                >
                  <Check size={12} /> 저장
                </button>
                <button
                  onClick={handleCancelEdit}
                  className="px-3 py-1 bg-gray-700 hover:bg-gray-600 text-gray-300 rounded-lg text-xs flex items-center gap-1"
                >
                  <X size={12} /> 취소
                </button>
              </div>
            </div>
          ) : (
            <div>
              <p className="text-sm text-gray-300 leading-relaxed mb-1">
                {item.narrationScript}
              </p>
              <button
                onClick={() => setShowDetails(!showDetails)}
                className="text-xs text-gray-500 hover:text-gray-400 flex items-center gap-1"
              >
                {showDetails ? (
                  <ChevronUp size={12} />
                ) : (
                  <ChevronDown size={12} />
                )}
                이미지 프롬프트
              </button>
              {showDetails && (
                <p className="text-xs text-gray-500 mt-1 font-mono bg-gray-900 rounded p-2">
                  {item.imagePrompt}
                </p>
              )}
            </div>
          )}

          {/* Audio Preview */}
          {item.audioUrl &&
            (isBrowserTTS(item.audioUrl) ? (
              <button
                onClick={handleBrowserTTS}
                className={`w-full mt-2 flex items-center justify-center gap-2 px-3 py-2 rounded-lg text-sm transition-colors ${
                  isSpeaking
                    ? "bg-red-600/20 text-red-400 border border-red-600/40"
                    : "bg-purple-600/20 text-purple-400 border border-purple-600/40 hover:bg-purple-600/30"
                }`}
              >
                {isSpeaking ? (
                  <>
                    <Square size={14} /> 정지
                  </>
                ) : (
                  <>
                    <Play size={14} /> 브라우저 TTS 재생
                  </>
                )}
              </button>
            ) : (
              <audio
                src={item.audioUrl}
                controls
                className="w-full h-8 mt-2"
                style={{ filter: "invert(1)" }}
              />
            ))}

          {/* Video Preview */}
          {item.videoUrl && (
            <video
              src={item.videoUrl}
              controls
              className="w-full h-20 mt-2 rounded-lg"
            />
          )}

          {/* Action Buttons */}
          <div className="flex gap-2 mt-3">
            <button
              onClick={() => setIsEditing(true)}
              disabled={isEditing}
              className="px-3 py-1.5 bg-gray-700 hover:bg-gray-600 text-gray-300 rounded-lg text-xs flex items-center gap-1 disabled:opacity-50 transition-colors"
            >
              <Edit3 size={12} /> 편집
            </button>
            <button
              onClick={() => onGenerateImage(item.id, item.imagePrompt)}
              disabled={item.isGeneratingImage}
              className="px-3 py-1.5 bg-blue-600/20 hover:bg-blue-600/30 text-blue-400 border border-blue-600/40 rounded-lg text-xs flex items-center gap-1 disabled:opacity-50 transition-colors"
            >
              {item.isGeneratingImage ? (
                <Loader2 size={12} className="animate-spin" />
              ) : (
                <ImagePlus size={12} />
              )}
              이미지
            </button>
            <button
              onClick={() => onGenerateAudio(item.id, item.narrationScript)}
              disabled={item.isGeneratingAudio}
              className="px-3 py-1.5 bg-purple-600/20 hover:bg-purple-600/30 text-purple-400 border border-purple-600/40 rounded-lg text-xs flex items-center gap-1 disabled:opacity-50 transition-colors"
            >
              {item.isGeneratingAudio ? (
                <Loader2 size={12} className="animate-spin" />
              ) : (
                <Volume2 size={12} />
              )}
              TTS
            </button>
            <button
              onClick={() => onGenerateVideo(item.id)}
              disabled={item.isGeneratingVideo || !item.imageUrl}
              className="px-3 py-1.5 bg-green-600/20 hover:bg-green-600/30 text-green-400 border border-green-600/40 rounded-lg text-xs flex items-center gap-1 disabled:opacity-50 transition-colors"
              title={!item.imageUrl ? "이미지를 먼저 생성하세요" : ""}
            >
              {item.isGeneratingVideo ? (
                <Loader2 size={12} className="animate-spin" />
              ) : (
                <Video size={12} />
              )}
              영상
            </button>
          </div>

          {item.error && (
            <p className="text-xs text-red-400 mt-2">{item.error}</p>
          )}
        </div>
      </div>
    </div>
  );
};
