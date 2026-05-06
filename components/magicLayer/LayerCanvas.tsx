import React, { useEffect, useRef, useState } from 'react';
import { Stage, Layer as KLayer, Image as KImage, Text as KText, Transformer } from 'react-konva';
import Konva from 'konva';
import { Layer } from '../../types';

interface LayerCanvasProps {
  width: number;            // intrinsic image width
  height: number;           // intrinsic image height
  layers: Layer[];          // already z-sorted ascending
  selectedId: string | null;
  onSelect: (id: string | null) => void;
  onTransform: (id: string, patch: Partial<Layer['transform']>) => void;
  onTextEdit: (id: string, newText: string) => void;
  stageRef: React.MutableRefObject<Konva.Stage | null>;
  containerWidth: number;   // available pixel width to fit
  containerHeight: number;
}

function useImageEl(src?: string): HTMLImageElement | null {
  const [img, setImg] = useState<HTMLImageElement | null>(null);
  useEffect(() => {
    if (!src) { setImg(null); return; }
    const el = new window.Image();
    el.crossOrigin = 'anonymous';
    el.onload = () => setImg(el);
    el.onerror = () => setImg(null);
    el.src = src;
    return () => { el.onload = null; el.onerror = null; };
  }, [src]);
  return img;
}

interface ObjectNodeProps {
  layer: Layer;
  isSelected: boolean;
  onSelect: () => void;
  onChange: (patch: Partial<Layer['transform']>) => void;
}

const ObjectNode: React.FC<ObjectNodeProps> = ({ layer, isSelected, onSelect, onChange }) => {
  const img = useImageEl(layer.imageDataUrl);
  const ref = useRef<Konva.Image | null>(null);
  if (!img) return null;
  const t = layer.transform;
  return (
    <KImage
      ref={(node) => { ref.current = node; }}
      image={img}
      x={t.x}
      y={t.y}
      width={t.width}
      height={t.height}
      rotation={t.rotation}
      scaleX={t.scaleX}
      scaleY={t.scaleY}
      opacity={t.opacity * (layer.visible ? 1 : 0)}
      draggable={!layer.locked}
      listening={!layer.locked && layer.visible}
      onMouseDown={onSelect}
      onTap={onSelect}
      name={`layer-${layer.id}`}
      onDragEnd={(e) => onChange({ x: e.target.x(), y: e.target.y() })}
      onTransformEnd={(e) => {
        const node = e.target as Konva.Image;
        onChange({
          x: node.x(),
          y: node.y(),
          rotation: node.rotation(),
          scaleX: node.scaleX(),
          scaleY: node.scaleY(),
        });
      }}
    />
  );
};

interface TextNodeProps {
  layer: Layer;
  isSelected: boolean;
  onSelect: () => void;
  onChange: (patch: Partial<Layer['transform']>) => void;
  onDoubleClick: () => void;
}

const TextNode: React.FC<TextNodeProps> = ({ layer, onSelect, onChange, onDoubleClick }) => {
  const t = layer.transform;
  return (
    <KText
      text={layer.text || ''}
      x={t.x}
      y={t.y}
      width={t.width}
      rotation={t.rotation}
      scaleX={t.scaleX}
      scaleY={t.scaleY}
      opacity={t.opacity * (layer.visible ? 1 : 0)}
      fontFamily={layer.fontFamily || 'sans-serif'}
      fontSize={layer.fontSize || 24}
      fill={layer.color || '#ffffff'}
      align={layer.textAlign || 'left'}
      draggable={!layer.locked}
      listening={!layer.locked && layer.visible}
      onMouseDown={onSelect}
      onTap={onSelect}
      onDblClick={onDoubleClick}
      onDblTap={onDoubleClick}
      name={`layer-${layer.id}`}
      onDragEnd={(e) => onChange({ x: e.target.x(), y: e.target.y() })}
      onTransformEnd={(e) => {
        const node = e.target as Konva.Text;
        onChange({
          x: node.x(),
          y: node.y(),
          rotation: node.rotation(),
          scaleX: node.scaleX(),
          scaleY: node.scaleY(),
        });
      }}
    />
  );
};

