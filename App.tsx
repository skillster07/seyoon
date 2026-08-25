import React, { useRef, useState } from 'react';
import {
  Aperture, Blend, Camera, Check, ChevronDown, ChevronLeft, ChevronRight, CircleHelp,
  Copy, Eye, EyeOff, Image, Layers3, Lock, Maximize2, Mic2, Minus, MonitorUp,
  MoreHorizontal, Move, Palette, Plus, Radio, RotateCcw, Settings, SlidersHorizontal,
  Sparkles, Square, Trash2, UserRound, Video, WandSparkles, Wifi, X, Zap
} from 'lucide-react';
import './styles.css';
import { OutputPlatform, SourceType, useStudioProject } from './studioState';

const outputProfiles = [
  { id: 'soop' as const, name: 'SOOP', detail: '고화질 가로 방송', width: 1920, height: 1080, fps: 60 as const, bitrate: 8000 },
  { id: 'tiktok' as const, name: 'TikTok LIVE', detail: '모바일 세로 방송', width: 1080, height: 1920, fps: 60 as const, bitrate: 6000 },
  { id: 'obs' as const, name: 'OBS', detail: '사용자 지정 제작', width: 1920, height: 1080, fps: 60 as const, bitrate: 8000 },
];

type SliderRowProps = { label: string; value: number; onChange: (value: number) => void; min?: number; max?: number };
const SliderRow = ({ label, value, onChange, min = 0, max = 100 }: SliderRowProps) => (
  <div className="slider-row">
    <div><span>{label}</span><button onClick={() => onChange(0)}><RotateCcw size={11} /></button><b>{value}</b></div>
    <input type="range" min={min} max={max} value={value} onChange={(e) => onChange(Number(e.target.value))} />
  </div>
);

