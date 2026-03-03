import React, { useState, useEffect, useRef, useCallback } from "react";
import { NewsItem } from "../types";
import {
  isBrowserTTS,
  playBrowserTTS,
  stopBrowserTTS,
} from "../services/geminiService";
import {
  Play,
  Pause,
  SkipForward,
  SkipBack,
  Volume2,
  VolumeX,
} from "lucide-react";

interface Props {
  newsItems: NewsItem[];
  introAudioUrl?: string;
  outroAudioUrl?: string;
}

export const PreviewPlayer: React.FC<Props> = ({
  newsItems,
  introAudioUrl,
  outroAudioUrl,
}) => {
  const [currentIndex, setCurrentIndex] = useState(0);
  const [isPlaying, setIsPlaying] = useState(false);
  const [isMuted, setIsMuted] = useState(false);
  const audioRef = useRef<HTMLAudioElement>(null);
  const timerRef = useRef<number | null>(null);

  const validItems = newsItems.filter((n) => n.imageUrl || n.videoUrl);

  const playCurrentAudio = useCallback(() => {
    if (isMuted) return;
    const audioUrl = validItems[currentIndex]?.audioUrl;
    if (!audioUrl) return;

    stopBrowserTTS();
    if (isBrowserTTS(audioUrl)) {
      playBrowserTTS(audioUrl);
    } else if (audioRef.current) {
      audioRef.current.src = audioUrl;
      audioRef.current.play().catch(() => {});
    }
  }, [currentIndex, validItems, isMuted]);

  const goNext = useCallback(() => {
    setCurrentIndex((prev) => {
      if (prev >= validItems.length - 1) {
        setIsPlaying(false);
        return prev;
      }
      return prev + 1;
    });
  }, [validItems.length]);

  const goPrev = useCallback(() => {
    setCurrentIndex((prev) => Math.max(0, prev - 1));
  }, []);

  useEffect(() => {
    if (isPlaying) {
      playCurrentAudio();
      timerRef.current = window.setTimeout(goNext, 8000);
    }
    return () => {
      if (timerRef.current) clearTimeout(timerRef.current);
    };
  }, [isPlaying, currentIndex, playCurrentAudio, goNext]);

  if (validItems.length === 0) {
    return (
      <div className="bg-gray-800 rounded-xl border border-gray-700 p-8 text-center">
        <p className="text-gray-500">
          미리보기할 미디어가 없습니다. 이미지를 먼저 생성해주세요.
        </p>
      </div>
    );
  }

  const current = validItems[currentIndex];

  return (
    <div className="bg-gray-900 rounded-xl border border-gray-700 overflow-hidden">
      <audio ref={audioRef} />

      {/* Display */}
      <div className="relative aspect-video bg-black">
        {current?.videoUrl ? (
          <video
            src={current.videoUrl}
            autoPlay
            muted={isMuted}
            className="w-full h-full object-contain"
          />
        ) : current?.imageUrl ? (
          <img
            src={current.imageUrl}
            alt={current.title}
            className="w-full h-full object-contain"
          />
        ) : null}

        {/* Overlay - Title */}
        <div className="absolute bottom-0 left-0 right-0 bg-gradient-to-t from-black/90 via-black/50 to-transparent p-6">
          <div className="flex items-center gap-3">
            <span className="text-3xl font-black text-red-500">
              {current?.rank}
            </span>
            <div>
              <h3 className="text-xl font-bold text-white drop-shadow-lg">
                {current?.title}
              </h3>
              <p className="text-sm text-gray-300 mt-1 line-clamp-2">
                {current?.narrationScript}
              </p>
            </div>
          </div>
        </div>

        {/* Counter */}
        <div className="absolute top-4 right-4 bg-black/60 text-white text-sm px-3 py-1 rounded-full">
          {currentIndex + 1} / {validItems.length}
        </div>
      </div>

      {/* Controls */}
      <div className="flex items-center justify-center gap-4 p-4 bg-gray-800">
        <button
          onClick={goPrev}
          disabled={currentIndex === 0}
          className="p-2 text-gray-400 hover:text-white disabled:opacity-30 transition-colors"
        >
          <SkipBack size={20} />
        </button>
        <button
          onClick={() => setIsPlaying(!isPlaying)}
          className="p-3 bg-blue-600 hover:bg-blue-500 text-white rounded-full transition-colors"
        >
          {isPlaying ? <Pause size={24} /> : <Play size={24} />}
        </button>
        <button
          onClick={goNext}
          disabled={currentIndex >= validItems.length - 1}
          className="p-2 text-gray-400 hover:text-white disabled:opacity-30 transition-colors"
        >
          <SkipForward size={20} />
        </button>
        <button
          onClick={() => setIsMuted(!isMuted)}
          className="p-2 text-gray-400 hover:text-white transition-colors"
        >
          {isMuted ? <VolumeX size={20} /> : <Volume2 size={20} />}
        </button>
      </div>

      {/* Timeline thumbnails */}
      <div className="flex gap-1 p-3 bg-gray-800/50 overflow-x-auto">
        {validItems.map((item, i) => (
          <button
            key={item.id}
            onClick={() => setCurrentIndex(i)}
            className={`flex-shrink-0 w-20 h-12 rounded-md overflow-hidden border-2 transition-all ${
              i === currentIndex
                ? "border-blue-500 ring-2 ring-blue-500/30"
                : "border-transparent opacity-60 hover:opacity-100"
            }`}
          >
            {item.imageUrl ? (
              <img
                src={item.imageUrl}
                alt={`News ${item.rank}`}
                className="w-full h-full object-cover"
              />
            ) : (
              <div className="w-full h-full bg-gray-700 flex items-center justify-center text-xs text-gray-500">
                {item.rank}
              </div>
            )}
          </button>
        ))}
      </div>
    </div>
  );
};
