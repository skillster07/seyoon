import { useEffect, useMemo, useState } from 'react';

export type SourceType = 'camera' | 'image' | 'text' | 'screen';
export type StudioScene = { id: number; name: string; thumb: string };
export type StudioLayer = {
  id: number;
  sceneId: number;
  name: string;
  type: SourceType;
  visible: boolean;
  locked: boolean;
};
export type BeautySettings = { smooth: number; tone: number; blemish: number; sharpness: number; jaw: number; eyes: number; chin: number };
export type OutputPlatform = 'soop' | 'tiktok' | 'obs';
export type OutputSettings = {
  platform: OutputPlatform;
  width: number;
  height: number;
  fps: 30 | 60;
  bitrate: number;
  encoder: 'auto' | 'nvenc' | 'qsv' | 'amf';
};
export type StudioProject = {
  name: string;
  activeScene: number;
  scenes: StudioScene[];
  layers: StudioLayer[];
  orientation: 'portrait' | 'landscape';
  background: string;
  beauty: BeautySettings;
  output: OutputSettings;
  updatedAt: number;
};

const STORAGE_KEY = 'vividcam_studio_project_v1';
const defaults: StudioProject = {
  name: '윤슬의 뮤직 라이브',
  activeScene: 1,
  scenes: [
    { id: 1, name: '메인 방송', thumb: 'main' },
    { id: 2, name: '토크 화면', thumb: 'talk' },
    { id: 3, name: '잠시 후 시작', thumb: 'wait' },
  ],
  layers: [
    { id: 1, sceneId: 1, name: '캠 1 · Sony ZV-E10', type: 'camera', visible: true, locked: false },
    { id: 2, sceneId: 1, name: 'Soft violet gradient', type: 'image', visible: true, locked: true },
    { id: 3, sceneId: 1, name: '오늘도 반가워요 ✨', type: 'text', visible: true, locked: false },
  ],
  orientation: 'landscape',
  background: 'violet',
  beauty: { smooth: 42, tone: 18, blemish: 28, sharpness: 14, jaw: 12, eyes: 8, chin: 6 },
  output: { platform: 'soop', width: 1920, height: 1080, fps: 60, bitrate: 8000, encoder: 'auto' },
  updatedAt: Date.now(),
};

export const useStudioProject = () => {
  const [project, setProject] = useState<StudioProject>(() => {
    try {
      const saved = localStorage.getItem(STORAGE_KEY);
      if (!saved) return defaults;
      const parsed = JSON.parse(saved) as Partial<StudioProject>;
      return {
        ...defaults,
        ...parsed,
        beauty: { ...defaults.beauty, ...parsed.beauty },
        output: { ...defaults.output, ...parsed.output, fps: parsed.output?.fps === 30 ? 30 : 60 },
      };
    } catch { return defaults; }
  });
  const [savedAt, setSavedAt] = useState(project.updatedAt);

  useEffect(() => {
    const timer = window.setTimeout(() => {
      const next = { ...project, updatedAt: Date.now() };
      localStorage.setItem(STORAGE_KEY, JSON.stringify(next));
      setSavedAt(next.updatedAt);
    }, 350);
    return () => window.clearTimeout(timer);
  }, [project]);

  const activeLayers = useMemo(() => project.layers.filter((layer) => layer.sceneId === project.activeScene), [project.layers, project.activeScene]);
  const update = (patch: Partial<StudioProject>) => setProject((prev) => ({ ...prev, ...patch }));
  const setBeauty = (key: keyof BeautySettings, value: number) => setProject((prev) => ({ ...prev, beauty: { ...prev.beauty, [key]: value } }));
  const setOutput = (patch: Partial<OutputSettings>) => setProject((prev) => ({ ...prev, output: { ...prev.output, ...patch } }));
  const resetBeautyGroup = (keys: (keyof BeautySettings)[]) => setProject((prev) => ({ ...prev, beauty: keys.reduce((beauty, key) => ({ ...beauty, [key]: 0 }), prev.beauty) }));

  const addScene = () => {
    const id = Date.now();
    setProject((prev) => ({ ...prev, activeScene: id, scenes: [...prev.scenes, { id, name: `새 장면 ${prev.scenes.length + 1}`, thumb: 'new' }] }));
  };
  const renameScene = (id: number, name: string) => setProject((prev) => ({ ...prev, scenes: prev.scenes.map((scene) => scene.id === id ? { ...scene, name } : scene) }));
  const removeScene = (id: number) => setProject((prev) => {
    if (prev.scenes.length === 1) return prev;
    const scenes = prev.scenes.filter((scene) => scene.id !== id);
    return { ...prev, scenes, layers: prev.layers.filter((layer) => layer.sceneId !== id), activeScene: prev.activeScene === id ? scenes[0].id : prev.activeScene };
  });
  const addLayer = (type: SourceType, name: string) => setProject((prev) => ({ ...prev, layers: [{ id: Date.now(), sceneId: prev.activeScene, name, type, visible: true, locked: false }, ...prev.layers] }));
  const toggleLayer = (id: number, key: 'visible' | 'locked') => setProject((prev) => ({ ...prev, layers: prev.layers.map((layer) => layer.id === id ? { ...layer, [key]: !layer[key] } : layer) }));
  const removeLayer = (id: number) => setProject((prev) => ({ ...prev, layers: prev.layers.filter((layer) => layer.id !== id) }));

  return { project, activeLayers, savedAt, update, setBeauty, setOutput, resetBeautyGroup, addScene, renameScene, removeScene, addLayer, toggleLayer, removeLayer };
};