const App: React.FC = () => {
  const { project, activeLayers: layers, savedAt, update, setBeauty, setOutput, resetBeautyGroup, addScene, renameScene, removeScene, addLayer, toggleLayer, removeLayer } = useStudioProject();
  const { scenes, activeScene, beauty, orientation, background, output } = project;
  const [activeLayer, setActiveLayer] = useState<number | null>(1);
  const [sideTab, setSideTab] = useState<'beauty' | 'adjust' | 'effect'>('beauty');
  const [beautyTab, setBeautyTab] = useState<'face' | 'makeup' | 'body'>('face');
  const [zoom, setZoom] = useState(72);
  const [outputOn, setOutputOn] = useState(true);
  const [showOutput, setShowOutput] = useState(false);
  const [showSourcePicker, setShowSourcePicker] = useState(false);
  const [showOutputSettings, setShowOutputSettings] = useState(false);
  const [showSceneMenu, setShowSceneMenu] = useState<number | null>(null);
  const [cameraActive, setCameraActive] = useState(false);
  const videoRef = useRef<HTMLVideoElement>(null);

  const applyOutputProfile = (platform: OutputPlatform) => {
    const profile = outputProfiles.find((item) => item.id === platform)!;
    setOutput({ platform, width: profile.width, height: profile.height, fps: profile.fps, bitrate: profile.bitrate });
    update({ orientation: profile.height > profile.width ? 'portrait' : 'landscape' });
  };

  const startCamera = async () => {
    if (cameraActive) {
      const stream = videoRef.current?.srcObject as MediaStream | null;
      stream?.getTracks().forEach((track) => track.stop());
      if (videoRef.current) videoRef.current.srcObject = null;
      setCameraActive(false);
      return;
    }
    try {
      const stream = await navigator.mediaDevices.getUserMedia({ video: true, audio: false });
      if (videoRef.current) videoRef.current.srcObject = stream;
      setCameraActive(true);
    } catch {
      alert('카메라 권한을 확인해 주세요. 현재는 데모 이미지로 계속 사용할 수 있습니다.');
    }
  };



  return (
    <div className="studio-app">
      <header className="app-bar">
        <div className="app-brand"><span><Aperture size={20} /></span><strong>VIVID<span>CAM</span></strong></div>
        <div className="project-name"><button><ChevronLeft size={18} /></button><div><small>라이브 프로젝트</small><strong>{project.name}</strong></div><button><ChevronDown size={15} /></button></div>
        <div className="app-status"><span><i /> {Date.now() - savedAt < 2000 ? '저장됨' : '자동 저장'}</span><button><CircleHelp size={17} /> 도움말</button><button><Settings size={17} /></button><button className="account">YS</button></div>
      </header>

      <div className="main-layout">
        <aside className="tool-rail">
          <button className="active"><WandSparkles /><span>뷰티</span></button>
          <button><Palette /><span>효과</span></button>
          <button><Image /><span>배경</span></button>
          <button><Layers3 /><span>소재</span></button>
          <button><Sparkles /><span>템플릿</span></button>
          <div className="rail-line" />
          <button><Zap /><span>마켓</span><em>NEW</em></button>
        </aside>

        <aside className="scene-panel">
          <div className="panel-title"><div><strong>장면</strong><span>{scenes.length}</span></div><button onClick={addScene}><Plus size={16} /></button></div>
          <div className="scene-list">
            {scenes.map((scene, index) => (
              <React.Fragment key={scene.id}><button className={`scene-item ${activeScene === scene.id ? 'active' : ''}`} onClick={() => update({ activeScene: scene.id })}>
                <span className={`scene-thumb ${scene.thumb}`}><i>{scene.thumb === 'wait' ? '잠시 후 시작합니다' : <UserRound size={21} />}</i></span>
                <span className="scene-meta"><small>{String(index + 1).padStart(2, '0')}</small><strong>{scene.name}</strong></span>
                <MoreHorizontal size={15} onClick={(e) => { e.stopPropagation(); setShowSceneMenu(showSceneMenu === scene.id ? null : scene.id); }} />
              </button>
              {showSceneMenu === scene.id && <div className="scene-menu"><button onClick={() => { const name = prompt('장면 이름', scene.name); if (name?.trim()) renameScene(scene.id, name.trim()); setShowSceneMenu(null); }}>이름 변경</button><button onClick={() => { removeScene(scene.id); setShowSceneMenu(null); }}>장면 삭제</button></div>}
            </React.Fragment>))}
          </div>
          <div className="source-title"><div><strong>소스</strong><span>{layers.length}</span></div><button onClick={() => setShowSourcePicker(true)}><Plus size={16} /></button></div>
          <div className="layer-list">
            {layers.map((layer) => (
              <div key={layer.id} className={`layer-item ${activeLayer === layer.id ? 'active' : ''}`} onClick={() => setActiveLayer(layer.id)}>
                <button onClick={(e) => { e.stopPropagation(); toggleLayer(layer.id, 'visible'); }}>{layer.visible ? <Eye size={14} /> : <EyeOff size={14} />}</button>
                <span>{layer.type === 'camera' ? <Camera size={15} /> : layer.type === 'image' ? <Image size={15} /> : <Square size={14} />}</span>
                <p>{layer.name}</p>
                <button onClick={(e) => { e.stopPropagation(); toggleLayer(layer.id, 'locked'); }}>{layer.locked ? <Lock size={13} /> : <MoreHorizontal size={14} />}</button>
              </div>
            ))}
          </div>
          <div className="layer-actions"><button onClick={() => setShowSourcePicker(true)}><Plus /></button><button><Copy /></button><button onClick={() => activeLayer && removeLayer(activeLayer)}><Trash2 /></button></div>
        </aside>

        <main className="canvas-workspace">
          <div className="canvas-toolbar">
            <div className="device-select"><span><Camera size={15} /></span><div><small>카메라 소스</small><strong>Sony ZV-E10</strong></div><ChevronDown size={14} /></div>
            <div className="toolbar-center">
              <button><Move size={15} /> 선택</button><i />
              <button><ChevronLeft size={15} /></button><button><ChevronRight size={15} /></button>
            </div>
            <div className="orientation-switch">
              <button className={orientation === 'portrait' ? 'active' : ''} onClick={() => update({ orientation: 'portrait' })}><span className="phone-icon" /> 세로</button>
              <button className={orientation === 'landscape' ? 'active' : ''} onClick={() => update({ orientation: 'landscape' })}><span className="landscape-icon" /> 가로</button>
            </div>
          </div>

          <div className="canvas-zone">
            <div className={`broadcast-canvas ${orientation} bg-${background}`}>
              <img className="demo-camera" src="/demo-presenter.jpg" alt="카메라 데모" />
              <video ref={videoRef} className={cameraActive ? 'camera-feed visible' : 'camera-feed'} autoPlay playsInline muted />
              <div className="beauty-wash" style={{ opacity: beauty.smooth / 400 }} />
              <div className="camera-frame"><i className="tl" /><i className="tr" /><i className="bl" /><i className="br" /></div>
              {layers.find((l) => l.id === 3)?.visible && <div className="canvas-caption"><small>YOONSEUL'S LIVE</small><strong>오늘도 반가워요 ✨</strong></div>}
              <div className="canvas-live"><i /> LIVE READY</div>
              <div className="safe-zone">TikTok 안전 영역</div>
            </div>
            <div className="zoom-control"><button onClick={() => setZoom(Math.max(25, zoom - 10))}><Minus size={14} /></button><input type="range" min="25" max="100" value={zoom} onChange={(e) => setZoom(Number(e.target.value))} /><span>{zoom}%</span><button onClick={() => setZoom(Math.min(100, zoom + 10))}><Plus size={14} /></button><button><Maximize2 size={14} /></button></div>
          </div>

          <div className="monitor-strip">
            <button className={cameraActive ? 'monitor-card active' : 'monitor-card'} onClick={startCamera}><span><Video size={17} /></span><div><small>카메라</small><strong>{cameraActive ? '사용 중' : '데모 화면'}</strong></div><i className={cameraActive ? 'on' : ''} /></button>
            <div className="monitor-card"><span><Mic2 size={17} /></span><div><small>마이크</small><strong>MacBook Microphone</strong></div><div className="meter"><b /><b /><b /><b /></div></div>
            <div className="performance"><span><Wifi size={14} /> 안정적</span><p>CPU <b>18%</b></p><p>GPU <b>24%</b></p><p>FPS <b>30</b></p></div>
          </div>
        </main>

        <aside className="property-panel">
          <div className="property-tabs">
            <button className={sideTab === 'beauty' ? 'active' : ''} onClick={() => setSideTab('beauty')}><Sparkles size={16} /> 뷰티</button>
            <button className={sideTab === 'adjust' ? 'active' : ''} onClick={() => setSideTab('adjust')}><SlidersHorizontal size={16} /> 조정</button>
            <button className={sideTab === 'effect' ? 'active' : ''} onClick={() => setSideTab('effect')}><Blend size={16} /> 효과</button>
          </div>
          {sideTab === 'adjust' && <div className="quick-panel"><h3>영상 조정</h3><p>카메라 입력의 색감과 선명도를 빠르게 보정하세요.</p><SliderRow label="밝기" value={12} onChange={() => {}} min={-50} max={50} /><SliderRow label="대비" value={8} onChange={() => {}} min={-50} max={50} /><SliderRow label="채도" value={16} onChange={() => {}} min={-50} max={50} /></div>}
          {sideTab === 'effect' && <div className="quick-panel"><h3>배경 스타일</h3><p>캔버스 분위기를 선택하면 프로젝트에 자동 저장됩니다.</p><div className="background-grid">{['violet','midnight','sunset','mint'].map((item) => <button key={item} className={`${item} ${background === item ? 'active' : ''}`} onClick={() => update({ background: item })}><i />{item === 'violet' ? '바이올렛' : item === 'midnight' ? '미드나잇' : item === 'sunset' ? '선셋' : '민트'}</button>)}</div></div>}
          {sideTab === 'beauty' && <><div className="beauty-tabs">
            <button className={beautyTab === 'face' ? 'active' : ''} onClick={() => setBeautyTab('face')}>얼굴</button>
            <button className={beautyTab === 'makeup' ? 'active' : ''} onClick={() => setBeautyTab('makeup')}>메이크업</button>
            <button className={beautyTab === 'body' ? 'active' : ''} onClick={() => setBeautyTab('body')}>바디</button>
          </div>
          <div className="preset-section">
            <div className="section-label"><strong>추천 프리셋</strong><button>전체보기 <ChevronRight size={12} /></button></div>
            <div className="preset-grid">
              {['내추럴', '화사하게', '스튜디오', '소프트'].map((name, i) => <button key={name} className={i === 0 ? 'active' : ''}><span className={`preset-face p${i + 1}`}><UserRound size={23} /></span><small>{name}</small>{i === 0 && <i><Check size={9} /></i>}</button>)}
            </div>
          </div>
          <div className="control-section">
            <div className="section-label"><strong>피부</strong><button onClick={() => { resetBeautyGroup(['smooth', 'tone', 'blemish', 'sharpness']); }}><RotateCcw size={12} /> 초기화</button></div>
            <SliderRow label="매끄럽게" value={beauty.smooth} onChange={(v) => setBeauty('smooth', v)} />
            <SliderRow label="피부 톤" value={beauty.tone} onChange={(v) => setBeauty('tone', v)} />
            <SliderRow label="잡티 제거" value={beauty.blemish} onChange={(v) => setBeauty('blemish', v)} />
            <SliderRow label="선명도" value={beauty.sharpness} onChange={(v) => setBeauty('sharpness', v)} />
          </div>
          <div className="control-section">
            <div className="section-label"><strong>얼굴 형태</strong><button onClick={() => { resetBeautyGroup(['jaw', 'eyes', 'chin']); }}><RotateCcw size={12} /> 초기화</button></div>
            <SliderRow label="얼굴 슬림" value={beauty.jaw} onChange={(v) => setBeauty('jaw', v)} min={-50} max={50} />
            <SliderRow label="눈 크기" value={beauty.eyes} onChange={(v) => setBeauty('eyes', v)} min={-50} max={50} />
            <SliderRow label="턱 라인" value={beauty.chin} onChange={(v) => setBeauty('chin', v)} min={-50} max={50} />
          </div></>}
        </aside>
      </div>

      <footer className="output-bar">
        <div className="output-device"><span><Radio size={18} /></span><div><small>가상 카메라 출력</small><strong>VIVIDCAM Virtual Camera</strong></div><button onClick={() => setOutputOn(!outputOn)} className={outputOn ? 'toggle on' : 'toggle'}><i /></button></div>
        <div className="output-info"><span>{output.width} × {output.height}</span><i /> <span className="fps-highlight">{output.fps} FPS</span><i /><span>{output.bitrate.toLocaleString()} Kbps</span><i /><span>색공간 sRGB</span></div>
        <div className="footer-actions"><button onClick={() => setShowOutputSettings(true)}><SlidersHorizontal size={16} /> 출력 설정</button><button className="primary" onClick={() => setShowOutput(true)}><Zap size={17} /> 가상 카메라 시작</button></div>
      </footer>

      {showSourcePicker && <div className="modal-backdrop" onClick={() => setShowSourcePicker(false)}><div className="source-modal" onClick={(e) => e.stopPropagation()}><div className="source-modal-head"><div><h2>소스 추가</h2><p>현재 장면에 추가할 입력 소스를 선택하세요.</p></div><button onClick={() => setShowSourcePicker(false)}><X size={18} /></button></div><div className="source-options">{([['camera','카메라','USB·캡처 카드 입력',Camera],['screen','화면 캡처','앱 또는 디스플레이',MonitorUp],['image','이미지·배경','로컬 이미지 파일',Image],['text','텍스트','제목과 방송 자막',Square]] as [SourceType,string,string,React.ElementType][]).map(([type,title,desc,Icon]) => <button key={type} onClick={() => { addLayer(type, title); setShowSourcePicker(false); }}><span><Icon size={20} /></span><div><strong>{title}</strong><small>{desc}</small></div><ChevronRight size={15} /></button>)}</div></div></div>}
      {showOutputSettings && <div className="modal-backdrop" onClick={() => setShowOutputSettings(false)}><div className="output-settings-modal" onClick={(e) => e.stopPropagation()}><div className="source-modal-head"><div><h2>출력 설정</h2><p>플랫폼 권장 프로필은 모두 60p를 기본으로 사용합니다.</p></div><button onClick={() => setShowOutputSettings(false)}><X size={18} /></button></div><div className="profile-list">{outputProfiles.map((profile) => <button key={profile.id} className={output.platform === profile.id ? 'active' : ''} onClick={() => applyOutputProfile(profile.id)}><span className={`platform-mark ${profile.id}`}>{profile.name[0]}</span><div><strong>{profile.name}</strong><small>{profile.detail}</small></div><b>{profile.width} × {profile.height}<em>{profile.fps}p</em></b>{output.platform === profile.id && <Check size={16} />}</button>)}</div><div className="output-fields"><label><span>프레임 레이트</span><select value={output.fps} onChange={(e) => setOutput({ fps: Number(e.target.value) as 30 | 60 })}><option value="60">60 FPS (권장)</option><option value="30">30 FPS</option></select></label><label><span>비트레이트</span><select value={output.bitrate} onChange={(e) => setOutput({ bitrate: Number(e.target.value) })}><option value="8000">8,000 Kbps</option><option value="6000">6,000 Kbps</option><option value="4500">4,500 Kbps</option></select></label><label><span>하드웨어 인코더</span><select value={output.encoder} onChange={(e) => setOutput({ encoder: e.target.value as typeof output.encoder })}><option value="auto">자동 선택</option><option value="nvenc">NVIDIA NVENC</option><option value="qsv">Intel Quick Sync</option><option value="amf">AMD AMF</option></select></label></div><div className="sixty-note"><Zap size={16} /><div><strong>60p 방송 최적화</strong><p>빠른 움직임과 댄스·게임 방송에서 더 부드러운 화면을 제공합니다.</p></div></div><button className="modal-done" onClick={() => setShowOutputSettings(false)}>설정 적용</button></div></div>}
      {showOutput && <div className="modal-backdrop" onClick={() => setShowOutput(false)}><div className="output-modal" onClick={(e) => e.stopPropagation()}><button className="modal-close" onClick={() => setShowOutput(false)}><X size={18} /></button><span className="success-icon"><Check size={28} /></span><h2>가상 카메라가 준비됐어요</h2><p>방송 프로그램에서 카메라 장치로<br/><strong>VIVIDCAM Virtual Camera</strong>를 선택하세요.</p><div className="readiness"><span><Check size={12} /> 카메라 입력</span><span><Check size={12} /> {output.width} × {output.height}</span><span><Check size={12} /> {output.fps} FPS</span></div><div className="platforms"><span className={output.platform === 'tiktok' ? 'selected' : ''}>TikTok LIVE</span><span className={output.platform === 'obs' ? 'selected' : ''}>OBS</span><span className={output.platform === 'soop' ? 'selected' : ''}>SOOP</span></div><button className="modal-done" onClick={() => setShowOutput(false)}>확인</button></div></div>}
    </div>
  );
};

export default App;
