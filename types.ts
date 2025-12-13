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