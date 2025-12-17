import React, { useState, useRef, useEffect } from 'react';
import { Scene, MediaVersion, TransitionType } from '../types';
import { Play, Pause, Music, Download, Layers, Film, Mic, Clock, RefreshCw, Volume2, History, Link, Wand2, Plus, ZoomIn, ZoomOut, MonitorPlay, FastForward, FileVideo } from 'lucide-react';
import { generateBgm } from '../services/geminiService';
import { videoExporter, ExportProgress } from '../services/videoExporter';
import { ExportModal } from './ExportModal';

interface TimelineEditorProps {
  scenes: Scene[];
  setScenes: React.Dispatch<React.SetStateAction<Scene[]>>;
  bgmUrl?: string;
  setBgmUrl: (url: string) => void;
  bgmVersions: MediaVersion[];
  onAddBgmVersion: (url: string) => void;
  onExtendVideo?: (id: string) => void;
}

export const TimelineEditor: React.FC<TimelineEditorProps> = ({ 
  scenes, 
  setScenes,
  bgmUrl, 
  setBgmUrl,
  bgmVersions,
  onAddBgmVersion,
  onExtendVideo
}) => {
  const [isPlaying, setIsPlaying] = useState(false);
  const [currentSceneIndex, setCurrentSceneIndex] = useState(0);
  const [isGeneratingBgm, setIsGeneratingBgm] = useState(false);
  const [bgmMood, setBgmMood] = useState("Romantic piano, sentimental, soft strings");
  const [showBgmHistory, setShowBgmHistory] = useState(false);
  const [timelineZoom, setTimelineZoom] = useState(1); // Zoom level 0.5 to 2.0
  const [isExporting, setIsExporting] = useState(false);
  const [exportProgress, setExportProgress] = useState<ExportProgress | null>(null);
  const [exportedVideoUrl, setExportedVideoUrl] = useState<string | null>(null);
  const [showExportModal, setShowExportModal] = useState(false);

  // References for Playback
  const videoRefs = useRef<{ [key: string]: HTMLVideoElement | null }>({});
  const audioRef = useRef<HTMLAudioElement>(null); // For dialogue
  const bgmRef = useRef<HTMLAudioElement>(null); // For BGM
  const timelineInterval = useRef<ReturnType<typeof setTimeout> | null>(null);

  // References for Export
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const mediaRecorderRef = useRef<MediaRecorder | null>(null);
  const recordedChunksRef = useRef<Blob[]>([]);

  // Set BGM volume
  useEffect(() => {
    if (bgmRef.current) {
      bgmRef.current.volume = 0.3; // Lower background music
    }
  }, [bgmUrl]);

  useEffect(() => {
    if (isPlaying) {
      playSequence();
    } else {
      pauseAll();
    }
    return () => clearTimeline();
  }, [isPlaying, currentSceneIndex]);

  const clearTimeline = () => {
    if (timelineInterval.current) {
      clearTimeout(timelineInterval.current);
      timelineInterval.current = null;
    }
  };

  const pauseAll = () => {
    clearTimeline();
    // Pause all video elements
    Object.values(videoRefs.current).forEach(v => v?.pause());
    if (audioRef.current) audioRef.current.pause();
    if (bgmRef.current) bgmRef.current.pause();
  };

  const playSequence = () => {
    clearTimeline();
    const currentScene = scenes[currentSceneIndex];
    if (!currentScene) {
        setIsPlaying(false);
        return;
    }

    // --- Double Buffering Logic ---
    // 1. Play Current Video/Image
    const currentVid = videoRefs.current[currentScene.id];
    if (currentVid) {
      currentVid.currentTime = 0;
      currentVid.style.opacity = '1';
      currentVid.style.zIndex = '10';
      currentVid.play().catch(() => {});
    }

    // 2. Hide Others (or fade out previous)
    Object.keys(videoRefs.current).forEach(id => {
        if (id !== currentScene.id && videoRefs.current[id]) {
            videoRefs.current[id]!.style.zIndex = '0';
            // If transition is fade, we might keep it visible for a moment, but for now simple cut/fade logic handled by CSS
            videoRefs.current[id]!.style.opacity = '0';
            videoRefs.current[id]!.pause();
        }
    });

    // 3. Play Dialogue
    if (currentScene.audioUrl && audioRef.current) {
      audioRef.current.src = currentScene.audioUrl;
      audioRef.current.play().catch(() => {});
    }

    // 4. Play BGM
    if (bgmUrl && bgmRef.current && bgmRef.current.paused) {
      bgmRef.current.play().catch(() => {});
    }

    // 5. Preload Next Video (Double Buffer)
    const nextScene = scenes[currentSceneIndex + 1];
    if (nextScene && videoRefs.current[nextScene.id]) {
        videoRefs.current[nextScene.id]!.load();
    }

    // 6. Schedule Next
    const durationMs = currentScene.duration * 1000;
    timelineInterval.current = setTimeout(() => {
      handleNextScene();
    }, durationMs);
  };

  const handleNextScene = () => {
    if (currentSceneIndex < scenes.length - 1) {
      setCurrentSceneIndex(prev => prev + 1);
    } else {
      setIsPlaying(false);
      setCurrentSceneIndex(0);
      pauseAll();
      // If we were exporting, stop recording
      if (mediaRecorderRef.current && mediaRecorderRef.current.state === 'recording') {
         mediaRecorderRef.current.stop();
      }
    }
  };

  const handleGenerateBgm = async () => {
    setIsGeneratingBgm(true);
    try {
      const url = await generateBgm(bgmMood);
      onAddBgmVersion(url);
    } catch (e) {
      alert("BGM 생성 실패");
    } finally {
      setIsGeneratingBgm(false);
    }
  };

  const updateSceneDuration = (idx: number, newDuration: number) => {
    const newScenes = [...scenes];
    newScenes[idx].duration = newDuration;
    setScenes(newScenes);
  };

  const updateSceneTransition = (idx: number, type: TransitionType) => {
    const newScenes = [...scenes];
    newScenes[idx].transition = type;
    setScenes(newScenes);
  };

  const syncDurationToAudio = (idx: number) => {
    const scene = scenes[idx];
    if (scene.audioUrl) {
       const tempAudio = new Audio(scene.audioUrl);
       tempAudio.preload = 'metadata';
       tempAudio.onloadedmetadata = () => {
          const duration = Math.ceil(tempAudio.duration);
          if (isFinite(duration) && duration > 0) {
             updateSceneDuration(idx, duration);
          } else {
             updateSceneDuration(idx, 5); 
          }
       };
       tempAudio.load();
    } else {
      alert("동기화할 오디오(대사)가 없습니다.");
    }
  };

  const handleAutoSyncAll = () => {
     scenes.forEach((scene, idx) => {
        if (scene.audioUrl) {
           syncDurationToAudio(idx);
        }
     });
     alert("모든 장면의 길이가 대사 길이에 맞춰 조정되었습니다.");
  };

  // --- MP4 Export Logic using FFmpeg.wasm ---
  const handleExportMp4 = async () => {
    if (scenes.length === 0) {
      alert('내보낼 장면이 없습니다.');
      return;
    }

    // Check if there are any valid scenes with media
    const validScenes = scenes.filter(s => s.imageUrl || s.videoUrl);
    if (validScenes.length === 0) {
      alert('이미지 또는 영상이 생성된 장면이 없습니다. 먼저 장면의 미디어를 생성해주세요.');
      return;
    }

    setIsExporting(true);
    setShowExportModal(true);
    setExportProgress(null);
    setExportedVideoUrl(null);

    try {
      const blob = await videoExporter.exportToMp4(
        scenes,
        bgmUrl,
        (progress) => setExportProgress(progress)
      );

      const url = URL.createObjectURL(blob);
      setExportedVideoUrl(url);
      setExportProgress({
        stage: 'complete',
        progress: 100,
        message: '내보내기 완료! 다운로드 버튼을 클릭하세요.'
      });
    } catch (error: any) {
      console.error('Export failed:', error);
      setExportProgress({
        stage: 'error',
        progress: 0,
        message: error.message || '내보내기 중 오류가 발생했습니다.'
      });
    } finally {
      setIsExporting(false);
    }
  };

  const handleDownloadExport = () => {
    if (!exportedVideoUrl) return;

    const a = document.createElement('a');
    a.href = exportedVideoUrl;
    a.download = `red_garden_episode_${Date.now()}.mp4`;
    document.body.appendChild(a);
    a.click();
    document.body.removeChild(a);
  };

  const handleCloseExportModal = () => {
    setShowExportModal(false);
    if (exportedVideoUrl) {
      URL.revokeObjectURL(exportedVideoUrl);
      setExportedVideoUrl(null);
    }
    setExportProgress(null);
  };

  // --- Legacy WebM Export (Real-time recording) ---
  const handleExportWebm = async () => {
    if (!canvasRef.current) return;
    setIsExporting(true);
    setCurrentSceneIndex(0);
    recordedChunksRef.current = [];

    const stream = canvasRef.current.captureStream(30);
    const mediaRecorder = new MediaRecorder(stream, { mimeType: 'video/webm; codecs=vp9' });

    mediaRecorder.ondataavailable = (event) => {
      if (event.data.size > 0) {
        recordedChunksRef.current.push(event.data);
      }
    };

    mediaRecorder.onstop = () => {
      const blob = new Blob(recordedChunksRef.current, { type: 'video/webm' });
      const url = URL.createObjectURL(blob);
      const a = document.createElement('a');
      a.href = url;
      a.download = `red_garden_episode_${Date.now()}.webm`;
      document.body.appendChild(a);
      a.click();
      document.body.removeChild(a);
      setIsExporting(false);
      setIsPlaying(false);
      alert("내보내기 완료! (.webm 형식)");
    };

    mediaRecorderRef.current = mediaRecorder;
    mediaRecorder.start();
    setIsPlaying(true);
  };

  // Canvas Drawing Loop for Export/Preview
  useEffect(() => {
    let animationFrameId: number;
    const canvas = canvasRef.current;
    if (!canvas) return;
    const ctx = canvas.getContext('2d');
    if (!ctx) return;

    const render = () => {
        // Draw the current scene's video or image to the canvas
        const currentScene = scenes[currentSceneIndex];
        if (currentScene) {
            const vid = videoRefs.current[currentScene.id];
            
            // Draw background (black)
            ctx.fillStyle = 'black';
            ctx.fillRect(0, 0, canvas.width, canvas.height);

            // Draw content
            if (vid && currentScene.videoUrl) {
                // If video is playing, draw it
                ctx.drawImage(vid, 0, 0, canvas.width, canvas.height);
            } else if (currentScene.imageUrl) {
                const img = new Image();
                img.src = currentScene.imageUrl;
                // Note: Loading image every frame is bad, but for cached images likely fine
                // For better performance we would pre-load images
                ctx.drawImage(img, 0, 0, canvas.width, canvas.height);
            }
        }
        animationFrameId = requestAnimationFrame(render);
    };
    render();

    return () => cancelAnimationFrame(animationFrameId);
  }, [currentSceneIndex, scenes]);

  const currentScene = scenes[currentSceneIndex];

  return (
    <div className="max-w-6xl mx-auto animate-in fade-in duration-500 pb-20">
      {/* Export Modal */}
      <ExportModal
        isOpen={showExportModal}
        progress={exportProgress}
        onClose={handleCloseExportModal}
        onDownload={handleDownloadExport}
        downloadUrl={exportedVideoUrl || undefined}
      />

      <div className="text-center mb-8">
        <h2 className="text-3xl font-bold text-white mb-2 flex items-center justify-center gap-3">
          <Layers className="text-primary-500" /> 타임라인 편집 & 미리보기
        </h2>
        <p className="text-gray-400">영상과 대사의 싱크를 맞추고 프로젝트를 완성하세요.</p>
      </div>

      {/* Main Preview Player with Double Buffering & Canvas for Export */}
      <div className="bg-black rounded-xl overflow-hidden aspect-video relative border border-gray-700 shadow-2xl mb-6 mx-auto max-w-4xl group">
         {/* Hidden Canvas for Recording */}
         <canvas ref={canvasRef} width={1280} height={720} className="absolute inset-0 w-full h-full pointer-events-none opacity-0" />

         {/* Visible Video Elements (Double Buffer) */}
         {scenes.map((scene) => (
             <div key={scene.id} 
                  className={`absolute inset-0 w-full h-full transition-all duration-1000 bg-black
                    ${scene.id === currentScene?.id ? 'opacity-100 z-10' : 'opacity-0 z-0'}
                    ${scene.transition === 'zoom' && scene.id === currentScene?.id ? 'animate-zoom-in' : ''}
                  `}
             >
                {scene.videoUrl ? (
                    <video
                        ref={(el) => { videoRefs.current[scene.id] = el; }}
                        src={scene.videoUrl}
                        className="w-full h-full object-contain"
                        muted
                        playsInline
                        loop // Loop fallback if extension not used
                    />
                ) : (
                    <img src={scene.imageUrl} className="w-full h-full object-contain" />
                )}
             </div>
         ))}
         
         {!currentScene && (
            <div className="w-full h-full flex items-center justify-center text-gray-500 relative z-20">
               장면이 없습니다.
            </div>
         )}
         
         {/* Audio Element */}
         <audio ref={audioRef} />

         {/* Info Overlay */}
         <div className="absolute top-4 left-4 bg-black/50 text-white px-3 py-1 rounded-full text-sm backdrop-blur-md border border-white/10 flex items-center gap-2 z-50">
           <span>Scene {currentSceneIndex + 1} / {scenes.length}</span>
           <span className="text-gray-400 text-xs">|</span>
           <Clock size={12} className="text-primary-400"/>
           <span>{currentScene?.duration || 0}s</span>
           {isExporting && <span className="text-red-500 font-bold animate-pulse ml-2">● 녹화 중...</span>}
         </div>
      </div>

      {/* Controls */}
      <div className="flex flex-wrap justify-center gap-4 mb-8">
        <button
          onClick={() => setIsPlaying(!isPlaying)}
          disabled={isExporting}
          className="flex items-center gap-2 px-8 py-3 bg-white text-gray-900 rounded-full font-bold hover:bg-gray-200 transition-colors shadow-lg disabled:opacity-50"
        >
          {isPlaying ? <Pause fill="currentColor" /> : <Play fill="currentColor" />}
          {isPlaying ? "일시정지" : "전체 재생"}
        </button>

        <button
          onClick={handleExportMp4}
          disabled={isExporting}
          className="flex items-center gap-2 px-8 py-3 bg-gradient-to-r from-red-600 to-rose-600 text-white rounded-full font-bold hover:from-red-500 hover:to-rose-500 transition-all shadow-lg shadow-red-500/30 disabled:opacity-50"
        >
          <FileVideo size={20} /> MP4 내보내기
        </button>

        <button
          onClick={handleExportWebm}
          disabled={isExporting}
          className="flex items-center gap-2 px-6 py-3 bg-gray-700 text-gray-200 rounded-full font-medium hover:bg-gray-600 transition-colors shadow-lg disabled:opacity-50"
          title="실시간 녹화 방식 (빠름, 오디오 미포함)"
        >
          <MonitorPlay size={18} /> WebM (실시간)
        </button>
      </div>

      {/* Export Info */}
      <div className="flex justify-center mb-8">
        <div className="bg-gray-800/50 rounded-lg px-4 py-2 text-xs text-gray-400 flex items-center gap-4 border border-gray-700">
          <span><strong className="text-primary-400">MP4:</strong> FFmpeg 인코딩, 오디오 포함, 고품질</span>
          <span className="text-gray-600">|</span>
          <span><strong className="text-gray-300">WebM:</strong> 실시간 녹화, 영상만</span>
        </div>
      </div>

      {/* Track Manager */}
      <div className="grid md:grid-cols-12 gap-6 bg-gray-800 p-6 rounded-xl border border-gray-700">
         {/* BGM Section */}
         <div className="md:col-span-3 border-r border-gray-700 pr-6">
            <h3 className="text-white font-bold mb-4 flex items-center justify-between">
              <span className="flex items-center gap-2"><Music className="text-primary-500" /> BGM 설정</span>
              <button onClick={() => setShowBgmHistory(!showBgmHistory)} className="text-xs text-gray-400 hover:text-white flex items-center gap-1"><History size={12}/> 기록</button>
            </h3>
            {showBgmHistory && bgmVersions.length > 0 && (
              <div className="mb-4 bg-gray-900 rounded-lg p-2 max-h-40 overflow-y-auto border border-gray-700">
                 {bgmVersions.map((v, i) => (
                    <div key={v.id} onClick={() => { setBgmUrl(v.url); setShowBgmHistory(false); }} className={`text-xs p-2 rounded cursor-pointer flex justify-between ${v.url === bgmUrl ? 'bg-indigo-900/50 text-indigo-300' : 'text-gray-400 hover:bg-gray-800'}`}>
                       <span>Ver {bgmVersions.length - i}</span>
                    </div>
                 ))}
              </div>
            )}
            <div className="space-y-3">
              <input value={bgmMood} onChange={(e) => setBgmMood(e.target.value)} className="w-full bg-gray-900 border border-gray-600 rounded p-3 text-sm text-white" placeholder="음악 분위기" />
              <button onClick={handleGenerateBgm} disabled={isGeneratingBgm} className="w-full py-2 bg-indigo-600 hover:bg-indigo-500 text-white rounded text-sm font-medium flex items-center justify-center gap-2">{isGeneratingBgm ? <RefreshCw className="animate-spin" size={16}/> : <Volume2 size={16}/>} BGM 생성</button>
            </div>
            {/* Hidden BGM Audio */}
            {bgmUrl && <audio ref={bgmRef} src={bgmUrl} loop />}
         </div>

         {/* Scene Timeline */}
         <div className="md:col-span-9 overflow-hidden">
           <div className="flex justify-between items-center mb-4">
              <div className="flex items-center gap-4">
                  <h3 className="text-white font-bold flex items-center gap-2"><Film className="text-primary-500" /> 타임라인</h3>
                  {/* Zoom Controls */}
                  <div className="flex items-center gap-2 bg-gray-900 rounded-lg p-1 border border-gray-700">
                     <ZoomOut size={14} className="text-gray-400" />
                     <input 
                        type="range" min="0.5" max="2.0" step="0.1" 
                        value={timelineZoom} 
                        onChange={(e) => setTimelineZoom(parseFloat(e.target.value))}
                        className="w-24 h-1 bg-gray-600 rounded-lg appearance-none cursor-pointer"
                     />
                     <ZoomIn size={14} className="text-gray-400" />
                  </div>
              </div>
              <button onClick={handleAutoSyncAll} className="text-xs flex items-center gap-1 px-3 py-1.5 bg-indigo-900/50 text-indigo-200 border border-indigo-700 rounded hover:bg-indigo-800 transition-colors"><Wand2 size={12} /> 전체 싱크 자동 맞춤</button>
           </div>
           
           <div className="overflow-x-auto pb-4">
             <div className="flex gap-2" style={{ transform: `scaleX(${timelineZoom})`, transformOrigin: 'left top', width: 'fit-content' }}>
               {scenes.map((scene, idx) => (
                 <div key={scene.id} className={`flex-shrink-0 w-56 bg-gray-900 rounded-lg overflow-hidden border transition-all flex flex-col ${currentSceneIndex === idx ? 'border-primary-500 ring-2 ring-primary-500/50' : 'border-gray-700 hover:border-gray-500'}`}>
                   <div onClick={() => { setCurrentSceneIndex(idx); setIsPlaying(false); pauseAll(); }} className="h-28 bg-black relative cursor-pointer group">
                     {scene.imageUrl && <img src={scene.imageUrl} className="w-full h-full object-cover opacity-70 group-hover:opacity-100 transition-opacity" />}
                     <div className="absolute top-1 right-1 flex flex-col gap-1">
                        {scene.videoUrl && <div className="bg-rose-600/80 p-1 rounded"><Film size={10} className="text-white"/></div>}
                        {scene.audioUrl && <div className="bg-indigo-600/80 p-1 rounded"><Mic size={10} className="text-white"/></div>}
                     </div>
                     <div className="absolute bottom-1 left-1 text-[10px] font-bold text-white bg-black/50 px-1 rounded">SC {idx + 1}</div>
                   </div>

                   {/* Editing Controls */}
                   <div className="p-2 bg-gray-800 border-t border-gray-700 space-y-2 text-xs">
                      {/* Duration */}
                      <div className="flex items-center justify-between gap-1">
                         <span className="text-gray-400">길이</span>
                         <div className="flex items-center">
                            <button className="w-5 h-5 bg-gray-700 rounded hover:bg-gray-600" onClick={() => updateSceneDuration(idx, Math.max(1, scene.duration - 1))}>-</button>
                            <span className="w-8 text-center text-white font-bold">{scene.duration}s</span>
                            <button className="w-5 h-5 bg-gray-700 rounded hover:bg-gray-600" onClick={() => updateSceneDuration(idx, scene.duration + 1)}>+</button>
                         </div>
                      </div>

                      {/* Transition */}
                      <div className="flex items-center justify-between gap-1">
                         <span className="text-gray-400">효과</span>
                         <select 
                            value={scene.transition}
                            onChange={(e) => updateSceneTransition(idx, e.target.value as TransitionType)}
                            className="bg-gray-900 border border-gray-600 rounded text-[10px] text-white p-1 w-20"
                         >
                            <option value="none">없음</option>
                            <option value="fade">Fade</option>
                            <option value="zoom">Zoom</option>
                         </select>
                      </div>

                      <div className="flex gap-1">
                         {scene.audioUrl && (
                            <button onClick={() => syncDurationToAudio(idx)} className="flex-1 py-1 bg-indigo-900/50 hover:bg-indigo-800 text-indigo-300 rounded border border-indigo-800 flex items-center justify-center gap-1" title="오디오 싱크">
                              <Link size={10} /> 싱크
                            </button>
                         )}
                         {onExtendVideo && scene.videoUrl && (
                            <button onClick={() => onExtendVideo(scene.id)} className="flex-1 py-1 bg-rose-900/50 hover:bg-rose-800 text-rose-300 rounded border border-rose-800 flex items-center justify-center gap-1" title="영상 연장 (Veo)">
                              <FastForward size={10} /> 연장
                            </button>
                         )}
                      </div>
                   </div>
                 </div>
               ))}
             </div>
           </div>
         </div>
      </div>
      <style>{`
        @keyframes zoomIn {
          from { transform: scale(1); }
          to { transform: scale(1.1); }
        }
        .animate-zoom-in {
          animation: zoomIn 10s linear forwards;
        }
      `}</style>
    </div>
  );
}