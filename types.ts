// === News Video Automation Types ===

export interface NewsItem {
  id: string;
  rank: number;
  title: string;
  summary: string;           // 2-3 sentence summary
  narrationScript: string;   // Full narration script for TTS
  keywords: string[];
  imagePrompt: string;       // English prompt for image generation
  source?: string;           // News source name
  sourceUrl?: string;        // Original article URL

  // Generated Media
  imageUrl?: string;
  audioUrl?: string;
  videoUrl?: string;
  videoAssetContext?: any;

  // Generation States
  isGeneratingImage: boolean;
  isGeneratingAudio: boolean;
  isGeneratingVideo: boolean;
  error?: string;
}

export interface SearchResult {
  title: string;
  snippet: string;
  url?: string;
  source?: string;
}

export enum PipelineStep {
  IDLE = 'IDLE',
  SEARCHING_NEWS = 'SEARCHING_NEWS',
  ANALYZING = 'ANALYZING',
  REVIEW = 'REVIEW',
  GENERATING_MEDIA = 'GENERATING_MEDIA',
  COMPLETE = 'COMPLETE',
}

export interface PipelineState {
  step: PipelineStep;
  progress: number;        // 0-100
  message: string;
  error?: string;
}

export interface ProjectData {
  id: string;
  title: string;
  date: string;            // Target date (YYYY-MM-DD)
  createdAt: number;
  newsItems: NewsItem[];
  introScript?: string;    // Opening narration
  outroScript?: string;    // Closing narration
  introAudioUrl?: string;
  outroAudioUrl?: string;
  bgmUrl?: string;
}

export interface UserSettings {
  geminiApiKey?: string;
  ttsVoice: string;
  defaultImageStyle: string;
  autoGenerateMedia: boolean;
}

export interface AIStudio {
  hasSelectedApiKey: () => Promise<boolean>;
  openSelectKey: () => Promise<void>;
}