export const LayerCanvas: React.FC<LayerCanvasProps> = ({
  width,
  height,
  layers,
  selectedId,
  onSelect,
  onTransform,
  onTextEdit,
  stageRef,
  containerWidth,
  containerHeight,
}) => {
  const transformerRef = useRef<Konva.Transformer | null>(null);
  const stageInternalRef = useRef<Konva.Stage | null>(null);
  const [editingTextId, setEditingTextId] = useState<string | null>(null);

  const scale = Math.min(containerWidth / width, containerHeight / height) || 1;
  const stageW = width * scale;
  const stageH = height * scale;

  useEffect(() => {
    const stage = stageInternalRef.current;
    const tr = transformerRef.current;
    if (!stage || !tr) return;
    if (!selectedId) { tr.nodes([]); tr.getLayer()?.batchDraw(); return; }
    const node = stage.findOne(`.layer-${selectedId}`);
    if (node) {
      tr.nodes([node as Konva.Node]);
      tr.getLayer()?.batchDraw();
    } else {
      tr.nodes([]);
    }
  }, [selectedId, layers]);

  const handleStageMouseDown = (e: Konva.KonvaEventObject<MouseEvent | TouchEvent>) => {
    if (e.target === e.target.getStage()) {
      onSelect(null);
    }
  };

  const editingLayer = editingTextId ? layers.find((l) => l.id === editingTextId) : null;

  return (
    <div style={{ position: 'relative', width: stageW, height: stageH }}>
      <Stage
        ref={(s) => { stageInternalRef.current = s; stageRef.current = s; }}
        width={stageW}
        height={stageH}
        scaleX={scale}
        scaleY={scale}
        onMouseDown={handleStageMouseDown}
        onTouchStart={handleStageMouseDown}
        style={{ background: '#111827', borderRadius: 8 }}
      >
        <KLayer>
          {layers.map((layer) => {
            const isSelected = layer.id === selectedId;
            const handleSelect = () => { if (!layer.locked) onSelect(layer.id); };
            const handleChange = (patch: Partial<Layer['transform']>) => onTransform(layer.id, patch);
            if (layer.type === 'text') {
              return (
                <TextNode
                  key={layer.id}
                  layer={layer}
                  isSelected={isSelected}
                  onSelect={handleSelect}
                  onChange={handleChange}
                  onDoubleClick={() => setEditingTextId(layer.id)}
                />
              );
            }
            return (
              <ObjectNode
                key={layer.id}
                layer={layer}
                isSelected={isSelected}
                onSelect={handleSelect}
                onChange={handleChange}
              />
            );
          })}
          <Transformer
            ref={(node) => { transformerRef.current = node; }}
            rotateEnabled
            anchorSize={10}
            borderStroke="#f43f5e"
            anchorStroke="#f43f5e"
            anchorFill="#fff"
            boundBoxFunc={(oldBox, newBox) => (newBox.width < 8 || newBox.height < 8 ? oldBox : newBox)}
          />
        </KLayer>
      </Stage>

      {editingLayer && (
        <textarea
          autoFocus
          defaultValue={editingLayer.text || ''}
          onBlur={(e) => {
            onTextEdit(editingLayer.id, e.target.value);
            setEditingTextId(null);
          }}
          onKeyDown={(e) => {
            if (e.key === 'Escape') { setEditingTextId(null); }
            if (e.key === 'Enter' && (e.metaKey || e.ctrlKey)) {
              onTextEdit(editingLayer.id, (e.target as HTMLTextAreaElement).value);
              setEditingTextId(null);
            }
          }}
          style={{
            position: 'absolute',
            left: editingLayer.transform.x * scale,
            top: editingLayer.transform.y * scale,
            width: Math.max(80, editingLayer.transform.width * editingLayer.transform.scaleX * scale),
            minHeight: Math.max(24, (editingLayer.fontSize || 24) * editingLayer.transform.scaleY * scale + 8),
            fontFamily: editingLayer.fontFamily || 'sans-serif',
            fontSize: (editingLayer.fontSize || 24) * scale,
            color: editingLayer.color || '#fff',
            background: 'rgba(17,24,39,0.95)',
            border: '1px solid #f43f5e',
            borderRadius: 4,
            padding: 4,
            resize: 'both',
            outline: 'none',
            zIndex: 50,
          }}
        />
      )}
    </div>
  );
};

export default LayerCanvas;
