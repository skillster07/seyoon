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
  TEMPLATE_SELECTOR = 'TEMPLATE_SELECTOR',
}

export interface AIStudio {
  hasSelectedApiKey: () => Promise<boolean>;
  openSelectKey: () => Promise<void>;
}

// ============================================
// Sea Monster Shorts Template Types
// ============================================

export interface VideoSpec {
  duration_s: number;
  fps: number;
  resolution_px: { w: number; h: number };
  aspect_ratio: string;
  delivery_codec_hint: string;
  audio_hint: {
    codec: string;
    sample_rate_hz: number;
    channels: number;
  };
}

export interface ColorGrade {
  dominant: string[];
  accent_by_scene_allowed: boolean;
  accent_examples?: string[];
}

export interface GlobalStyle {
  visual_tone: string[];
  color_grade: ColorGrade;
  camera_language: string[];
  vfx: string[];
  negative_prompt: string[];
}

export interface CountryTagStyle {
  position: string;
  layout: string;
  country_name_language: string;
  country_name_style: {
    font_family_hint: string;
    font_color: string;
    shadow: string;
    outline: string;
    size_ratio_of_frame_height: number;
  };
  flag_style: {
    shape: string;
    size_ratio_of_frame_height: number;
    margin_px: number;
  };
  safe_area_margin_px: number;
}

export interface SubtitleStyle {
  position: string;
  font_family_hint: string;
  font_color: string;
  shadow: string;
  max_chars_per_line: number;
  timing_mode: string;
}

export interface TitleCardStyle {
  enabled: boolean;
  text: string;
  position: string;
  style: {
    font_family_hint: string;
    fill_color: string;
    stroke: string;
    size_ratio_of_frame_height: number;
  };
  show_in_scene_id: string;
}

export interface OverlaySystem {
  country_tag: CountryTagStyle;
  subtitle: SubtitleStyle;
  title_card: TitleCardStyle;
}

export interface MusicEnergyCurve {
  t: number;
  level: number;
}

export interface AudioPlan {
  music: {
    genre: string;
    energy_curve: MusicEnergyCurve[];
  };
  sfx: string[];
  voiceover: {
    language: string;
    tone: string;
    pace_wpm_hint: number;
    script_mode: string;
    script_placeholder_note?: string;
  };
}

export interface TemplateShot {
  shot_id: string;
  duration_s: number;
  prompt: string;
  camera: string;
}

export interface TemplateSubtitle {
  t: number;
  text: string;
}

export interface TemplateScene {
  id: string;
  time_range_s: { start: number; end: number };
  country: { name_ko: string; flag: string };
  theme_monster: string;
  shots: TemplateShot[];
  subtitles: TemplateSubtitle[];
}

export interface RemixCountryExample {
  name_ko: string;
  flag: string;
  monster: string;
}

export interface RemixKnobs {
  what_to_change_for_new_version: string[];
  countries_list_example_new: RemixCountryExample[];
  format_lock: {
    keep_duration: boolean;
    keep_overlay_layout: boolean;
    keep_cut_pace: boolean;
    keep_realistic_cinematic_style: boolean;
  };
}

export interface ShortsTemplate {
  project: {
    title: string;
    target_platform: string[];
    video_spec: VideoSpec;
  };
  global_style: GlobalStyle;
  overlay_system: OverlaySystem;
  audio_plan: AudioPlan;
  scenes: TemplateScene[];
  remix_knobs: RemixKnobs;
}