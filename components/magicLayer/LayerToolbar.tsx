import React from 'react';
import { Wand2, Type as TypeIcon, RefreshCw, Save, X } from 'lucide-react';

interface LayerToolbarProps {
  onAnalyze: () => void;
  onAddText: () => void;
  onSave: () => void;
  onClose: () => void;
  isAnalyzing: boolean;
  isSaving: boolean;
  cutoutEnabled: boolean;
  onToggleCutout: (enabled: boolean) => void;
  progressLabel?: string;
}

export const LayerToolbar: React.FC<LayerToolbarProps> = ({
  onAnalyze,
  onAddText,
  onSave,
  onClose,
  isAnalyzing,
  isSaving,
  cutoutEnabled,
  onToggleCutout,
  progressLabel,
}) => {
  return (
    <div className="flex items-center gap-2 px-4 py-2 bg-gray-900 border-b border-gray-700">
      <button
        onClick={onAnalyze}
        disabled={isAnalyzing}
        className={`flex items-center gap-2 px-3 py-1.5 rounded text-xs font-medium transition-colors ${
          isAnalyzing ? 'bg-gray-700 text-gray-500' : 'bg-rose-600 hover:bg-rose-500 text-white'
        }`}
        title="이미지에서 객체와 텍스트 감지"
      >
        {isAnalyzing ? <RefreshCw className="animate-spin" size={14} /> : <Wand2 size={14} />}
        {isAnalyzing ? '분석 중...' : '분석'}
      </button>

      <button
        onClick={onAddText}
        className="flex items-center gap-2 px-3 py-1.5 rounded text-xs font-medium bg-gray-700 hover:bg-gray-600 text-white"
      >
        <TypeIcon size={14} />
        텍스트 추가
      </button>

      <label className="flex items-center gap-2 ml-2 text-xs text-gray-300 cursor-pointer">
        <input
          type="checkbox"
          checked={cutoutEnabled}
          onChange={(e) => onToggleCutout(e.target.checked)}
          className="accent-rose-500"
        />
        진짜 컷아웃 (마스크 적용, 느림)
      </label>

      {progressLabel && (
        <span className="text-xs text-gray-400 ml-2 truncate max-w-[260px]">{progressLabel}</span>
      )}

      <div className="flex-1" />

      <button
        onClick={onSave}
        disabled={isSaving}
        className={`flex items-center gap-2 px-3 py-1.5 rounded text-xs font-medium ${
          isSaving ? 'bg-gray-700 text-gray-500' : 'bg-emerald-600 hover:bg-emerald-500 text-white'
        }`}
      >
        {isSaving ? <RefreshCw className="animate-spin" size={14} /> : <Save size={14} />}
        저장 후 닫기
      </button>

      <button
        onClick={onClose}
        className="flex items-center gap-1 px-3 py-1.5 rounded text-xs font-medium bg-gray-700 hover:bg-gray-600 text-gray-200"
      >
        <X size={14} />
        취소
      </button>
    </div>
  );
};

export default LayerToolbar;
