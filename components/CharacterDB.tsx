import React, { useState } from 'react';
import { v4 as uuidv4 } from 'uuid';
import { Character } from '../types';
import { generateImageFromPrompt } from '../services/geminiService';
import { Users, Plus, Trash2, User, Camera, Sparkles, Image as ImageIcon, Smile, Frown, Meh, AlertCircle } from 'lucide-react';

interface CharacterDBProps {
  characters: Character[];
  setCharacters: React.Dispatch<React.SetStateAction<Character[]>>;
}

const EXPRESSIONS = [
  { key: 'main', label: '기본 프로필', prompt: 'detailed character portrait, looking at camera, neutral expression', icon: <User size={14} /> },
  { key: 'anxious', label: '불안', prompt: 'worried anxious expression, sweat drops, nervous atmosphere', icon: <AlertCircle size={14} /> },
  { key: 'surprised', label: '놀람', prompt: 'shocked surprised expression, eyes wide open, mouth slightly open', icon: <AlertCircle size={14} /> },
  { key: 'tense', label: '긴장', prompt: 'nervous tense expression, serious face, cinematic lighting', icon: <Meh size={14} /> },
  { key: 'joy', label: '기쁨', prompt: 'gentle smile, warm expression, happy atmosphere', icon: <Smile size={14} /> },
  { key: 'sad', label: '슬픔', prompt: 'sad melancholic expression, teary eyes, emotional atmosphere', icon: <Frown size={14} /> },
];

