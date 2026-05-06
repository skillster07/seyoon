export interface Character {
  id: string;
  name: string;
  age: string;
  description: string;
  visualStyle: string; // Used for prompt generation
  basePrompt: string; // Basic prompt structure (e.g., "A korean man wearing suit")
  images: { [key: string]: string }; // Map key (main, anxious, etc) to URL
}

export interface MediaVersion {
  id: string;
  url: string;
  timestamp: number;
  label?: string;
}

export type TransitionType = 'none' | 'fade' | 'zoom';

export interface Scene {
  id: string;
  originalText: string;
  visualPrompt: string;

  // Active Media
  imageUrl?: string;
  videoUrl?: string;
  videoAssetContext?: any; // To store Veo operation/asset info for extension
  audioUrl?: string;

  // Versions / History
  imageVersions: MediaVersion[];
  videoVersions: MediaVersion[];
  audioVersions: MediaVersion[];

  // Playback Settings
  duration: number; // Duration in seconds (default 5s)
  transition: TransitionType; // Visual transition effect

  isGeneratingImage: boolean;
  isGeneratingVideo: boolean;
  isGeneratingAudio: boolean;
  error?: string;

  // Magic Layer (optional - last saved composition for re-editing)
  magicLayerComposition?: LayerComposition;
}

// === Magic Layer ===

export type LayerType = 'background' | 'object' | 'text';

export interface Transform {
  x: number;
  y: number;
  width: number;
  height: number;
  rotation: number; // degrees
  scaleX: number;
  scaleY: number;
  opacity: number;
}

export interface DetectionBox {
  // Gemini normalized [y0,x0,y1,x1] in 0-1000 range
  y0: number;
  x0: number;
  y1: number;
  x1: number;
}

export interface Layer {
  id: string;
  type: LayerType;
  label: string;
  zIndex: number;
  visible: boolean;
  locked: boolean;
  transform: Transform;
  // type === 'object'
  imageDataUrl?: string;
  sourceBox?: DetectionBox;
  maskDataUrl?: string;
  // type === 'text'
  text?: string;
  fontFamily?: string;
  fontSize?: number;
  color?: string;
  textAlign?: 'left' | 'center' | 'right';
}

export interface LayerComposition {
  id: string;
  sourceImageUrl: string;
  width: number;
  height: number;
  layers: Layer[];
  createdAt: number;
}

export interface ScriptAnalysisResponse {
  original_text: string;
  visual_prompt: string;
}

export interface UserSettings {
  defaultDuration: number;
  autoSave: boolean;
  theme: 'dark' | 'light';
  supertoneApiKey?: string;
}

export interface Project {
  id: string;
  title: string;
  lastModified: number; // timestamp
  data: {
    script: string;
    characters: Character[];
    scenes: Scene[];
    bgmUrl?: string;
    bgmVersions: MediaVersion[];
  };
}

export enum AppStatus {
  IDLE = 'IDLE',
  ANALYZING = 'ANALYZING',
  READY = 'READY',
}

export enum AppView {
  CHARACTERS = 'CHARACTERS',
  STORY_WRITER = 'STORY_WRITER',
  STORYBOARD = 'STORYBOARD',
  TIMELINE = 'TIMELINE',
}

export interface AIStudio {
  hasSelectedApiKey: () => Promise<boolean>;
  openSelectKey: () => Promise<void>;
}