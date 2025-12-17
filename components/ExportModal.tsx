import React from 'react';
import { ExportProgress } from '../services/videoExporter';
import { Download, Loader2, CheckCircle, XCircle, Film, Music, Clapperboard } from 'lucide-react';

interface ExportModalProps {
  isOpen: boolean;
  progress: ExportProgress | null;
  onClose: () => void;
  onDownload?: () => void;
  downloadUrl?: string;
}

export const ExportModal: React.FC<ExportModalProps> = ({
  isOpen,
  progress,
  onClose,
  onDownload,
  downloadUrl
}) => {
  if (!isOpen) return null;

  const getStageIcon = () => {
    switch (progress?.stage) {
      case 'loading':
        return <Clapperboard className="text-blue-400" size={48} />;
      case 'processing':
        return <Film className="text-yellow-400" size={48} />;
      case 'encoding':
        return <Music className="text-purple-400" size={48} />;
      case 'complete':
        return <CheckCircle className="text-green-400" size={48} />;
      case 'error':
        return <XCircle className="text-red-400" size={48} />;
      default:
        return <Loader2 className="text-gray-400 animate-spin" size={48} />;
    }
  };

  const getStageColor = () => {
    switch (progress?.stage) {
      case 'loading':
        return 'bg-blue-500';
      case 'processing':
        return 'bg-yellow-500';
      case 'encoding':
        return 'bg-purple-500';
      case 'complete':
        return 'bg-green-500';
      case 'error':
        return 'bg-red-500';
      default:
        return 'bg-gray-500';
    }
  };

  const getStageLabel = () => {
    switch (progress?.stage) {
      case 'loading':
        return 'FFmpeg 초기화';
      case 'processing':
        return '미디어 처리';
      case 'encoding':
        return '영상 인코딩';
      case 'complete':
        return '완료';
      case 'error':
        return '오류 발생';
      default:
        return '준비 중';
    }
  };

  return (
    <div className="fixed inset-0 bg-black/80 backdrop-blur-sm flex items-center justify-center z-[100]">
      <div className="bg-gray-900 rounded-2xl border border-gray-700 w-full max-w-md mx-4 overflow-hidden shadow-2xl">
        {/* Header */}
        <div className="bg-gradient-to-r from-primary-600 to-rose-600 px-6 py-4">
          <h2 className="text-xl font-bold text-white flex items-center gap-2">
            <Download size={24} />
            MP4 내보내기
          </h2>
        </div>

        {/* Content */}
        <div className="p-6">
          {/* Icon */}
          <div className="flex justify-center mb-6">
            <div className={`p-4 rounded-full ${progress?.stage === 'complete' ? 'bg-green-900/30' : progress?.stage === 'error' ? 'bg-red-900/30' : 'bg-gray-800'}`}>
              {getStageIcon()}
            </div>
          </div>

          {/* Stage Label */}
          <div className="text-center mb-4">
            <span className={`inline-block px-3 py-1 rounded-full text-sm font-medium ${getStageColor()} text-white`}>
              {getStageLabel()}
            </span>
          </div>

          {/* Progress Bar */}
          {progress?.stage !== 'complete' && progress?.stage !== 'error' && (
            <div className="mb-4">
              <div className="h-3 bg-gray-800 rounded-full overflow-hidden">
                <div
                  className={`h-full transition-all duration-300 ${getStageColor()}`}
                  style={{ width: `${progress?.progress || 0}%` }}
                />
              </div>
              <div className="flex justify-between mt-2 text-xs text-gray-400">
                <span>{progress?.progress || 0}%</span>
                {progress?.currentScene && progress?.totalScenes && (
                  <span>장면 {progress.currentScene}/{progress.totalScenes}</span>
                )}
              </div>
            </div>
          )}

          {/* Message */}
          <p className="text-center text-gray-300 mb-6">
            {progress?.message || '내보내기 준비 중...'}
          </p>

          {/* Processing Animation */}
          {progress?.stage !== 'complete' && progress?.stage !== 'error' && (
            <div className="flex justify-center gap-1 mb-6">
              {[0, 1, 2, 3, 4].map((i) => (
                <div
                  key={i}
                  className={`w-2 h-8 ${getStageColor()} rounded-full animate-pulse`}
                  style={{ animationDelay: `${i * 0.1}s` }}
                />
              ))}
            </div>
          )}

          {/* Complete State */}
          {progress?.stage === 'complete' && downloadUrl && (
            <div className="space-y-3">
              <button
                onClick={onDownload}
                className="w-full py-3 bg-green-600 hover:bg-green-500 text-white rounded-lg font-bold flex items-center justify-center gap-2 transition-colors"
              >
                <Download size={20} />
                MP4 다운로드
              </button>
              <button
                onClick={onClose}
                className="w-full py-2 bg-gray-800 hover:bg-gray-700 text-gray-300 rounded-lg font-medium transition-colors"
              >
                닫기
              </button>
            </div>
          )}

          {/* Error State */}
          {progress?.stage === 'error' && (
            <button
              onClick={onClose}
              className="w-full py-3 bg-gray-800 hover:bg-gray-700 text-gray-300 rounded-lg font-medium transition-colors"
            >
              닫기
            </button>
          )}

          {/* Cancel Button (during processing) */}
          {progress?.stage !== 'complete' && progress?.stage !== 'error' && (
            <p className="text-center text-xs text-gray-500 mt-4">
              브라우저에서 영상을 처리 중입니다. 잠시만 기다려주세요.
            </p>
          )}
        </div>

        {/* Footer Info */}
        <div className="bg-gray-800/50 px-6 py-3 border-t border-gray-700">
          <div className="flex items-center justify-between text-xs text-gray-500">
            <span>FFmpeg.wasm 기반</span>
            <span>1280x720 HD</span>
          </div>
        </div>
      </div>
    </div>
  );
};
