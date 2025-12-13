import React, { useState } from 'react';
import { generateStory } from '../services/geminiService';
import { Character } from '../types';
import { PenTool, Sparkles, ArrowRight, BookOpen } from 'lucide-react';

interface StoryWriterProps {
  characters: Character[];
  onScriptGenerated: (script: string) => void;
}

export const StoryWriter: React.FC<StoryWriterProps> = ({ characters, onScriptGenerated }) => {
  const [topic, setTopic] = useState('');
  const [isGenerating, setIsGenerating] = useState(false);
  const [generatedScript, setGeneratedScript] = useState('');

  const handleGenerate = async () => {
    if (!topic.trim()) return;
    setIsGenerating(true);
    setGeneratedScript('');
    
    try {
      const script = await generateStory(topic, characters);
      setGeneratedScript(script);
    } catch (e) {
      console.error(e);
      alert("스토리 생성에 실패했습니다.");
    } finally {
      setIsGenerating(false);
    }
  };

  return (
    <div className="max-w-4xl mx-auto animate-in fade-in duration-500 h-full flex flex-col">
      <div className="text-center mb-6">
        <h2 className="text-3xl font-bold text-white mb-2 flex items-center justify-center gap-3">
          <PenTool className="text-primary-500" /> 스토리 생성기
        </h2>
        <p className="text-gray-400">"붉은 정원" 에피소드 주제를 입력하면 AI가 시나리오와 연출 프롬프트를 작성합니다.</p>
      </div>

      <div className="flex-grow grid md:grid-cols-2 gap-6 min-h-[500px]">
        {/* Input Side */}
        <div className="flex flex-col gap-4">
          <div className="bg-gray-800 p-6 rounded-xl border border-gray-700 flex-grow flex flex-col">
            <label className="block text-sm font-medium text-gray-300 mb-2">스토리 요청</label>
            <textarea 
              value={topic}
              onChange={(e) => setTopic(e.target.value)}
              className="flex-grow w-full bg-gray-900 border border-gray-600 rounded-lg p-4 text-white focus:border-primary-500 focus:outline-none resize-none mb-4"
              placeholder="예: EP2 스토리 만들어줘. 남주와 여주가 처음으로 격식 있는 저녁 식사를 하는데, 여주의 전 남자친구가 나타나서 갈등이 생기는 내용."
            />
            
            <div className="bg-gray-900/50 rounded-lg p-3 mb-4">
              <div className="text-xs font-semibold text-gray-500 mb-2 uppercase">참조할 캐릭터 ({characters.length})</div>
              <div className="flex flex-wrap gap-2">
                {characters.length > 0 ? characters.map(c => (
                  <span key={c.id} className="text-xs bg-gray-800 text-gray-300 px-2 py-1 rounded border border-gray-700">
                    {c.name}
                  </span>
                )) : (
                  <span className="text-xs text-gray-600 italic">등록된 캐릭터 없음 (캐릭터 탭에서 추가 가능)</span>
                )}
              </div>
            </div>

            <button
              onClick={handleGenerate}
              disabled={!topic.trim() || isGenerating}
              className={`w-full py-3 rounded-lg font-bold text-white flex items-center justify-center gap-2 transition-all
                ${!topic.trim() || isGenerating
                  ? 'bg-gray-700 cursor-not-allowed text-gray-500' 
                  : 'bg-gradient-to-r from-primary-600 to-rose-600 hover:shadow-lg hover:shadow-primary-500/20'}`}
            >
              {isGenerating ? (
                <>
                  <Sparkles className="animate-spin" size={20} /> 제작 어시스턴트 집필 중...
                </>
              ) : (
                <>
                  <Sparkles size={20} /> 에피소드 생성하기
                </>
              )}
            </button>
          </div>
        </div>

        {/* Output Side */}
        <div className="bg-gray-800 p-6 rounded-xl border border-gray-700 flex flex-col relative overflow-hidden">
          <div className="flex justify-between items-center mb-4">
            <h3 className="font-semibold text-white flex items-center gap-2">
              <BookOpen size={18} /> 생성된 에피소드 플랜
            </h3>
          </div>
          
          <div className="flex-grow bg-gray-900 rounded-lg p-4 text-sm text-gray-300 overflow-y-auto whitespace-pre-wrap font-sans border border-gray-800 relative">
            {generatedScript ? generatedScript : (
              <div className="absolute inset-0 flex items-center justify-center text-gray-600 text-center p-4">
                왼쪽에서 주제를 입력하세요.<br/>(예: "EP2 스토리 만들어줘")
              </div>
            )}
          </div>

          {generatedScript && (
            <button
              onClick={() => onScriptGenerated(generatedScript)}
              className="mt-4 w-full bg-indigo-600 hover:bg-indigo-500 text-white py-3 rounded-lg font-bold flex items-center justify-center gap-2 shadow-lg transition-all animate-in slide-in-from-bottom-2"
            >
              스토리보드로 시각화하기 <ArrowRight size={20} />
            </button>
          )}
        </div>
      </div>
    </div>
  );
};