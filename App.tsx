import React, { useState, useEffect, useRef } from 'react';
import { v4 as uuidv4 } from 'uuid';
import { analyzeScript, generateImageFromPrompt, generateVideo, generateSpeech, extendVideo } from './services/geminiService';
import { Scene, AppStatus, AppView, Character, MediaVersion, AIStudio, TransitionType, Project, UserSettings, LayerComposition } from './types';
import { SceneCard } from './components/SceneCard';
import { CharacterDB } from './components/CharacterDB';
import { StoryWriter } from './components/StoryWriter';
import { TimelineEditor } from './components/TimelineEditor';
import { ProjectDashboard } from './components/ProjectDashboard';
import { SettingsModal } from './components/SettingsModal';
import { MagicLayerEditor } from './components/MagicLayerEditor';
import { Clapperboard, Sparkles, AlertCircle, Trash2, ArrowRight, PenTool, Users, Layout, Layers, Key, Save, Upload, ArrowLeft, Download, Settings } from 'lucide-react';

const App: React.FC = () => {
  // --- Global State ---
  const [hasVeoKey, setHasVeoKey] = useState(false);
  const [projects, setProjects] = useState<Project[]>([]);
  const [activeProjectId, setActiveProjectId] = useState<string | null>(null);
  const [userSettings, setUserSettings] = useState<UserSettings>({
    defaultDuration: 5,
    autoSave: true,
    theme: 'dark',
    supertoneApiKey: '2a6f8814df88b46e1beb2e227c00f6b8' // Default provided key
  });
  const [isSettingsOpen, setIsSettingsOpen] = useState(false);

  // --- Active Project State ---
  const [currentView, setCurrentView] = useState<AppView>(AppView.STORY_WRITER);
  const [script, setScript] = useState('');
  const [status, setStatus] = useState<AppStatus>(AppStatus.IDLE);
  const [scenes, setScenes] = useState<Scene[]>([]);
  const [characters, setCharacters] = useState<Character[]>([]);
  const [bgmUrl, setBgmUrl] = useState<string | undefined>(undefined);
  const [bgmVersions, setBgmVersions] = useState<MediaVersion[]>([]);
  const [error, setError] = useState<string | null>(null);
  const [magicLayerSceneId, setMagicLayerSceneId] = useState<string | null>(null);

  // --- Load Data on Mount ---
  useEffect(() => {
    const savedProjects = localStorage.getItem('red_garden_projects');
    const savedSettings = localStorage.getItem('red_garden_settings');
    if (savedProjects) {
      try {
        setProjects(JSON.parse(savedProjects));
      } catch (e) { console.error(e); }
    }
    if (savedSettings) {
      try {
        // Merge saved settings with defaults to ensure new keys exist
        const parsed = JSON.parse(savedSettings);
        setUserSettings(prev => ({ ...prev, ...parsed }));
      } catch (e) { console.error(e); }
    }
  }, []);

  // --- Persist Data ---
  useEffect(() => {
    if (projects.length > 0) {
      localStorage.setItem('red_garden_projects', JSON.stringify(projects));
    }
  }, [projects]);

  useEffect(() => {
    localStorage.setItem('red_garden_settings', JSON.stringify(userSettings));
  }, [userSettings]);

  // Check for Veo Key on Mount
  useEffect(() => {
    const checkKey = async () => {
      const aistudio = (window as any).aistudio as AIStudio | undefined;
      if (aistudio && aistudio.hasSelectedApiKey) {
        const hasKey = await aistudio.hasSelectedApiKey();
        setHasVeoKey(hasKey);
      }
    };
    checkKey();
  }, []);

  const handleSelectKey = async () => {
    const aistudio = (window as any).aistudio as AIStudio | undefined;
    if (aistudio && aistudio.openSelectKey) {
      await aistudio.openSelectKey();
      setHasVeoKey(true);
    }
  };

  const handleClearAllData = () => {
    localStorage.removeItem('red_garden_projects');
    localStorage.removeItem('red_garden_settings');
    setProjects([]);
    setActiveProjectId(null);
    setUserSettings({ defaultDuration: 5, autoSave: true, theme: 'dark', supertoneApiKey: '' });
    alert("초기화되었습니다.");
  };

  // --- Project Management Handlers ---

  const handleCreateProject = (title: string) => {
    const newProject: Project = {
      id: uuidv4(),
      title,
      lastModified: Date.now(),
      data: {
        script: '',
        characters: [],
        scenes: [],
        bgmUrl: undefined,
        bgmVersions: []
      }
    };
    setProjects(prev => [newProject, ...prev]);
    handleSelectProject(newProject);
  };

  const handleSelectProject = (project: Project) => {
    setActiveProjectId(project.id);
    setScript(project.data.script);
    setCharacters(project.data.characters);
    setScenes(project.data.scenes);
    setBgmUrl(project.data.bgmUrl);
    setBgmVersions(project.data.bgmVersions);
    setCurrentView(AppView.STORY_WRITER);
    setStatus(AppStatus.IDLE);
    setError(null);
  };

  const handleExitProject = () => {
    if (activeProjectId) saveCurrentStateToProject();
    setActiveProjectId(null);
  };

  const handleDeleteProject = (id: string) => {
    setProjects(prev => {
        const updated = prev.filter(p => p.id !== id);
        if (updated.length === 0) localStorage.removeItem('red_garden_projects'); // Clean up if empty
        else localStorage.setItem('red_garden_projects', JSON.stringify(updated)); // Force save
        return updated;
    });
    if (activeProjectId === id) setActiveProjectId(null);
  };

  const saveCurrentStateToProject = () => {
    if (!activeProjectId) return;
    setProjects(prev => prev.map(p => {
      if (p.id === activeProjectId) {
        return {
          ...p,
          lastModified: Date.now(),
          data: {
            script,
            characters,
            scenes,
            bgmUrl,
            bgmVersions
          }
        };
      }
      return p;
    }));
  };

  // Auto-save logic (if enabled)
  useEffect(() => {
    if (activeProjectId && userSettings.autoSave) {
        // Debounce save to avoid too many writes
        const timer = setTimeout(() => {
            saveCurrentStateToProject();
        }, 2000);
        return () => clearTimeout(timer);
    }
  }, [script, characters, scenes, bgmUrl]);

  const handleManualSave = () => {
    saveCurrentStateToProject();
    alert("프로젝트가 브라우저에 저장되었습니다.");
  };

  const handleExportJSON = () => {
    saveCurrentStateToProject();
    const project = projects.find(p => p.id === activeProjectId);
    if (!project) return;
    const blob = new Blob([JSON.stringify(project, null, 2)], { type: 'application/json' });
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = `${project.title.replace(/\s+/g, '_')}_${new Date().getTime()}.json`;
    document.body.appendChild(a);
    a.click();
    document.body.removeChild(a);
    URL.revokeObjectURL(url);
  };

  const handleImportProject = (file: File) => {
    const reader = new FileReader();
    reader.onload = (e) => {
      try {
        const content = e.target?.result as string;
        const projectData = JSON.parse(content);
        if (!projectData.id || !projectData.data) throw new Error("Invalid format");
        
        const newProject = {
          ...projectData,
          id: uuidv4(),
          title: projectData.title + " (Imported)"
        };
        setProjects(prev => [newProject, ...prev]);
        alert("프로젝트를 가져왔습니다.");
      } catch (err) {
        alert("파일 형식이 올바르지 않습니다.");
      }
    };
    reader.readAsText(file);
  };

  // --- Editor Handlers ---

  const handleAnalyzeScript = async (inputScript?: string) => {
    const scriptToAnalyze = inputScript || script;
    if (!scriptToAnalyze.trim()) return;
    
    if (inputScript) {
      setScript(inputScript);
      setCurrentView(AppView.STORYBOARD);
    }

    setStatus(AppStatus.ANALYZING);
    setError(null);
    setScenes([]);

    try {
      const analysisResults = await analyzeScript(scriptToAnalyze);
      
      const newScenes: Scene[] = analysisResults.map(item => ({
        id: uuidv4(),
        originalText: item.original_text,
        visualPrompt: item.visual_prompt,
        imageVersions: [],
        videoVersions: [],
        audioVersions: [],
        duration: userSettings.defaultDuration, // Use user setting
        transition: 'fade', 
        isGeneratingImage: false,
        isGeneratingVideo: false,
        isGeneratingAudio: false
      }));

      setScenes(newScenes);
      setStatus(AppStatus.READY);
    } catch (e: any) {
      console.error(e);
      setError(e.message || "스크립트 분석에 실패했습니다. (JSON 파싱 오류 등)");
      setStatus(AppStatus.IDLE);
    }
  };

  const createVersion = (url: string): MediaVersion => ({
    id: uuidv4(),
    url,
    timestamp: Date.now(),
    label: new Date().toLocaleTimeString()
  });

  const handleGenerateImage = async (id: string, prompt: string) => {
    setScenes(prev => prev.map(scene => 
      scene.id === id ? { ...scene, isGeneratingImage: true, error: undefined } : scene
    ));

    try {
      const imageUrl = await generateImageFromPrompt(prompt);
      setScenes(prev => prev.map(scene => {
        if (scene.id !== id) return scene;
        const newVersion = createVersion(imageUrl);
        return { 
          ...scene, 
          imageUrl, 
          imageVersions: [newVersion, ...scene.imageVersions],
          isGeneratingImage: false 
        };
      }));
    } catch (e: any) {
      setScenes(prev => prev.map(scene => 
        scene.id === id ? { ...scene, isGeneratingImage: false, error: "이미지 생성 실패" } : scene
      ));
    }
  };

  const handleGenerateVideo = async (id: string) => {
    if (!hasVeoKey) {
      setIsSettingsOpen(true); // Open settings to select key
      alert("영상 생성을 위해서는 유료 API Key가 필요합니다. 설정에서 키를 선택해주세요.");
      return;
    }

    const scene = scenes.find(s => s.id === id);
    if (!scene || !scene.imageUrl) return;

    setScenes(prev => prev.map(s => 
      s.id === id ? { ...s, isGeneratingVideo: true, error: undefined } : s
    ));

    try {
      const { url: videoUrl, assetContext } = await generateVideo(scene.visualPrompt, scene.imageUrl);
      
      setScenes(prev => prev.map(s => {
        if (s.id !== id) return s;
        const newVersion = createVersion(videoUrl);
        return { 
          ...s, 
          videoUrl, 
          videoAssetContext: assetContext,
          videoVersions: [newVersion, ...s.videoVersions],
          isGeneratingVideo: false 
        };
      }));
    } catch (e: any) {
      console.error(e);
      setScenes(prev => prev.map(s => 
        s.id === id ? { ...s, isGeneratingVideo: false, error: "영상 생성 실패" } : s
      ));
    }
  };

  const handleExtendVideo = async (id: string) => {
    if (!hasVeoKey) {
      setIsSettingsOpen(true);
      return;
    }

    const scene = scenes.find(s => s.id === id);
    if (!scene || !scene.videoAssetContext) {
      alert("연장할 수 있는 원본 영상 정보가 없습니다.");
      return;
    }

    setScenes(prev => prev.map(s => 
      s.id === id ? { ...s, isGeneratingVideo: true, error: undefined } : s
    ));

    try {
      const { url: videoUrl, assetContext } = await extendVideo(scene.visualPrompt, scene.videoAssetContext);
      
      setScenes(prev => prev.map(s => {
        if (s.id !== id) return s;
        const newVersion = createVersion(videoUrl);
        return { 
          ...s, 
          videoUrl, 
          videoAssetContext: assetContext, 
          videoVersions: [newVersion, ...s.videoVersions],
          isGeneratingVideo: false,
          duration: s.duration + 5
        };
      }));
    } catch (e: any) {
      console.error(e);
      setScenes(prev => prev.map(s => 
        s.id === id ? { ...s, isGeneratingVideo: false, error: "영상 연장 실패" } : s
      ));
    }
  };

  const handleGenerateAudio = async (id: string) => {
    const scene = scenes.find(s => s.id === id);
    if (!scene) return;

    setScenes(prev => prev.map(s => 
      s.id === id ? { ...s, isGeneratingAudio: true, error: undefined } : s
    ));

    try {
      const audioUrl = await generateSpeech(scene.originalText);
      setScenes(prev => prev.map(s => {
        if (s.id !== id) return s;
        const newVersion = createVersion(audioUrl);
        return { 
          ...s, 
          audioUrl, 
          audioVersions: [newVersion, ...s.audioVersions],
          isGeneratingAudio: false 
        };
      }));
    } catch (e: any) {
      setScenes(prev => prev.map(s => 
        s.id === id ? { ...s, isGeneratingAudio: false, error: "대사 생성 실패" } : s
      ));
    }
  };

  const handleGenerateAll = async () => {
    const scenesToGenerate = scenes.filter(s => !s.imageUrl);
    if (scenesToGenerate.length === 0) return;
    for (const scene of scenesToGenerate) {
       await handleGenerateImage(scene.id, scene.visualPrompt);
    }
  };

  const updatePrompt = (id: string, newPrompt: string) => {
    setScenes(prev => prev.map(scene => 
      scene.id === id ? { ...scene, visualPrompt: newPrompt } : scene
    ));
  };

  const handleUpdateScene = (updatedScene: Scene) => {
    setScenes(prev => prev.map(s => s.id === updatedScene.id ? updatedScene : s));
  };

  const handleOpenMagicLayer = (sceneId: string) => {
    setMagicLayerSceneId(sceneId);
  };

  const handleSaveMagicLayer = (newImageDataUrl: string, composition: LayerComposition) => {
    const sceneId = magicLayerSceneId;
    if (!sceneId) return;
    setScenes(prev => prev.map(s => {
      if (s.id !== sceneId) return s;
      const newVersion: MediaVersion = {
        id: uuidv4(),
        url: newImageDataUrl,
        timestamp: Date.now(),
        label: '매직 레이어',
      };
      return {
        ...s,
        imageUrl: newImageDataUrl,
        imageVersions: [newVersion, ...s.imageVersions],
        magicLayerComposition: composition,
      };
    }));
    setMagicLayerSceneId(null);
  };

  const handleAddBgmVersion = (url: string) => {
    const newVersion = createVersion(url);
    setBgmVersions(prev => [newVersion, ...prev]);
    setBgmUrl(url);
  };

  const handleReset = () => {
    if (window.confirm("현재 프로젝트의 내용을 초기화하시겠습니까?")) {
      setScript('');
      setScenes([]);
      setBgmUrl(undefined);
      setBgmVersions([]);
      setStatus(AppStatus.IDLE);
      setError(null);
    }
  };

  const activeProjectTitle = projects.find(p => p.id === activeProjectId)?.title || 'Unsaved Project';

  const magicLayerScene = magicLayerSceneId ? scenes.find(s => s.id === magicLayerSceneId) : null;

  return (
    <div className="min-h-screen bg-[#0f172a] text-gray-100 flex flex-col font-sans">
      <SettingsModal
        isOpen={isSettingsOpen}
        onClose={() => setIsSettingsOpen(false)}
        settings={userSettings}
        onSaveSettings={setUserSettings}
        hasVeoKey={hasVeoKey}
        onSelectVeoKey={handleSelectKey}
        onClearAllData={handleClearAllData}
      />

      {magicLayerScene && (
        <MagicLayerEditor
          scene={magicLayerScene}
          onClose={() => setMagicLayerSceneId(null)}
          onSave={handleSaveMagicLayer}
        />
      )}

      {/* --- DASHBOARD VIEW --- */}
      {!activeProjectId ? (
        <ProjectDashboard 
          projects={projects}
          onCreateProject={handleCreateProject}
          onSelectProject={handleSelectProject}
          onDeleteProject={handleDeleteProject}
          onImportProject={handleImportProject}
        />
      ) : (
        /* --- EDITOR VIEW --- */
        <>
          <header className="bg-gray-900/90 backdrop-blur-md border-b border-gray-800 sticky top-0 z-50 shadow-lg">
            <div className="max-w-7xl mx-auto px-4 sm:px-6 lg:px-8 h-16 flex items-center justify-between">
              <div className="flex items-center gap-2">
                <button 
                  onClick={handleExitProject}
                  className="mr-2 text-gray-400 hover:text-white transition-colors"
                  title="프로젝트 목록으로 돌아가기"
                >
                  <ArrowLeft size={20} />
                </button>
                <div className="bg-primary-600 p-2 rounded-lg shadow-lg shadow-primary-600/30">
                  <Clapperboard className="text-white" size={20} />
                </div>
                <div>
                  <h1 className="text-sm text-gray-400 font-medium">붉은 정원 에피소드 메이커</h1>
                  <p className="text-lg font-bold text-white leading-none">{activeProjectTitle}</p>
                </div>
              </div>
              
              <div className="flex items-center gap-3">
                <div className="flex items-center bg-gray-800 rounded-lg p-1 gap-1 border border-gray-700">
                  <button onClick={handleManualSave} className="p-1.5 text-gray-400 hover:text-white hover:bg-gray-700 rounded transition-colors" title="프로젝트 저장"><Save size={18} /></button>
                  <button onClick={handleExportJSON} className="p-1.5 text-gray-400 hover:text-white hover:bg-gray-700 rounded transition-colors" title="JSON 백업"><Download size={18} /></button>
                </div>

                <div className="h-6 w-px bg-gray-700 mx-1"></div>
                
                <button 
                  onClick={() => setIsSettingsOpen(true)}
                  className="p-1.5 text-gray-400 hover:text-white hover:bg-gray-700 rounded transition-colors"
                  title="설정"
                >
                   <Settings size={20} />
                </button>

                <div className="h-6 w-px bg-gray-700 mx-1"></div>

                <nav className="flex items-center gap-1 bg-gray-800 p-1 rounded-lg">
                  <button onClick={() => setCurrentView(AppView.CHARACTERS)} className={`flex items-center gap-2 px-3 md:px-4 py-1.5 rounded-md text-xs md:text-sm font-medium transition-all ${currentView === AppView.CHARACTERS ? 'bg-gray-700 text-white shadow' : 'text-gray-400 hover:text-white'}`}><Users size={16} /> <span className="hidden md:inline">캐릭터</span></button>
                  <button onClick={() => setCurrentView(AppView.STORY_WRITER)} className={`flex items-center gap-2 px-3 md:px-4 py-1.5 rounded-md text-xs md:text-sm font-medium transition-all ${currentView === AppView.STORY_WRITER ? 'bg-gray-700 text-white shadow' : 'text-gray-400 hover:text-white'}`}><PenTool size={16} /> <span className="hidden md:inline">스토리</span></button>
                  <button onClick={() => setCurrentView(AppView.STORYBOARD)} className={`flex items-center gap-2 px-3 md:px-4 py-1.5 rounded-md text-xs md:text-sm font-medium transition-all ${currentView === AppView.STORYBOARD ? 'bg-gray-700 text-white shadow' : 'text-gray-400 hover:text-white'}`}><Layout size={16} /> <span className="hidden md:inline">스토리보드</span></button>
                  <button onClick={() => setCurrentView(AppView.TIMELINE)} className={`flex items-center gap-2 px-3 md:px-4 py-1.5 rounded-md text-xs md:text-sm font-medium transition-all ${currentView === AppView.TIMELINE ? 'bg-gray-700 text-white shadow' : 'text-gray-400 hover:text-white'}`}><Layers size={16} /> <span className="hidden md:inline">편집</span></button>
                </nav>
              </div>
            </div>
          </header>

          <main className="flex-grow max-w-7xl mx-auto px-4 sm:px-6 lg:px-8 py-8 w-full">
            {currentView === AppView.CHARACTERS && <CharacterDB characters={characters} setCharacters={setCharacters} />}
            {currentView === AppView.STORY_WRITER && <StoryWriter characters={characters} onScriptGenerated={handleAnalyzeScript} />}
            {currentView === AppView.TIMELINE && (
              <TimelineEditor 
                scenes={scenes} 
                setScenes={setScenes}
                bgmUrl={bgmUrl} 
                setBgmUrl={setBgmUrl}
                bgmVersions={bgmVersions}
                onAddBgmVersion={handleAddBgmVersion}
                onExtendVideo={handleExtendVideo}
              />
            )}
            {currentView === AppView.STORYBOARD && (
              <div className="animate-in fade-in duration-500">
                {status === AppStatus.IDLE || status === AppStatus.ANALYZING ? (
                  <div className="max-w-3xl mx-auto mt-4">
                    <div className="text-center mb-8">
                      <h2 className="text-3xl font-bold text-white mb-4">장면 생성 및 시각화</h2>
                      <p className="text-gray-400">완성된 스크립트를 분석하여 스토리보드를 생성합니다.</p>
                    </div>
                    <div className="bg-gray-800 rounded-2xl shadow-xl p-2 border border-gray-700">
                      <textarea value={script} onChange={(e) => setScript(e.target.value)} placeholder="여기에 스크립트를 입력하세요..." className="w-full h-64 bg-gray-900 rounded-xl p-6 text-lg text-white placeholder-gray-500 focus:outline-none focus:ring-2 focus:ring-primary-500/50 resize-none" />
                      <div className="flex justify-between items-center p-4">
                        <span className="text-xs text-gray-500 font-medium">{script.length} 글자</span>
                        <button onClick={() => handleAnalyzeScript()} disabled={!script.trim() || status === AppStatus.ANALYZING} className={`flex items-center gap-2 px-8 py-3 rounded-xl font-bold text-white transition-all transform hover:scale-105 ${!script.trim() || status === AppStatus.ANALYZING ? 'bg-gray-700 cursor-not-allowed text-gray-500' : 'bg-gradient-to-r from-primary-600 to-rose-600 shadow-lg shadow-primary-500/30 hover:shadow-primary-500/50'}`}>
                            {status === AppStatus.ANALYZING ? <><Sparkles className="animate-spin" size={20} /> 분석 중...</> : <><Sparkles size={20} /> 시각화하기 <ArrowRight size={20} /></>}
                        </button>
                      </div>
                    </div>
                    {error && <div className="mt-4 p-4 bg-red-900/30 border border-red-800 rounded-xl flex items-center gap-3 text-red-200"><AlertCircle size={20} /><p>{error}</p></div>}
                  </div>
                ) : (
                  <div>
                    <div className="flex justify-between items-end mb-6">
                        <div><h2 className="text-2xl font-bold text-white flex items-center gap-2"><Layout className="text-primary-500"/> 스토리보드 장면</h2><p className="text-gray-400 text-sm mt-1">프롬프트를 확인하고 이미지(Image), 영상(Veo), 대사(Supertone)를 생성하세요.</p></div>
                        <div className="flex gap-2">
                          <button onClick={handleReset} className="flex items-center gap-2 px-4 py-2 bg-gray-800 hover:bg-gray-700 text-gray-300 rounded-lg font-medium transition-colors"><Trash2 size={16} /> 초기화</button>
                          <button onClick={handleGenerateAll} className="flex items-center gap-2 px-6 py-2 bg-primary-600 hover:bg-primary-500 text-white rounded-lg font-medium shadow-lg shadow-primary-500/20 transition-all"><Sparkles size={18} /> 누락된 이미지 생성</button>
                        </div>
                    </div>
                    <div className="space-y-8 pb-20">
                      {scenes.map((scene, index) => (
                        <div key={scene.id} className="relative">
                          <div className="absolute -left-3 md:-left-12 top-0 bottom-0 w-0.5 bg-gray-800 hidden md:block"></div>
                          <div className="absolute -left-[19px] md:-left-[55px] top-6 w-8 h-8 rounded-full bg-gray-900 border-2 border-gray-700 flex items-center justify-center text-sm font-bold text-gray-500 z-10 hidden md:flex">{index + 1}</div>
                          <SceneCard
                              scene={scene}
                              onGenerateImage={handleGenerateImage}
                              onUpdatePrompt={updatePrompt}
                              onGenerateVideo={handleGenerateVideo}
                              onGenerateAudio={handleGenerateAudio}
                              onUpdateScene={handleUpdateScene}
                              onOpenMagicLayer={handleOpenMagicLayer}
                          />
                        </div>
                      ))}
                    </div>
                    <div className="fixed bottom-8 right-8 z-50 animate-bounce">
                        <button onClick={() => setCurrentView(AppView.TIMELINE)} className="bg-gradient-to-r from-indigo-600 to-purple-600 text-white px-6 py-4 rounded-full shadow-2xl font-bold flex items-center gap-2 hover:scale-105 transition-transform"><Layers size={20} /> 타임라인 편집기로 이동</button>
                    </div>
                  </div>
                )}
              </div>
            )}
          </main>
        </>
      )}
    </div>
  );
};

export default App;