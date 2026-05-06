import React, { useState, useCallback } from 'react';
import { Scene } from '../types';
import { Image, RefreshCw, Wand2, Edit3, Save, Film, Mic, Play, ChevronDown, History, Layers } from 'lucide-react';

interface SceneCardProps {
  scene: Scene;
  onGenerateImage: (id: string, prompt: string) => void;
  onUpdatePrompt: (id: string, newPrompt: string) => void;
  onGenerateVideo: (id: string) => void;
  onGenerateAudio: (id: string) => void;
  onUpdateScene: (scene: Scene) => void;
  onOpenMagicLayer?: (id: string) => void;
}

export const SceneCard: React.FC<SceneCardProps> = ({
  scene,
  onGenerateImage,
  onUpdatePrompt,
  onGenerateVideo,
  onGenerateAudio,
  onUpdateScene,
  onOpenMagicLayer,
}) => {
  const [isEditing, setIsEditing] = useState(false);
  const [editedPrompt, setEditedPrompt] = useState(scene.visualPrompt);
  
  // Toggles for version dropdowns
  const [showVideoHistory, setShowVideoHistory] = useState(false);
  const [showAudioHistory, setShowAudioHistory] = useState(false);

  const handleSavePrompt = () => {
    onUpdatePrompt(scene.id, editedPrompt);
    setIsEditing(false);
  };

  const handleCancelEdit = () => {
    setEditedPrompt(scene.visualPrompt);
    setIsEditing(false);
  };

  const handleGenerateClick = useCallback(() => {
    onGenerateImage(scene.id, scene.visualPrompt);
  }, [scene.id, scene.visualPrompt, onGenerateImage]);

  const selectVideoVersion = (url: string) => {
    onUpdateScene({ ...scene, videoUrl: url });
    setShowVideoHistory(false);
  };

  const selectAudioVersion = (url: string) => {
    onUpdateScene({ ...scene, audioUrl: url });
    setShowAudioHistory(false);
  };

  return (
    <div className="bg-gray-800 border border-gray-700 rounded-xl overflow-hidden shadow-lg flex flex-col md:flex-row min-h-[350px]">
      {/* Left: Text & Prompt Control */}
      <div className="flex-1 p-6 flex flex-col justify-between order-2 md:order-1">
        <div>
          <div className="mb-4">
            <h4 className="text-xs font-semibold text-primary-500 uppercase tracking-wider mb-2">원문 스크립트</h4>
            <p className="text-white text-lg font-medium leading-relaxed font-sans">{scene.originalText}</p>
          </div>

          <div className="mt-4">
            <div className="flex items-center justify-between mb-2">
              <h4 className="text-xs font-semibold text-gray-400 uppercase tracking-wider">시각적 프롬프트 (Veo 3.1)</h4>
              {!isEditing && (
                <button 
                  onClick={() => setIsEditing(true)}
                  className="text-gray-400 hover:text-white transition-colors"
                  title="프롬프트 수정"
                >
                  <Edit3 size={14} />
                </button>
              )}
            </div>
            
            {isEditing ? (
              <div className="flex flex-col gap-2">
                <textarea
                  value={editedPrompt}
                  onChange={(e) => setEditedPrompt(e.target.value)}
                  className="w-full bg-gray-900 border border-gray-600 rounded p-3 text-sm text-gray-200 focus:border-primary-500 focus:outline-none h-24 resize-none"
                />
                <div className="flex gap-2 justify-end">
                  <button onClick={handleCancelEdit} className="px-3 py-1 text-xs text-gray-400 hover:text-white">취소</button>
                  <button onClick={handleSavePrompt} className="flex items-center gap-1 px-3 py-1 text-xs bg-primary-600 hover:bg-primary-500 text-white rounded"><Save size={12} /> 저장</button>
                </div>
              </div>
            ) : (
              <p className="text-gray-400 text-xs italic border-l-2 border-gray-600 pl-3 line-clamp-3">
                {scene.visualPrompt}
              </p>
            )}
          </div>
        </div>

        {/* Action Buttons */}
        <div className="mt-6 pt-4 border-t border-gray-700 grid grid-cols-2 lg:grid-cols-4 gap-2">
          {/* Image Gen */}
          <button
            onClick={handleGenerateClick}
            disabled={scene.isGeneratingImage}
            className={`flex items-center justify-center gap-2 px-3 py-2 rounded-lg font-medium text-xs transition-all
              ${scene.isGeneratingImage ? 'bg-gray-700 text-gray-500' : 'bg-gray-700 hover:bg-gray-600 text-white'}`}
          >
            {scene.isGeneratingImage ? <RefreshCw className="animate-spin" size={14}/> : <Image size={14} />}
            {scene.imageUrl ? '이미지 재생성' : '이미지 생성'}
          </button>

          {/* Magic Layer */}
          <button
            onClick={() => scene.imageUrl && onOpenMagicLayer && onOpenMagicLayer(scene.id)}
            disabled={!scene.imageUrl || !onOpenMagicLayer}
            className={`flex items-center justify-center gap-2 px-3 py-2 rounded-lg font-medium text-xs transition-all
              ${!scene.imageUrl ? 'bg-gray-700 text-gray-500 opacity-50' : 'bg-fuchsia-900/50 hover:bg-fuchsia-800 text-fuchsia-200 border border-fuchsia-800'}`}
            title="이미지를 객체별 레이어로 분리해 편집"
          >
            <Layers size={14} />
            매직 레이어
          </button>

          {/* Video Gen with History */}
          <div className="relative group">
            <button
              onClick={() => onGenerateVideo(scene.id)}
              disabled={!scene.imageUrl || scene.isGeneratingVideo}
              className={`w-full h-full flex items-center justify-center gap-2 px-3 py-2 rounded-lg font-medium text-xs transition-all
                ${!scene.imageUrl || scene.isGeneratingVideo ? 'bg-gray-700 text-gray-500 opacity-50' : 'bg-rose-900/50 hover:bg-rose-800 text-rose-200 border border-rose-800'}`}
            >
               {scene.isGeneratingVideo ? <RefreshCw className="animate-spin" size={14}/> : <Film size={14} />}
               {scene.videoUrl ? '영상 재생성' : '영상 생성'}
            </button>
            {scene.videoVersions.length > 1 && (
               <button 
                 onClick={() => setShowVideoHistory(!showVideoHistory)}
                 className="absolute top-0 right-0 bottom-0 px-1 hover:bg-black/20 text-rose-300 rounded-r-lg"
               >
                 <History size={12} />
               </button>
            )}
            
            {showVideoHistory && (
               <div className="absolute top-full left-0 right-0 mt-1 bg-gray-900 border border-gray-700 rounded-lg shadow-xl z-20 overflow-hidden">
                 <div className="p-2 text-[10px] text-gray-400 bg-gray-800 border-b border-gray-700 font-bold">영상 기록</div>
                 {scene.videoVersions.map((v, i) => (
                   <div 
                      key={v.id} 
                      onClick={() => selectVideoVersion(v.url)}
                      className={`p-2 text-xs cursor-pointer hover:bg-gray-800 flex justify-between ${v.url === scene.videoUrl ? 'text-rose-400 font-bold' : 'text-gray-300'}`}
                   >
                     <span>Ver {scene.videoVersions.length - i}</span>
                     <span className="text-[10px] text-gray-500">{new Date(v.timestamp).toLocaleTimeString([], {hour: '2-digit', minute:'2-digit'})}</span>
                   </div>
                 ))}
               </div>
            )}
          </div>

          {/* Audio Gen with History */}
          <div className="col-span-2 lg:col-span-1 relative">
            <button
              onClick={() => onGenerateAudio(scene.id)}
              disabled={scene.isGeneratingAudio}
              className={`w-full h-full flex items-center justify-center gap-2 px-3 py-2 rounded-lg font-medium text-xs transition-all
                ${scene.isGeneratingAudio ? 'bg-gray-700 text-gray-500' : 'bg-indigo-900/50 hover:bg-indigo-800 text-indigo-200 border border-indigo-800'}`}
            >
               {scene.isGeneratingAudio ? <RefreshCw className="animate-spin" size={14}/> : <Mic size={14} />}
               {scene.audioUrl ? '대사 재생성' : '대사 생성'}
            </button>
            {scene.audioVersions.length > 1 && (
               <button 
                 onClick={() => setShowAudioHistory(!showAudioHistory)}
                 className="absolute top-0 right-0 bottom-0 px-1 hover:bg-black/20 text-indigo-300 rounded-r-lg"
               >
                 <History size={12} />
               </button>
            )}

            {showAudioHistory && (
               <div className="absolute top-full left-0 right-0 mt-1 bg-gray-900 border border-gray-700 rounded-lg shadow-xl z-20 overflow-hidden">
                 <div className="p-2 text-[10px] text-gray-400 bg-gray-800 border-b border-gray-700 font-bold">대사 기록</div>
                 {scene.audioVersions.map((v, i) => (
                   <div 
                      key={v.id} 
                      onClick={() => selectAudioVersion(v.url)}
                      className={`p-2 text-xs cursor-pointer hover:bg-gray-800 flex justify-between ${v.url === scene.audioUrl ? 'text-indigo-400 font-bold' : 'text-gray-300'}`}
                   >
                     <span>Ver {scene.audioVersions.length - i}</span>
                     <span className="text-[10px] text-gray-500">{new Date(v.timestamp).toLocaleTimeString([], {hour: '2-digit', minute:'2-digit'})}</span>
                   </div>
                 ))}
               </div>
            )}
          </div>
        </div>
      </div>

      {/* Right: Media Display */}
      <div className="w-full md:w-[480px] bg-black relative flex items-center justify-center order-1 md:order-2 border-b md:border-b-0 md:border-l border-gray-700">
        {scene.videoUrl ? (
          <div className="relative w-full h-full group">
            <video 
              src={scene.videoUrl} 
              controls 
              className="w-full h-full object-cover" 
              loop
            />
            <div className="absolute top-2 left-2 bg-rose-600 text-white text-[10px] font-bold px-2 py-0.5 rounded shadow z-10">
              Veo 3.1
            </div>
            
            {/* Audio Visualizer Overlay */}
            {scene.audioUrl && (
               <div className="absolute bottom-4 left-4 right-4 bg-black/60 backdrop-blur-sm rounded-lg p-2 flex items-center gap-3 z-10 border border-white/10">
                  <button 
                    onClick={() => { const a = new Audio(scene.audioUrl); a.play(); }}
                    className="w-8 h-8 rounded-full bg-indigo-600 flex items-center justify-center hover:bg-indigo-500 transition-colors"
                  >
                    <Play size={12} fill="white" className="ml-0.5"/>
                  </button>
                  <div className="flex-1 flex items-center gap-0.5 h-6">
                     {/* Fake Waveform Animation */}
                     {Array.from({ length: 20 }).map((_, i) => (
                        <div key={i} className="flex-1 bg-indigo-400 rounded-full animate-pulse" style={{ height: `${Math.random() * 80 + 20}%`, animationDelay: `${i * 0.05}s`}}></div>
                     ))}
                  </div>
               </div>
            )}
          </div>
        ) : scene.imageUrl ? (
          <div className="relative w-full h-full group">
            <img src={scene.imageUrl} alt="Generated Scene" className="w-full h-full object-cover" />
            <div className="absolute top-2 right-2 flex gap-1">
               {scene.audioUrl && (
                  <button 
                    onClick={() => { const a = new Audio(scene.audioUrl); a.play(); }}
                    className="bg-indigo-600 text-white p-2 rounded-full hover:bg-indigo-500 transition-colors shadow-lg"
                    title="대사 미리듣기"
                  >
                    <Mic size={16} />
                  </button>
               )}
            </div>
            {scene.audioUrl && (
               <div className="absolute bottom-4 left-1/2 -translate-x-1/2 flex items-center gap-1">
                  <div className="px-3 py-1 bg-black/70 text-indigo-200 text-xs rounded-full border border-indigo-500/30 flex items-center gap-2">
                     <div className="w-2 h-2 bg-indigo-500 rounded-full animate-ping"></div>
                     대사 오디오 생성됨
                  </div>
               </div>
            )}
          </div>
        ) : (
          <div className="text-center p-8">
            <Wand2 className="text-gray-700 mx-auto mb-2" size={32} />
            <p className="text-gray-600 text-xs">이미지 생성 후<br/>영상을 제작할 수 있습니다.</p>
          </div>
        )}
        
        {/* Loading Overlay */}
        {(scene.isGeneratingImage || scene.isGeneratingVideo) && (
          <div className="absolute inset-0 bg-black/80 flex flex-col items-center justify-center z-10 backdrop-blur-sm">
            <div className="w-12 h-12 border-4 border-primary-500/30 border-t-primary-500 rounded-full animate-spin mb-4"></div>
            <p className="text-primary-400 text-sm font-medium animate-pulse">
              {scene.isGeneratingVideo ? 'Veo가 영상을 생성 중입니다...' : '이미지 생성 중...'}
            </p>
          </div>
        )}
      </div>
    </div>
  );
};