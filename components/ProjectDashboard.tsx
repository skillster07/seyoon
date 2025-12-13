import React, { useState, useRef } from 'react';
import { Project } from '../types';
import { FolderPlus, FileJson, Trash2, Clock, Edit2, Upload, ArrowRight } from 'lucide-react';

interface ProjectDashboardProps {
  projects: Project[];
  onCreateProject: (title: string) => void;
  onSelectProject: (project: Project) => void;
  onDeleteProject: (id: string) => void;
  onImportProject: (file: File) => void;
}

export const ProjectDashboard: React.FC<ProjectDashboardProps> = ({
  projects,
  onCreateProject,
  onSelectProject,
  onDeleteProject,
  onImportProject
}) => {
  const [newProjectTitle, setNewProjectTitle] = useState('');
  const fileInputRef = useRef<HTMLInputElement>(null);

  const handleCreate = (e: React.FormEvent) => {
    e.preventDefault();
    if (newProjectTitle.trim()) {
      onCreateProject(newProjectTitle);
      setNewProjectTitle('');
    }
  };

  const handleFileChange = (e: React.ChangeEvent<HTMLInputElement>) => {
    const file = e.target.files?.[0];
    if (file) {
      onImportProject(file);
      if (fileInputRef.current) fileInputRef.current.value = '';
    }
  };

  return (
    <div className="max-w-6xl mx-auto px-4 py-12 animate-in fade-in duration-500">
      <div className="text-center mb-12">
        <h1 className="text-4xl font-bold bg-clip-text text-transparent bg-gradient-to-r from-white to-gray-400 mb-4">
          프로젝트 관리
        </h1>
        <p className="text-gray-400">에피소드별로 프로젝트를 생성하고 관리하세요.</p>
      </div>

      {/* New Project Input */}
      <div className="max-w-xl mx-auto mb-16">
        <form onSubmit={handleCreate} className="relative group">
          <input
            type="text"
            value={newProjectTitle}
            onChange={(e) => setNewProjectTitle(e.target.value)}
            placeholder="새 프로젝트 이름 (예: Ep.1 붉은 정원의 시작)"
            className="w-full bg-gray-800 border-2 border-gray-700 rounded-2xl py-4 px-6 text-lg text-white placeholder-gray-500 focus:outline-none focus:border-primary-500 focus:ring-4 focus:ring-primary-500/20 transition-all"
          />
          <button
            type="submit"
            disabled={!newProjectTitle.trim()}
            className="absolute right-2 top-2 bottom-2 bg-primary-600 hover:bg-primary-500 disabled:bg-gray-700 disabled:text-gray-500 text-white px-6 rounded-xl font-bold transition-all flex items-center gap-2"
          >
            <FolderPlus size={20} /> 생성
          </button>
        </form>
        
        <div className="flex justify-center mt-4">
           <label className="flex items-center gap-2 text-sm text-gray-400 hover:text-primary-400 cursor-pointer transition-colors">
              <Upload size={16} /> JSON 프로젝트 파일 불러오기 (.json)
              <input 
                type="file" 
                ref={fileInputRef} 
                onChange={handleFileChange} 
                accept=".json" 
                className="hidden" 
              />
           </label>
        </div>
      </div>

      {/* Project Grid */}
      <div className="grid md:grid-cols-2 lg:grid-cols-3 gap-6">
        {projects.length === 0 ? (
          <div className="col-span-full text-center py-20 border-2 border-dashed border-gray-800 rounded-2xl">
            <p className="text-gray-600">생성된 프로젝트가 없습니다.</p>
          </div>
        ) : (
          projects.sort((a, b) => b.lastModified - a.lastModified).map((project) => (
            <div
              key={project.id}
              onClick={() => onSelectProject(project)}
              className="group bg-gray-800 hover:bg-gray-750 border border-gray-700 hover:border-primary-500/50 rounded-2xl p-6 transition-all shadow-lg hover:shadow-2xl hover:shadow-primary-500/10 flex flex-col h-64 relative overflow-hidden cursor-pointer"
            >
              <div className="flex-1 z-10">
                <div className="flex justify-between items-start mb-4">
                   <h3 className="text-xl font-bold text-white group-hover:text-primary-400 transition-colors line-clamp-2 pr-8">
                     {project.title}
                   </h3>
                   <div className="absolute top-6 right-6 flex gap-2" onClick={(e) => e.stopPropagation()}>
                      <button
                        onClick={(e) => {
                           e.stopPropagation(); // Stop bubbling to card click
                           if(window.confirm(`'${project.title}' 프로젝트를 삭제하시겠습니까?`)) {
                             onDeleteProject(project.id);
                           }
                        }}
                        className="text-gray-500 hover:text-red-500 p-2 hover:bg-gray-700/50 rounded-lg transition-colors"
                        title="삭제"
                      >
                        <Trash2 size={18} />
                      </button>
                   </div>
                </div>
                
                <div className="space-y-2 text-sm text-gray-400">
                  <div className="flex items-center gap-2">
                    <Clock size={14} /> 
                    <span>수정: {new Date(project.lastModified).toLocaleDateString()}</span>
                  </div>
                  <div className="flex items-center gap-2">
                    <Edit2 size={14} /> 
                    <span>장면: {project.data.scenes.length}개</span>
                  </div>
                </div>
              </div>

              <div className="mt-auto pt-4 border-t border-gray-700 flex justify-end z-10">
                <span
                  className="w-full py-3 bg-gray-700 group-hover:bg-primary-600 text-white rounded-xl font-bold flex items-center justify-center gap-2 transition-all opacity-90 group-hover:opacity-100"
                >
                  편집하기 <ArrowRight size={18} />
                </span>
              </div>
              
              {/* Decorative gradient */}
              <div className="absolute top-0 right-0 w-32 h-32 bg-primary-500/5 rounded-full blur-3xl -translate-y-1/2 translate-x-1/2 group-hover:bg-primary-500/10 transition-colors z-0"></div>
            </div>
          ))
        )}
      </div>
    </div>
  );
};