export const CharacterDB: React.FC<CharacterDBProps> = ({ characters, setCharacters }) => {
  // Input States
  const [newName, setNewName] = useState('');
  const [newAge, setNewAge] = useState('');
  const [newDesc, setNewDesc] = useState('');
  const [newVisual, setNewVisual] = useState('');
  const [newBasePrompt, setNewBasePrompt] = useState('');

  // Loading state for specific images: key format `${charId}-${expressionKey}`
  const [loadingImages, setLoadingImages] = useState<{ [key: string]: boolean }>({});
  // Selected preview image for each character: key format `${charId}` -> expressionKey
  const [selectedPreview, setSelectedPreview] = useState<{ [key: string]: string }>({});

  const handleAddCharacter = () => {
    if (!newName.trim()) return;
    const newChar: Character = {
      id: uuidv4(),
      name: newName,
      age: newAge,
      description: newDesc,
      visualStyle: newVisual,
      basePrompt: newBasePrompt,
      images: {}
    };
    setCharacters([...characters, newChar]);
    
    // Reset inputs
    setNewName('');
    setNewAge('');
    setNewDesc('');
    setNewVisual('');
    setNewBasePrompt('');
  };

  const handleDelete = (id: string) => {
    setCharacters(characters.filter(c => c.id !== id));
  };

  const handleGenerateImage = async (char: Character, exprKey: string, exprPrompt: string) => {
    const loadingKey = `${char.id}-${exprKey}`;
    setLoadingImages(prev => ({ ...prev, [loadingKey]: true }));
    
    // Select this view immediately so user sees the spinner
    setSelectedPreview(prev => ({ ...prev, [char.id]: exprKey }));

    // Construct detailed prompt
    const fullPrompt = `
      ${char.basePrompt || ''}, 
      ${char.visualStyle}, 
      ${char.age ? `${char.age} years old` : ''}, 
      ${exprPrompt}, 
      modern romance webtoon style, anime style, masterpiece, best quality, 8k, cinematic lighting
    `.trim();

    try {
      const imageUrl = await generateImageFromPrompt(fullPrompt);
      setCharacters(prev => prev.map(c => {
        if (c.id === char.id) {
          return { ...c, images: { ...c.images, [exprKey]: imageUrl } };
        }
        return c;
      }));
    } catch (error) {
      console.error("Character image generation failed", error);
      alert("이미지 생성에 실패했습니다.");
    } finally {
      setLoadingImages(prev => ({ ...prev, [loadingKey]: false }));
    }
  };

  return (
    <div className="max-w-6xl mx-auto animate-in fade-in duration-500">
      <div className="text-center mb-8">
        <h2 className="text-3xl font-bold text-white mb-2 flex items-center justify-center gap-3">
          <Users className="text-primary-500" /> 캐릭터 데이터베이스
        </h2>
        <p className="text-gray-400">"붉은 정원" 등장인물의 상세 정보와 다양한 표정을 생성하여 저장하세요.</p>
      </div>

      <div className="grid lg:grid-cols-12 gap-8">
        {/* Input Form (Left Side - 4 Columns) */}
        <div className="lg:col-span-4 bg-gray-800 p-6 rounded-xl border border-gray-700 h-fit sticky top-6 shadow-xl">
          <h3 className="text-lg font-bold text-white mb-4 flex items-center gap-2">
            <Plus size={18} className="text-primary-500" /> 새 캐릭터 등록
          </h3>
          <div className="space-y-4">
            <div className="grid grid-cols-2 gap-3">
              <div>
                <label className="block text-xs font-medium text-gray-400 mb-1">이름</label>
                <input 
                  value={newName}
                  onChange={(e) => setNewName(e.target.value)}
                  className="w-full bg-gray-900 border border-gray-600 rounded p-2 text-white focus:border-primary-500 focus:outline-none text-sm"
                  placeholder="예: 한지우"
                />
              </div>
              <div>
                <label className="block text-xs font-medium text-gray-400 mb-1">나이</label>
                <input 
                  value={newAge}
                  onChange={(e) => setNewAge(e.target.value)}
                  className="w-full bg-gray-900 border border-gray-600 rounded p-2 text-white focus:border-primary-500 focus:outline-none text-sm"
                  placeholder="예: 25"
                />
              </div>
            </div>

            <div>
              <label className="block text-xs font-medium text-gray-400 mb-1">성격 및 역할</label>
              <textarea 
                value={newDesc}
                onChange={(e) => setNewDesc(e.target.value)}
                className="w-full bg-gray-900 border border-gray-600 rounded p-2 text-white focus:border-primary-500 focus:outline-none h-16 resize-none text-sm"
                placeholder="예: 회사원, 아버지 빚으로 계약 결혼, 당차지만 속은 여림"
              />
            </div>

            <div className="pt-2 border-t border-gray-700">
               <p className="text-xs font-semibold text-primary-400 mb-2">이미지 생성 설정</p>
               <div>
                <label className="block text-xs font-medium text-gray-400 mb-1">외형 특징 (시각화용)</label>
                <textarea 
                  value={newVisual}
                  onChange={(e) => setNewVisual(e.target.value)}
                  className="w-full bg-gray-900 border border-gray-600 rounded p-2 text-white focus:border-primary-500 focus:outline-none h-16 resize-none text-sm"
                  placeholder="예: 갈색 긴 생머리, 큰 눈, 오피스룩, 단정한 인상"
                />
              </div>
              <div className="mt-2">
                <label className="block text-xs font-medium text-gray-400 mb-1">기본 프롬프트 (Base Prompt)</label>
                <input 
                  value={newBasePrompt}
                  onChange={(e) => setNewBasePrompt(e.target.value)}
                  className="w-full bg-gray-900 border border-gray-600 rounded p-2 text-white focus:border-primary-500 focus:outline-none text-sm"
                  placeholder="예: A korean woman with long brown hair, office look"
                />
              </div>
            </div>

            <button 
              onClick={handleAddCharacter}
              disabled={!newName.trim()}
              className="w-full mt-2 bg-primary-600 hover:bg-primary-700 disabled:bg-gray-700 disabled:text-gray-500 text-white py-2.5 rounded-lg flex items-center justify-center gap-2 transition-colors font-bold shadow-lg"
            >
              <Plus size={16} /> 캐릭터 추가
            </button>
          </div>
        </div>

        {/* List (Right Side - 8 Columns) */}
        <div className="lg:col-span-8 grid gap-6 content-start">
          {characters.length === 0 ? (
            <div className="text-center py-20 border-2 border-dashed border-gray-700 rounded-xl bg-gray-800/50">
              <div className="w-20 h-20 bg-gray-800 rounded-full flex items-center justify-center mx-auto mb-4">
                 <User className="text-gray-600" size={40} />
              </div>
              <h3 className="text-xl font-bold text-gray-400">등록된 캐릭터가 없습니다</h3>
              <p className="text-gray-500 mt-2">왼쪽 양식을 통해 새로운 캐릭터를 추가해주세요.</p>
            </div>
          ) : (
            characters.map(char => {
              const currentPreviewKey = selectedPreview[char.id] || 'main';
              const currentImage = char.images[currentPreviewKey];
              const isLoading = loadingImages[`${char.id}-${currentPreviewKey}`];

              return (
                <div key={char.id} className="bg-gray-800 rounded-xl border border-gray-700 overflow-hidden shadow-lg flex flex-col md:flex-row">
                  
                  {/* Left: Image Preview Area */}
                  <div className="w-full md:w-64 h-64 md:h-auto bg-black relative flex-shrink-0 border-b md:border-b-0 md:border-r border-gray-700">
                    {currentImage ? (
                      <div className="w-full h-full relative group">
                        <img src={currentImage} alt={char.name} className="w-full h-full object-cover" />
                        <div className="absolute inset-0 bg-gradient-to-t from-black/80 to-transparent opacity-0 group-hover:opacity-100 transition-opacity flex items-end p-2">
                           <span className="text-white text-xs font-medium">
                             {EXPRESSIONS.find(e => e.key === currentPreviewKey)?.label}
                           </span>
                        </div>
                      </div>
                    ) : (
                      <div className="w-full h-full flex flex-col items-center justify-center text-gray-600 p-4 text-center">
                        {isLoading ? (
                           <>
                             <div className="w-8 h-8 border-2 border-primary-500 border-t-transparent rounded-full animate-spin mb-2"></div>
                             <span className="text-xs text-primary-400">생성 중...</span>
                           </>
                        ) : (
                           <>
                             <ImageIcon size={32} className="mb-2 opacity-50" />
                             <span className="text-xs">이미지가 없습니다</span>
                           </>
                        )}
                      </div>
                    )}
                  </div>

                  {/* Right: Info & Controls */}
                  <div className="flex-1 p-5 flex flex-col">
                    <div className="flex justify-between items-start mb-2">
                      <div>
                        <div className="flex items-center gap-2">
                           <h4 className="text-2xl font-bold text-white">{char.name}</h4>
                           {char.age && <span className="text-sm bg-gray-700 text-gray-300 px-2 py-0.5 rounded">{char.age}세</span>}
                        </div>
                        <p className="text-sm text-gray-400 mt-1 line-clamp-2">{char.description}</p>
                      </div>
                      <button 
                        onClick={() => handleDelete(char.id)}
                        className="text-gray-500 hover:text-red-400 p-1.5 hover:bg-red-900/20 rounded-lg transition-colors"
                        title="삭제"
                      >
                        <Trash2 size={18} />
                      </button>
                    </div>

                    <div className="bg-gray-900/50 rounded-lg p-3 mb-4 text-xs text-gray-400">
                       <span className="text-primary-500 font-semibold mr-1">외형:</span> {char.visualStyle}
                    </div>

                    <div className="mt-auto">
                      <p className="text-xs font-semibold text-gray-500 uppercase mb-2 flex items-center gap-1">
                        <Camera size={12} /> 표정 생성 및 미리보기
                      </p>
                      <div className="grid grid-cols-3 sm:grid-cols-6 gap-2">
                        {EXPRESSIONS.map((expr) => {
                          const hasImage = !!char.images[expr.key];
                          const isSelected = currentPreviewKey === expr.key;
                          const isExprLoading = loadingImages[`${char.id}-${expr.key}`];

                          return (
                            <button
                              key={expr.key}
                              onClick={() => {
                                setSelectedPreview(prev => ({ ...prev, [char.id]: expr.key }));
                                if (!hasImage) {
                                  handleGenerateImage(char, expr.key, expr.prompt);
                                }
                              }}
                              disabled={isExprLoading}
                              className={`
                                relative flex flex-col items-center justify-center p-2 rounded-lg border transition-all
                                ${isSelected 
                                  ? 'bg-primary-900/30 border-primary-500 text-primary-200' 
                                  : 'bg-gray-700/50 border-gray-600 text-gray-400 hover:bg-gray-700'}
                                ${hasImage ? 'ring-1 ring-green-500/30' : ''}
                              `}
                            >
                              <div className="mb-1">{expr.icon}</div>
                              <span className="text-[10px] font-medium whitespace-nowrap">{expr.label}</span>
                              
                              {/* Status Indicators */}
                              {isExprLoading && (
                                <div className="absolute inset-0 bg-black/60 flex items-center justify-center rounded-lg">
                                  <div className="w-3 h-3 border-2 border-white border-t-transparent rounded-full animate-spin"></div>
                                </div>
                              )}
                              {hasImage && !isExprLoading && !isSelected && (
                                <div className="absolute top-1 right-1 w-1.5 h-1.5 bg-green-500 rounded-full"></div>
                              )}
                            </button>
                          );
                        })}
                      </div>
                    </div>
                  </div>
                </div>
              );
            })
          )}
        </div>
      </div>
    </div>
  );
};