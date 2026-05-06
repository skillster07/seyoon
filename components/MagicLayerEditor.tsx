import React, { useEffect, useMemo, useRef, useState } from 'react';
import { v4 as uuidv4 } from 'uuid';
import Konva from 'konva';
import { Layer, LayerComposition, Scene } from '../types';
import {
  applyMaskToImage,
  boxToPixels,
  cropImage,
  detectionToObjectLayer,
  detectionToTextLayer,
  loadImage,
  makeBackgroundLayer,
  newComposition,
} from '../services/layerService';
import { detectObjectsAndText, generateMaskForBox } from '../services/segmentationService';
import LayerCanvas from './magicLayer/LayerCanvas';
import LayerPanel from './magicLayer/LayerPanel';
import LayerToolbar from './magicLayer/LayerToolbar';

interface MagicLayerEditorProps {
  scene: Scene;
  onClose: () => void;
  onSave: (newImageDataUrl: string, composition: LayerComposition) => void;
}

export const MagicLayerEditor: React.FC<MagicLayerEditorProps> = ({ scene, onClose, onSave }) => {
  const [composition, setComposition] = useState<LayerComposition | null>(null);
  const [selectedId, setSelectedId] = useState<string | null>(null);
  const [isAnalyzing, setIsAnalyzing] = useState(false);
  const [isSaving, setIsSaving] = useState(false);
  const [cutoutEnabled, setCutoutEnabled] = useState(true);
  const [progressLabel, setProgressLabel] = useState<string | undefined>();
  const [error, setError] = useState<string | null>(null);

  const stageRef = useRef<Konva.Stage | null>(null);
  const containerRef = useRef<HTMLDivElement | null>(null);
  const [containerSize, setContainerSize] = useState({ w: 800, h: 500 });

  // Load source image once and initialize composition
  useEffect(() => {
    let cancelled = false;
    if (!scene.imageUrl) return;
    (async () => {
      try {
        const img = await loadImage(scene.imageUrl!);
        if (cancelled) return;
        setComposition(newComposition(scene.imageUrl!, img.width, img.height));
      } catch (e) {
        console.error(e);
        setError('이미지를 불러올 수 없습니다.');
      }
    })();
    return () => { cancelled = true; };
  }, [scene.imageUrl]);

  // Track container size for fit-to-container scaling
  useEffect(() => {
    if (!containerRef.current) return;
    const el = containerRef.current;
    const update = () => {
      setContainerSize({ w: el.clientWidth, h: el.clientHeight });
    };
    update();
    const ro = new ResizeObserver(update);
    ro.observe(el);
    return () => ro.disconnect();
  }, []);

  const sortedLayers = useMemo(
    () => (composition ? [...composition.layers].sort((a, b) => a.zIndex - b.zIndex) : []),
    [composition],
  );

  const selectedLayer = useMemo(
    () => sortedLayers.find((l) => l.id === selectedId) || null,
    [sortedLayers, selectedId],
  );

  function updateLayers(updater: (prev: Layer[]) => Layer[]) {
    setComposition((c) => (c ? { ...c, layers: updater(c.layers) } : c));
  }

  const handleAnalyze = async () => {
    if (!composition || !scene.imageUrl) return;
    setIsAnalyzing(true);
    setError(null);
    setProgressLabel('객체/텍스트 감지 중...');
    try {
      const { objects, texts } = await detectObjectsAndText(scene.imageUrl);
      const img = await loadImage(scene.imageUrl);
      const imgW = img.width;
      const imgH = img.height;

      // Phase 1: rectangular crops
      const baseZ = composition.layers.length;
      const objectLayers: Layer[] = [];
      for (let i = 0; i < objects.length; i++) {
        const o = objects[i];
        const { x, y, w, h } = boxToPixels(o.box, imgW, imgH);
        const cropped = await cropImage(img, x, y, w, h);
        objectLayers.push(detectionToObjectLayer(o, imgW, imgH, cropped, baseZ + i + 1));
      }
      const textLayers: Layer[] = texts.map((t, i) =>
        detectionToTextLayer(t, imgW, imgH, baseZ + objectLayers.length + i + 1),
      );

      // Apply Phase 1 result first so user sees something fast
      setComposition({
        ...composition,
        layers: [
          makeBackgroundLayer(scene.imageUrl, imgW, imgH),
          ...objectLayers,
          ...textLayers,
        ],
      });

      // Phase 2: refine object layers with masks (optional)
      if (cutoutEnabled && objectLayers.length > 0) {
        for (let i = 0; i < objectLayers.length; i++) {
          const o = objects[i];
          const layer = objectLayers[i];
          setProgressLabel(`컷아웃 ${i + 1}/${objectLayers.length}: ${o.label}`);
          try {
            const maskBase64 = await generateMaskForBox(scene.imageUrl, o.box, o.label);
            // Crop the source for this box, then apply same-size mask
            const { x, y, w, h } = boxToPixels(o.box, imgW, imgH);
            const cropped = await cropImage(img, x, y, w, h);
            const cutout = await applyMaskToImage(cropped, maskBase64);
            updateLayers((prev) =>
              prev.map((l) =>
                l.id === layer.id
                  ? { ...l, imageDataUrl: cutout, maskDataUrl: `data:image/png;base64,${maskBase64}` }
                  : l,
              ),
            );
          } catch (e) {
            console.warn(`Mask failed for ${o.label}`, e);
          }
        }
      }

      setProgressLabel(undefined);
    } catch (e) {
      console.error(e);
      setError(e instanceof Error ? e.message : '분석에 실패했습니다.');
    } finally {
      setIsAnalyzing(false);
    }
  };

  const handleAddText = () => {
    if (!composition) return;
    const z = Math.max(0, ...composition.layers.map((l) => l.zIndex)) + 1;
    const newLayer: Layer = {
      id: uuidv4(),
      type: 'text',
      label: 'text-new',
      zIndex: z,
      visible: true,
      locked: false,
      transform: {
        x: composition.width * 0.1,
        y: composition.height * 0.1,
        width: composition.width * 0.4,
        height: composition.height * 0.1,
        rotation: 0,
        scaleX: 1,
        scaleY: 1,
        opacity: 1,
      },
      text: '텍스트 입력',
      fontFamily: 'Pretendard, system-ui, sans-serif',
      fontSize: composition.height * 0.06,
      color: '#ffffff',
      textAlign: 'left',
    };
    updateLayers((prev) => [...prev, newLayer]);
    setSelectedId(newLayer.id);
  };

  const handleTransform = (id: string, patch: Partial<Layer['transform']>) => {
    updateLayers((prev) =>
      prev.map((l) => (l.id === id ? { ...l, transform: { ...l.transform, ...patch } } : l)),
    );
  };

  const handleTextEdit = (id: string, newText: string) => {
    updateLayers((prev) => prev.map((l) => (l.id === id ? { ...l, text: newText } : l)));
  };

  const handleToggleVisible = (id: string) => {
    updateLayers((prev) => prev.map((l) => (l.id === id ? { ...l, visible: !l.visible } : l)));
  };

  const handleToggleLocked = (id: string) => {
    updateLayers((prev) => prev.map((l) => (l.id === id ? { ...l, locked: !l.locked } : l)));
    if (selectedId === id) setSelectedId(null);
  };

  const handleMove = (id: string, dir: 'up' | 'down') => {
    if (!composition) return;
    const sorted = [...composition.layers].sort((a, b) => a.zIndex - b.zIndex);
    const idx = sorted.findIndex((l) => l.id === id);
    if (idx < 0) return;
    const target = dir === 'up' ? idx + 1 : idx - 1;
    if (target < 1 || target >= sorted.length) return; // can't go below background (z=0 reserved)
    const a = sorted[idx];
    const b = sorted[target];
    updateLayers((prev) =>
      prev.map((l) => {
        if (l.id === a.id) return { ...l, zIndex: b.zIndex };
        if (l.id === b.id) return { ...l, zIndex: a.zIndex };
        return l;
      }),
    );
  };

  const handleDelete = (id: string) => {
    updateLayers((prev) => prev.filter((l) => l.id !== id));
    if (selectedId === id) setSelectedId(null);
  };

  const handleSave = async () => {
    if (!composition || !stageRef.current) return;
    setIsSaving(true);
    try {
      const stage = stageRef.current;
      // Deselect transformer for clean export
      const prevSelected = selectedId;
      setSelectedId(null);
      await new Promise((r) => requestAnimationFrame(() => r(null)));

      const stageW = stage.width();
      const pixelRatio = stageW > 0 ? composition.width / stageW : 1;
      const dataUrl = stage.toDataURL({ pixelRatio });
      onSave(dataUrl, composition);
      setSelectedId(prevSelected);
    } catch (e) {
      console.error(e);
      setError('저장 중 오류가 발생했습니다.');
    } finally {
      setIsSaving(false);
    }
  };

  if (!scene.imageUrl) {
    return (
      <div className="fixed inset-0 z-50 bg-black/80 flex items-center justify-center">
        <div className="bg-gray-900 p-6 rounded-lg text-gray-200">
          이 Scene에는 이미지가 없습니다. 먼저 이미지를 생성하세요.
          <button onClick={onClose} className="ml-4 px-3 py-1 bg-rose-600 rounded text-white">닫기</button>
        </div>
      </div>
    );
  }

  return (
    <div className="fixed inset-0 z-50 bg-black/85 flex flex-col">
      <LayerToolbar
        onAnalyze={handleAnalyze}
        onAddText={handleAddText}
        onSave={handleSave}
        onClose={onClose}
        isAnalyzing={isAnalyzing}
        isSaving={isSaving}
        cutoutEnabled={cutoutEnabled}
        onToggleCutout={setCutoutEnabled}
        progressLabel={progressLabel}
      />
      {error && (
        <div className="px-4 py-2 bg-rose-900/40 border-b border-rose-800 text-rose-200 text-xs">
          {error}
        </div>
      )}
      <div className="flex-1 flex min-h-0">
        <LayerPanel
          layers={sortedLayers}
          selectedId={selectedId}
          onSelect={setSelectedId}
          onToggleVisible={handleToggleVisible}
          onToggleLocked={handleToggleLocked}
          onMove={handleMove}
          onDelete={handleDelete}
        />

        <div ref={containerRef} className="flex-1 flex items-center justify-center p-4 bg-gray-950 overflow-hidden">
          {composition && (
            <LayerCanvas
              width={composition.width}
              height={composition.height}
              layers={sortedLayers}
              selectedId={selectedId}
              onSelect={setSelectedId}
              onTransform={handleTransform}
              onTextEdit={handleTextEdit}
              stageRef={stageRef}
              containerWidth={containerSize.w - 32}
              containerHeight={containerSize.h - 32}
            />
          )}
        </div>

        <div className="w-72 bg-gray-900 border-l border-gray-700 p-4 overflow-y-auto">
          <h3 className="text-xs font-semibold text-gray-300 uppercase tracking-wider mb-3">속성</h3>
          {!selectedLayer && <div className="text-xs text-gray-500">레이어를 선택하세요.</div>}
          {selectedLayer && (
            <div className="space-y-3">
              <div className="text-xs text-gray-400">
                <span className="text-gray-500">레이블:</span> {selectedLayer.label}
              </div>
              {selectedLayer.type === 'text' && (
                <>
                  <div>
                    <label className="block text-xs text-gray-400 mb-1">내용</label>
                    <textarea
                      value={selectedLayer.text || ''}
                      onChange={(e) => handleTextEdit(selectedLayer.id, e.target.value)}
                      className="w-full bg-gray-800 border border-gray-700 rounded p-2 text-sm text-gray-100 h-20 resize-none"
                    />
                  </div>
                  <div>
                    <label className="block text-xs text-gray-400 mb-1">폰트 크기: {Math.round(selectedLayer.fontSize || 24)}</label>
                    <input
                      type="range"
                      min={8}
                      max={Math.round(composition!.height * 0.5)}
                      value={Math.round(selectedLayer.fontSize || 24)}
                      onChange={(e) => updateLayers((prev) =>
                        prev.map((l) => l.id === selectedLayer.id ? { ...l, fontSize: Number(e.target.value) } : l),
                      )}
                      className="w-full accent-rose-500"
                    />
                  </div>
                  <div>
                    <label className="block text-xs text-gray-400 mb-1">색상</label>
                    <input
                      type="color"
                      value={selectedLayer.color || '#ffffff'}
                      onChange={(e) => updateLayers((prev) =>
                        prev.map((l) => l.id === selectedLayer.id ? { ...l, color: e.target.value } : l),
                      )}
                      className="w-full h-8 rounded bg-gray-800 border border-gray-700"
                    />
                  </div>
                </>
              )}
              <div className="text-[10px] text-gray-500 grid grid-cols-2 gap-1 pt-2 border-t border-gray-800">
                <span>x: {Math.round(selectedLayer.transform.x)}</span>
                <span>y: {Math.round(selectedLayer.transform.y)}</span>
                <span>w: {Math.round(selectedLayer.transform.width)}</span>
                <span>h: {Math.round(selectedLayer.transform.height)}</span>
                <span>scale: {selectedLayer.transform.scaleX.toFixed(2)}</span>
                <span>rot: {Math.round(selectedLayer.transform.rotation)}°</span>
              </div>
            </div>
          )}
        </div>
      </div>
    </div>
  );
};

export default MagicLayerEditor;
