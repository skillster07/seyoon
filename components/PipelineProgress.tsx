import React from "react";
import { PipelineState, PipelineStep } from "../types";
import { Search, Brain, Eye, ImagePlus, Check, Loader2 } from "lucide-react";

interface Props {
  state: PipelineState;
}

const STEPS = [
  { key: PipelineStep.SEARCHING_NEWS, label: "뉴스 검색", icon: Search },
  { key: PipelineStep.ANALYZING, label: "AI 분석", icon: Brain },
  { key: PipelineStep.REVIEW, label: "검토/편집", icon: Eye },
  { key: PipelineStep.GENERATING_MEDIA, label: "미디어 생성", icon: ImagePlus },
  { key: PipelineStep.COMPLETE, label: "완료", icon: Check },
];

const stepOrder = [
  PipelineStep.IDLE,
  PipelineStep.SEARCHING_NEWS,
  PipelineStep.ANALYZING,
  PipelineStep.REVIEW,
  PipelineStep.GENERATING_MEDIA,
  PipelineStep.COMPLETE,
];

export const PipelineProgress: React.FC<Props> = ({ state }) => {
  const currentIdx = stepOrder.indexOf(state.step);

  return (
    <div className="w-full bg-gray-800/50 rounded-xl p-6 border border-gray-700">
      <div className="flex items-center justify-between mb-4">
        {STEPS.map((step, i) => {
          const stepIdx = stepOrder.indexOf(step.key);
          const isActive = state.step === step.key;
          const isDone = currentIdx > stepIdx;
          const Icon = step.icon;

          return (
            <React.Fragment key={step.key}>
              <div className="flex flex-col items-center gap-2">
                <div
                  className={`w-10 h-10 rounded-full flex items-center justify-center transition-all ${
                    isActive
                      ? "bg-blue-600 text-white ring-4 ring-blue-600/30 animate-pulse"
                      : isDone
                      ? "bg-green-600 text-white"
                      : "bg-gray-700 text-gray-500"
                  }`}
                >
                  {isActive && state.step !== PipelineStep.COMPLETE ? (
                    <Loader2 size={18} className="animate-spin" />
                  ) : (
                    <Icon size={18} />
                  )}
                </div>
                <span
                  className={`text-xs font-medium ${
                    isActive
                      ? "text-blue-400"
                      : isDone
                      ? "text-green-400"
                      : "text-gray-500"
                  }`}
                >
                  {step.label}
                </span>
              </div>
              {i < STEPS.length - 1 && (
                <div
                  className={`flex-1 h-0.5 mx-2 mt-[-20px] ${
                    isDone ? "bg-green-600" : "bg-gray-700"
                  }`}
                />
              )}
            </React.Fragment>
          );
        })}
      </div>

      {state.message && (
        <p className="text-sm text-gray-400 text-center mt-2">
          {state.message}
        </p>
      )}

      {state.error && (
        <p className="text-sm text-red-400 text-center mt-2 bg-red-900/20 rounded-lg p-2">
          {state.error}
        </p>
      )}
    </div>
  );
};
