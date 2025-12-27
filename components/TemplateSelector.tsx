import React, { useState } from 'react';
import { ShortsTemplate, TemplateScene, Scene } from '../types';
import { seaMonsterShortsTemplate, getFlagEmoji } from '../templates/seaMonsterShorts';
import {
  Waves,
  Clock,
  Film,
  Play,
  ChevronDown,
  ChevronUp,
  Monitor,
  Music,
  Palette,
  Sparkles,
  ArrowRight,
  Skull,
  Globe
} from 'lucide-react';
import { v4 as uuidv4 } from 'uuid';

interface TemplateSelectorProps {
  onApplyTemplate: (scenes: Scene[], projectTitle: string) => void;
  onCancel: () => void;
}

export const TemplateSelector: React.FC<TemplateSelectorProps> = ({
  onApplyTemplate,
  onCancel
}) => {
  const [selectedTemplate, setSelectedTemplate] = useState<ShortsTemplate>(seaMonsterShortsTemplate);
  const [expandedScene, setExpandedScene] = useState<string | null>('SC01_JP');
  const [isApplying, setIsApplying] = useState(false);

  const templates = [
    {
      id: 'sea-monster-shorts',
      name: '국가별 바다괴물',
      template: seaMonsterShortsTemplate,
      icon: Skull,
      description: '각 나라의 전설적인 바다괴물을 시네마틱하게 표현'
    }
  ];

  const convertTemplateToScenes = (template: ShortsTemplate): Scene[] => {
    const scenes: Scene[] = [];
    const globalStyle = template.global_style;
    const negativePrompt = globalStyle.negative_prompt.join(', ');
    const visualTone = globalStyle.visual_tone.join(', ');

    template.scenes.forEach((templateScene: TemplateScene) => {
      templateScene.shots.forEach((shot) => {
        // Build enhanced prompt with global style context
        const enhancedPrompt = `${shot.prompt}, ${visualTone}, camera: ${shot.camera}, vertical 9:16 aspect ratio for shorts/reels, ${template.project.title} - ${templateScene.country.name_ko} ${getFlagEmoji(templateScene.country.flag)}, monster theme: ${templateScene.theme_monster}. Negative: ${negativePrompt}`;

        const scene: Scene = {
          id: uuidv4(),
          originalText: `[${templateScene.country.name_ko}] ${templateScene.theme_monster} - ${shot.shot_id}`,
          visualPrompt: enhancedPrompt,
          duration: shot.duration_s,
          transition: 'fade',
          imageVersions: [],
          videoVersions: [],
          audioVersions: [],
          isGeneratingImage: false,
          isGeneratingVideo: false,
          isGeneratingAudio: false
        };
        scenes.push(scene);
      });
    });

    return scenes;
  };

  const handleApply = () => {
    setIsApplying(true);
    const scenes = convertTemplateToScenes(selectedTemplate);
    onApplyTemplate(scenes, selectedTemplate.project.title);
  };

  const toggleScene = (sceneId: string) => {
    setExpandedScene(expandedScene === sceneId ? null : sceneId);
  };

  const formatDuration = (seconds: number) => {
    const mins = Math.floor(seconds / 60);
    const secs = Math.round(seconds % 60);
    return mins > 0 ? `${mins}:${secs.toString().padStart(2, '0')}` : `${secs}s`;
  };

  return (
    <div className="max-w-6xl mx-auto px-4 py-8 animate-in fade-in duration-500">
      {/* Header */}
      <div className="text-center mb-8">
        <h1 className="text-3xl font-bold bg-clip-text text-transparent bg-gradient-to-r from-cyan-400 to-blue-500 mb-3">
          Shorts 템플릿
        </h1>
        <p className="text-gray-400">바이럴 숏폼 콘텐츠를 위한 사전 구성된 템플릿</p>
      </div>

      <div className="grid lg:grid-cols-3 gap-6">
        {/* Template List - Left Column */}
        <div className="lg:col-span-1 space-y-4">
          <h2 className="text-lg font-semibold text-gray-300 mb-3 flex items-center gap-2">
            <Sparkles size={18} className="text-yellow-400" /> 사용 가능한 템플릿
          </h2>

          {templates.map((t) => (
            <div
              key={t.id}
              onClick={() => setSelectedTemplate(t.template)}
              className={`p-4 rounded-xl border-2 cursor-pointer transition-all ${
                selectedTemplate === t.template
                  ? 'bg-cyan-900/30 border-cyan-500 shadow-lg shadow-cyan-500/20'
                  : 'bg-gray-800 border-gray-700 hover:border-gray-600'
              }`}
            >
              <div className="flex items-center gap-3 mb-2">
                <div className={`p-2 rounded-lg ${
                  selectedTemplate === t.template ? 'bg-cyan-600' : 'bg-gray-700'
                }`}>
                  <t.icon size={24} className="text-white" />
                </div>
                <div>
                  <h3 className="font-bold text-white">{t.name}</h3>
                  <p className="text-xs text-gray-400">{t.description}</p>
                </div>
              </div>
            </div>
          ))}

          {/* Quick Stats */}
          <div className="mt-6 p-4 bg-gray-800/50 rounded-xl border border-gray-700">
            <h3 className="text-sm font-semibold text-gray-400 mb-3">템플릿 정보</h3>
            <div className="grid grid-cols-2 gap-3 text-sm">
              <div className="flex items-center gap-2 text-gray-300">
                <Clock size={14} className="text-cyan-400" />
                <span>{formatDuration(selectedTemplate.project.video_spec.duration_s)}</span>
              </div>
              <div className="flex items-center gap-2 text-gray-300">
                <Monitor size={14} className="text-cyan-400" />
                <span>{selectedTemplate.project.video_spec.aspect_ratio}</span>
              </div>
              <div className="flex items-center gap-2 text-gray-300">
                <Film size={14} className="text-cyan-400" />
                <span>{selectedTemplate.scenes.length}개 장면</span>
              </div>
              <div className="flex items-center gap-2 text-gray-300">
                <Globe size={14} className="text-cyan-400" />
                <span>{selectedTemplate.scenes.reduce((acc, s) => acc + s.shots.length, 0)}개 샷</span>
              </div>
            </div>

            <div className="mt-4 pt-3 border-t border-gray-700">
              <div className="flex flex-wrap gap-1.5">
                {selectedTemplate.project.target_platform.map((platform) => (
                  <span key={platform} className="px-2 py-1 bg-gray-700 rounded text-xs text-gray-300">
                    {platform}
                  </span>
                ))}
              </div>
            </div>
          </div>
        </div>

        {/* Template Preview - Right Column */}
        <div className="lg:col-span-2 space-y-4">
          {/* Style Preview */}
          <div className="bg-gray-800 rounded-xl border border-gray-700 p-4">
            <h3 className="text-lg font-semibold text-white mb-4 flex items-center gap-2">
              <Palette size={18} className="text-purple-400" /> 글로벌 스타일
            </h3>

            <div className="grid md:grid-cols-2 gap-4">
              {/* Visual Tone */}
              <div>
                <h4 className="text-sm text-gray-400 mb-2">비주얼 톤</h4>
                <div className="flex flex-wrap gap-1.5">
                  {selectedTemplate.global_style.visual_tone.slice(0, 4).map((tone, i) => (
                    <span key={i} className="px-2 py-1 bg-purple-900/30 border border-purple-700/50 rounded text-xs text-purple-300">
                      {tone}
                    </span>
                  ))}
                  {selectedTemplate.global_style.visual_tone.length > 4 && (
                    <span className="px-2 py-1 text-xs text-gray-500">
                      +{selectedTemplate.global_style.visual_tone.length - 4}
                    </span>
                  )}
                </div>
              </div>

              {/* Color Grade */}
              <div>
                <h4 className="text-sm text-gray-400 mb-2">컬러 그레이드</h4>
                <div className="flex flex-wrap gap-1.5">
                  {selectedTemplate.global_style.color_grade.dominant.map((color, i) => (
                    <span key={i} className="px-2 py-1 bg-teal-900/30 border border-teal-700/50 rounded text-xs text-teal-300">
                      {color}
                    </span>
                  ))}
                </div>
              </div>
            </div>

            {/* Audio */}
            <div className="mt-4 pt-3 border-t border-gray-700">
              <div className="flex items-center gap-2 text-sm text-gray-300">
                <Music size={14} className="text-amber-400" />
                <span>{selectedTemplate.audio_plan.music.genre}</span>
              </div>
            </div>
          </div>

          {/* Scene List */}
          <div className="bg-gray-800 rounded-xl border border-gray-700 p-4">
            <h3 className="text-lg font-semibold text-white mb-4 flex items-center gap-2">
              <Film size={18} className="text-cyan-400" /> 장면 구성
            </h3>

            <div className="space-y-2 max-h-[400px] overflow-y-auto pr-2 custom-scrollbar">
              {selectedTemplate.scenes.map((scene) => (
                <div key={scene.id} className="bg-gray-900/50 rounded-lg border border-gray-700 overflow-hidden">
                  {/* Scene Header */}
                  <button
                    onClick={() => toggleScene(scene.id)}
                    className="w-full px-4 py-3 flex items-center justify-between hover:bg-gray-700/30 transition-colors"
                  >
                    <div className="flex items-center gap-3">
                      <span className="text-2xl">{getFlagEmoji(scene.country.flag)}</span>
                      <div className="text-left">
                        <span className="font-semibold text-white">{scene.country.name_ko}</span>
                        <span className="ml-2 text-sm text-gray-500">
                          {formatDuration(scene.time_range_s.start)} - {formatDuration(scene.time_range_s.end)}
                        </span>
                      </div>
                    </div>
                    <div className="flex items-center gap-2">
                      <span className="text-xs text-gray-500">{scene.shots.length}개 샷</span>
                      {expandedScene === scene.id ? (
                        <ChevronUp size={18} className="text-gray-400" />
                      ) : (
                        <ChevronDown size={18} className="text-gray-400" />
                      )}
                    </div>
                  </button>

                  {/* Expanded Scene Details */}
                  {expandedScene === scene.id && (
                    <div className="px-4 pb-4 space-y-3 border-t border-gray-700 pt-3">
                      <div className="text-sm text-cyan-400 mb-2">
                        <Waves size={14} className="inline mr-1" />
                        {scene.theme_monster}
                      </div>

                      {scene.shots.map((shot, idx) => (
                        <div key={shot.shot_id} className="bg-gray-800 rounded-lg p-3 border border-gray-700">
                          <div className="flex justify-between items-start mb-2">
                            <span className="text-xs font-mono text-gray-500">{shot.shot_id}</span>
                            <span className="text-xs text-gray-500">{shot.duration_s}s</span>
                          </div>
                          <p className="text-sm text-gray-300 mb-2">{shot.prompt}</p>
                          <p className="text-xs text-gray-500 italic">
                            <Play size={10} className="inline mr-1" />
                            {shot.camera}
                          </p>
                        </div>
                      ))}

                      {scene.subtitles.length > 0 && (
                        <div className="text-xs text-gray-400 mt-2">
                          <span className="text-yellow-400">자막:</span>
                          {scene.subtitles.map((sub, i) => (
                            <span key={i} className="ml-2">"{sub.text}" @ {sub.t}s</span>
                          ))}
                        </div>
                      )}
                    </div>
                  )}
                </div>
              ))}
            </div>
          </div>

          {/* Action Buttons */}
          <div className="flex gap-4 pt-4">
            <button
              onClick={onCancel}
              className="flex-1 py-3 bg-gray-700 hover:bg-gray-600 text-white rounded-xl font-semibold transition-colors"
            >
              취소
            </button>
            <button
              onClick={handleApply}
              disabled={isApplying}
              className="flex-1 py-3 bg-gradient-to-r from-cyan-600 to-blue-600 hover:from-cyan-500 hover:to-blue-500 disabled:from-gray-600 disabled:to-gray-600 text-white rounded-xl font-semibold transition-all flex items-center justify-center gap-2 shadow-lg shadow-cyan-500/20"
            >
              {isApplying ? (
                <>
                  <div className="w-5 h-5 border-2 border-white/30 border-t-white rounded-full animate-spin" />
                  적용 중...
                </>
              ) : (
                <>
                  템플릿 적용 <ArrowRight size={18} />
                </>
              )}
            </button>
          </div>
        </div>
      </div>

      {/* Remix Hints Section */}
      <div className="mt-8 bg-gradient-to-r from-gray-800 to-gray-800/50 rounded-xl border border-gray-700 p-6">
        <h3 className="text-lg font-semibold text-white mb-4 flex items-center gap-2">
          <Sparkles size={18} className="text-yellow-400" /> 리믹스 힌트
        </h3>
        <p className="text-sm text-gray-400 mb-4">이 템플릿을 변형하여 새로운 버전을 만들 수 있습니다:</p>

        <div className="grid md:grid-cols-2 gap-4">
          <div>
            <h4 className="text-sm font-semibold text-gray-300 mb-2">변경 가능 요소</h4>
            <div className="flex flex-wrap gap-2">
              {selectedTemplate.remix_knobs.what_to_change_for_new_version.map((item, i) => (
                <span key={i} className="px-2 py-1 bg-yellow-900/20 border border-yellow-700/30 rounded text-xs text-yellow-300">
                  {item}
                </span>
              ))}
            </div>
          </div>

          <div>
            <h4 className="text-sm font-semibold text-gray-300 mb-2">새 국가 예시</h4>
            <div className="flex flex-wrap gap-2">
              {selectedTemplate.remix_knobs.countries_list_example_new.slice(0, 4).map((country, i) => (
                <span key={i} className="px-2 py-1 bg-gray-700 rounded text-xs text-gray-300 flex items-center gap-1">
                  {getFlagEmoji(country.flag)} {country.name_ko}
                </span>
              ))}
            </div>
          </div>
        </div>
      </div>
    </div>
  );
};

export default TemplateSelector;
