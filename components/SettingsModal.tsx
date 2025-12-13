import React from 'react';
import { Settings, X, Key, Save, Trash2, ExternalLink } from 'lucide-react';
import { UserSettings } from '../types';

interface SettingsModalProps {
  isOpen: boolean;
  onClose: () => void;
  settings: UserSettings;
  onSaveSettings: (settings: UserSettings) => void;
  hasVeoKey: boolean;
  onSelectVeoKey: () => void;
  onClearAllData: () => void;
}

export const SettingsModal: React.FC<SettingsModalProps> = ({
  isOpen,
  onClose,
  settings,
  onSaveSettings,
  hasVeoKey,
  onSelectVeoKey,
  onClearAllData
}) => {
  if (!isOpen) return null;

  const handleChange = (key: keyof UserSettings, value: any) => {
    onSaveSettings({ ...settings, [key]: value });
  };

  return (
    <div className="fixed inset-0 bg-black/70 backdrop-blur-sm z-50 flex items-center justify-center p-4">
      <div className="bg-gray-800 border border-gray-700 rounded-2xl w-full max-w-md shadow-2xl animate-in fade-in zoom-in-95 duration-200 max-h-[90vh] overflow-y-auto">
        <div className="flex justify-between items-center p-6 border-b border-gray-700 sticky top-0 bg-gray-800 z-10">
          <h2 className="text-xl font-bold text-white flex items-center gap-2">
            <Settings size={20} className="text-primary-500" /> 설정
          </h2>
          <button onClick={onClose} className="text-gray-400 hover:text-white">
            <X size={20} />
          </button>
        </div>

        <div className="p-6 space-y-6">
          {/* API Key Section */}
          <div className="space-y-3">
            <h3 className="text-sm font-semibold text-gray-400 uppercase tracking-wider">API 키 관리</h3>
            
            {/* Veo Key */}
            <div className="bg-gray-900 rounded-xl p-4 border border-gray-700">
               <div className="flex justify-between items-center mb-2">
                 <span className="text-sm font-medium text-white">Veo (Video Generation)</span>
                 <span className={`text-[10px] px-2 py-0.5 rounded-full ${hasVeoKey ? 'bg-green-900 text-green-300' : 'bg-gray-700 text-gray-400'}`}>
                   {hasVeoKey ? '활성화됨' : '미설정'}
                 </span>
               </div>
               <p className="text-xs text-gray-500 mb-3">
                 Google Cloud 결제 계정 API 키가 필요합니다.
               </p>
               <button 
                 onClick={onSelectVeoKey}
                 className="w-full flex items-center justify-center gap-2 py-2 bg-indigo-900/50 hover:bg-indigo-800 text-indigo-200 border border-indigo-700 rounded-lg text-sm transition-colors"
               >
                 <Key size={14} /> {hasVeoKey ? 'API Key 변경' : 'API Key 선택'}
               </button>
            </div>

            {/* Supertone Key */}
            <div className="bg-gray-900 rounded-xl p-4 border border-gray-700">
               <div className="flex justify-between items-center mb-2">
                 <span className="text-sm font-medium text-white">Supertone API (Voice)</span>
                 <a href="https://console.supertoneapi.com/dashboard" target="_blank" rel="noopener noreferrer" className="text-[10px] text-indigo-400 hover:text-indigo-300 flex items-center gap-1">
                   대시보드 <ExternalLink size={10} />
                 </a>
               </div>
               <input 
                 type="password" 
                 value={settings.supertoneApiKey || ''}
                 onChange={(e) => handleChange('supertoneApiKey', e.target.value)}
                 placeholder="API Key 입력"
                 className="w-full bg-gray-800 border border-gray-600 rounded p-2 text-xs text-white focus:border-primary-500 focus:outline-none mb-2 placeholder-gray-600"
               />
               <p className="text-[10px] text-gray-500">
                 * 설정 시 Gemini TTS 대신 Supertone 음성을 사용합니다.
               </p>
            </div>
          </div>

          {/* Editor Settings */}
          <div className="space-y-3">
            <h3 className="text-sm font-semibold text-gray-400 uppercase tracking-wider">에디터 설정</h3>
            <div className="space-y-4">
               <div className="flex items-center justify-between">
                  <span className="text-sm text-gray-300">새 장면 기본 길이</span>
                  <div className="flex items-center gap-2">
                    <input 
                      type="number" 
                      min="1" 
                      max="30"
                      value={settings.defaultDuration}
                      onChange={(e) => handleChange('defaultDuration', parseInt(e.target.value) || 5)}
                      className="w-16 bg-gray-900 border border-gray-600 rounded p-1 text-center text-sm text-white"
                    />
                    <span className="text-xs text-gray-500">초</span>
                  </div>
               </div>

               <div className="flex items-center justify-between">
                  <span className="text-sm text-gray-300">프로젝트 자동 저장</span>
                  <label className="relative inline-flex items-center cursor-pointer">
                    <input 
                      type="checkbox" 
                      checked={settings.autoSave} 
                      onChange={(e) => handleChange('autoSave', e.target.checked)}
                      className="sr-only peer" 
                    />
                    <div className="w-11 h-6 bg-gray-700 peer-focus:outline-none rounded-full peer peer-checked:after:translate-x-full peer-checked:after:border-white after:content-[''] after:absolute after:top-[2px] after:left-[2px] after:bg-white after:border-gray-300 after:border after:rounded-full after:h-5 after:w-5 after:transition-all peer-checked:bg-primary-600"></div>
                  </label>
               </div>
            </div>
          </div>

          {/* Danger Zone */}
          <div className="pt-4 border-t border-gray-700">
             <button 
               onClick={() => {
                 if (window.confirm("정말로 모든 데이터를 초기화하시겠습니까? 모든 프로젝트와 설정이 삭제됩니다.")) {
                   onClearAllData();
                   onClose();
                 }
               }}
               className="w-full flex items-center justify-center gap-2 py-3 bg-red-900/30 hover:bg-red-900/50 text-red-400 border border-red-900/50 rounded-lg text-sm transition-colors"
             >
               <Trash2 size={16} /> 모든 데이터 초기화
             </button>
          </div>
        </div>
      </div>
    </div>
  );
};