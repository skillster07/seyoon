import React from 'react';
import { Eye, EyeOff, Lock, Unlock, ChevronUp, ChevronDown, Trash2, Image as ImageIcon, Type as TypeIcon, Square } from 'lucide-react';
import { Layer } from '../../types';

interface LayerPanelProps {
  layers: Layer[]; // ascending zIndex
  selectedId: string | null;
  onSelect: (id: string) => void;
  onToggleVisible: (id: string) => void;
  onToggleLocked: (id: string) => void;
  onMove: (id: string, dir: 'up' | 'down') => void;
  onDelete: (id: string) => void;
}

const ICON: Record<Layer['type'], React.ReactNode> = {
  background: <Square size={14} />,
  object: <ImageIcon size={14} />,
  text: <TypeIcon size={14} />,
};

export const LayerPanel: React.FC<LayerPanelProps> = ({
  layers,
  selectedId,
  onSelect,
  onToggleVisible,
  onToggleLocked,
  onMove,
  onDelete,
}) => {
  const display = [...layers].sort((a, b) => b.zIndex - a.zIndex); // top first
  return (
    <div className="w-64 bg-gray-900 border-r border-gray-700 flex flex-col">
      <div className="px-3 py-2 border-b border-gray-700 text-xs font-semibold text-gray-300 uppercase tracking-wider">
        레이어 ({layers.length})
      </div>
      <div className="flex-1 overflow-y-auto">
        {display.length === 0 && (
          <div className="p-4 text-xs text-gray-500 text-center">레이어가 없습니다. "분석"을 눌러 객체를 감지하세요.</div>
        )}
        {display.map((l) => {
          const selected = l.id === selectedId;
          return (
            <div
              key={l.id}
              onClick={() => !l.locked && onSelect(l.id)}
              className={`flex items-center gap-2 px-3 py-2 border-b border-gray-800 cursor-pointer ${
                selected ? 'bg-rose-900/30 border-l-2 border-l-rose-500' : 'hover:bg-gray-800'
              } ${l.locked ? 'opacity-70' : ''}`}
            >
              <span className="text-gray-400">{ICON[l.type]}</span>
              <span className="flex-1 text-xs text-gray-200 truncate">{l.label}</span>
              <button
                onClick={(e) => { e.stopPropagation(); onToggleVisible(l.id); }}
                className="text-gray-400 hover:text-white p-1"
                title={l.visible ? '숨기기' : '보이기'}
              >
                {l.visible ? <Eye size={12} /> : <EyeOff size={12} />}
              </button>
              <button
                onClick={(e) => { e.stopPropagation(); onToggleLocked(l.id); }}
                className="text-gray-400 hover:text-white p-1"
                title={l.locked ? '잠금 해제' : '잠금'}
              >
                {l.locked ? <Lock size={12} /> : <Unlock size={12} />}
              </button>
              {l.type !== 'background' && (
                <>
                  <button
                    onClick={(e) => { e.stopPropagation(); onMove(l.id, 'up'); }}
                    className="text-gray-400 hover:text-white p-1"
                    title="위로"
                  >
                    <ChevronUp size={12} />
                  </button>
                  <button
                    onClick={(e) => { e.stopPropagation(); onMove(l.id, 'down'); }}
                    className="text-gray-400 hover:text-white p-1"
                    title="아래로"
                  >
                    <ChevronDown size={12} />
                  </button>
                  <button
                    onClick={(e) => { e.stopPropagation(); onDelete(l.id); }}
                    className="text-gray-400 hover:text-rose-400 p-1"
                    title="삭제"
                  >
                    <Trash2 size={12} />
                  </button>
                </>
              )}
            </div>
          );
        })}
      </div>
    </div>
  );
};

export default LayerPanel;
