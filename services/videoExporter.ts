import { FFmpeg } from '@ffmpeg/ffmpeg';
import { fetchFile, toBlobURL } from '@ffmpeg/util';
import { Scene } from '../types';

export interface ExportProgress {
  stage: 'loading' | 'processing' | 'encoding' | 'complete' | 'error';
  progress: number; // 0-100
  currentScene?: number;
  totalScenes?: number;
  message: string;
}

export type ProgressCallback = (progress: ExportProgress) => void;

class VideoExporterService {
  private ffmpeg: FFmpeg | null = null;
  private loaded = false;

  async load(onProgress?: ProgressCallback): Promise<void> {
    if (this.loaded && this.ffmpeg) return;

    onProgress?.({
      stage: 'loading',
      progress: 0,
      message: 'FFmpeg 초기화 중...'
    });

    this.ffmpeg = new FFmpeg();

    this.ffmpeg.on('log', ({ message }) => {
      console.log('[FFmpeg]', message);
    });

    this.ffmpeg.on('progress', ({ progress }) => {
      onProgress?.({
        stage: 'encoding',
        progress: Math.round(progress * 100),
        message: `인코딩 중... ${Math.round(progress * 100)}%`
      });
    });

    // Load FFmpeg WASM
    const baseURL = 'https://unpkg.com/@ffmpeg/core@0.12.6/dist/esm';
    await this.ffmpeg.load({
      coreURL: await toBlobURL(`${baseURL}/ffmpeg-core.js`, 'text/javascript'),
      wasmURL: await toBlobURL(`${baseURL}/ffmpeg-core.wasm`, 'application/wasm'),
    });

    this.loaded = true;
    onProgress?.({
      stage: 'loading',
      progress: 100,
      message: 'FFmpeg 로드 완료'
    });
  }

  /**
   * Convert base64 data URL to Uint8Array
   */
  private base64ToUint8Array(base64: string): Uint8Array {
    const data = base64.split(',')[1] || base64;
    const binaryString = atob(data);
    const bytes = new Uint8Array(binaryString.length);
    for (let i = 0; i < binaryString.length; i++) {
      bytes[i] = binaryString.charCodeAt(i);
    }
    return bytes;
  }

  /**
   * Fetch blob URL content as Uint8Array
   */
  private async fetchBlobAsUint8Array(url: string): Promise<Uint8Array> {
    const response = await fetch(url);
    const blob = await response.blob();
    const arrayBuffer = await blob.arrayBuffer();
    return new Uint8Array(arrayBuffer);
  }

  /**
   * Export scenes to MP4 video
   */
  async exportToMp4(
    scenes: Scene[],
    bgmUrl?: string,
    onProgress?: ProgressCallback
  ): Promise<Blob> {
    if (!this.ffmpeg || !this.loaded) {
      await this.load(onProgress);
    }

    const ffmpeg = this.ffmpeg!;
    const validScenes = scenes.filter(s => s.imageUrl || s.videoUrl);

    if (validScenes.length === 0) {
      throw new Error('내보낼 장면이 없습니다.');
    }

    onProgress?.({
      stage: 'processing',
      progress: 0,
      currentScene: 0,
      totalScenes: validScenes.length,
      message: '미디어 파일 준비 중...'
    });

    const sceneFiles: string[] = [];
    const audioFiles: string[] = [];
    let concatContent = '';

    // Process each scene
    for (let i = 0; i < validScenes.length; i++) {
      const scene = validScenes[i];
      const sceneIndex = i.toString().padStart(3, '0');

      onProgress?.({
        stage: 'processing',
        progress: Math.round((i / validScenes.length) * 50),
        currentScene: i + 1,
        totalScenes: validScenes.length,
        message: `장면 ${i + 1}/${validScenes.length} 처리 중...`
      });

      if (scene.videoUrl) {
        // Handle video scene
        const videoData = await this.fetchBlobAsUint8Array(scene.videoUrl);
        const videoFileName = `scene_${sceneIndex}.mp4`;
        await ffmpeg.writeFile(videoFileName, videoData);

        // Re-encode video to ensure consistent format
        const outputFileName = `processed_${sceneIndex}.mp4`;
        await ffmpeg.exec([
          '-i', videoFileName,
          '-c:v', 'libx264',
          '-preset', 'fast',
          '-crf', '23',
          '-t', scene.duration.toString(),
          '-r', '30',
          '-s', '1280x720',
          '-an', // Remove audio, we'll add it later
          '-y',
          outputFileName
        ]);

        sceneFiles.push(outputFileName);
        concatContent += `file '${outputFileName}'\n`;

      } else if (scene.imageUrl) {
        // Handle image scene - create video from still image
        let imageData: Uint8Array;

        if (scene.imageUrl.startsWith('data:')) {
          imageData = this.base64ToUint8Array(scene.imageUrl);
        } else {
          imageData = await this.fetchBlobAsUint8Array(scene.imageUrl);
        }

        const imageFileName = `image_${sceneIndex}.png`;
        await ffmpeg.writeFile(imageFileName, imageData);

        // Convert image to video with duration
        const outputFileName = `processed_${sceneIndex}.mp4`;

        // Apply Ken Burns effect based on transition type
        let filterComplex = '';
        if (scene.transition === 'zoom') {
          filterComplex = `scale=1920:1080,zoompan=z='min(zoom+0.001,1.2)':x='iw/2-(iw/zoom/2)':y='ih/2-(ih/zoom/2)':d=${scene.duration * 30}:s=1280x720`;
        } else {
          filterComplex = `scale=1280:720:force_original_aspect_ratio=decrease,pad=1280:720:(ow-iw)/2:(oh-ih)/2`;
        }

        await ffmpeg.exec([
          '-loop', '1',
          '-i', imageFileName,
          '-vf', filterComplex,
          '-c:v', 'libx264',
          '-t', scene.duration.toString(),
          '-pix_fmt', 'yuv420p',
          '-r', '30',
          '-y',
          outputFileName
        ]);

        sceneFiles.push(outputFileName);
        concatContent += `file '${outputFileName}'\n`;
      }

      // Process scene audio (TTS)
      if (scene.audioUrl) {
        const audioData = await this.fetchBlobAsUint8Array(scene.audioUrl);
        const audioFileName = `audio_${sceneIndex}.wav`;
        await ffmpeg.writeFile(audioFileName, audioData);
        audioFiles.push(audioFileName);
      }
    }

    onProgress?.({
      stage: 'encoding',
      progress: 50,
      message: '영상 합치는 중...'
    });

    // Write concat file
    await ffmpeg.writeFile('concat.txt', concatContent);

    // Concatenate all video segments
    await ffmpeg.exec([
      '-f', 'concat',
      '-safe', '0',
      '-i', 'concat.txt',
      '-c', 'copy',
      '-y',
      'merged_video.mp4'
    ]);

    onProgress?.({
      stage: 'encoding',
      progress: 70,
      message: '오디오 믹싱 중...'
    });

    // Mix audio tracks
    let finalOutput = 'merged_video.mp4';

    // Create mixed audio if we have any audio files
    if (audioFiles.length > 0 || bgmUrl) {
      const audioInputs: string[] = [];
      const filterParts: string[] = [];
      let inputIndex = 0;

      // Add merged video as first input
      audioInputs.push('-i', 'merged_video.mp4');
      inputIndex++;

      // Calculate audio timeline for each scene
      let currentTime = 0;
      const audioDelays: { file: string; delay: number; index: number }[] = [];

      for (let i = 0; i < validScenes.length; i++) {
        const scene = validScenes[i];
        if (scene.audioUrl && audioFiles.includes(`audio_${i.toString().padStart(3, '0')}.wav`)) {
          const audioFileName = `audio_${i.toString().padStart(3, '0')}.wav`;
          audioInputs.push('-i', audioFileName);
          audioDelays.push({
            file: audioFileName,
            delay: Math.round(currentTime * 1000), // Convert to ms
            index: inputIndex
          });
          inputIndex++;
        }
        currentTime += scene.duration;
      }

      // Add BGM if present
      let bgmInputIndex = -1;
      if (bgmUrl) {
        try {
          const bgmData = await fetchFile(bgmUrl);
          await ffmpeg.writeFile('bgm.mp3', bgmData);
          audioInputs.push('-i', 'bgm.mp3');
          bgmInputIndex = inputIndex;
          inputIndex++;
        } catch (e) {
          console.warn('BGM 로드 실패:', e);
        }
      }

      // Build filter complex for audio mixing
      if (audioDelays.length > 0 || bgmInputIndex >= 0) {
        let mixInputs: string[] = [];

        // Add delayed TTS audio streams
        for (const { index, delay } of audioDelays) {
          filterParts.push(`[${index}:a]adelay=${delay}|${delay},volume=1.0[a${index}]`);
          mixInputs.push(`[a${index}]`);
        }

        // Add BGM (lowered volume)
        if (bgmInputIndex >= 0) {
          const totalDuration = validScenes.reduce((sum, s) => sum + s.duration, 0);
          filterParts.push(`[${bgmInputIndex}:a]volume=0.3,afade=t=out:st=${totalDuration - 2}:d=2[bgm]`);
          mixInputs.push('[bgm]');
        }

        if (mixInputs.length > 0) {
          // Mix all audio streams
          filterParts.push(`${mixInputs.join('')}amix=inputs=${mixInputs.length}:duration=longest[aout]`);

          const filterComplex = filterParts.join(';');

          await ffmpeg.exec([
            ...audioInputs,
            '-filter_complex', filterComplex,
            '-map', '0:v',
            '-map', '[aout]',
            '-c:v', 'copy',
            '-c:a', 'aac',
            '-b:a', '192k',
            '-shortest',
            '-y',
            'final_output.mp4'
          ]);

          finalOutput = 'final_output.mp4';
        }
      }
    }

    onProgress?.({
      stage: 'encoding',
      progress: 90,
      message: '최종 MP4 생성 중...'
    });

    // Read the final output
    const outputData = await ffmpeg.readFile(finalOutput);

    // Cleanup
    for (const file of sceneFiles) {
      try { await ffmpeg.deleteFile(file); } catch {}
    }
    for (const file of audioFiles) {
      try { await ffmpeg.deleteFile(file); } catch {}
    }
    try { await ffmpeg.deleteFile('concat.txt'); } catch {}
    try { await ffmpeg.deleteFile('merged_video.mp4'); } catch {}
    try { await ffmpeg.deleteFile('final_output.mp4'); } catch {}
    try { await ffmpeg.deleteFile('bgm.mp3'); } catch {}

    onProgress?.({
      stage: 'complete',
      progress: 100,
      message: '내보내기 완료!'
    });

    return new Blob([outputData], { type: 'video/mp4' });
  }

  /**
   * Generate preview of a single scene
   */
  async generateScenePreview(scene: Scene): Promise<Blob> {
    if (!this.ffmpeg || !this.loaded) {
      await this.load();
    }

    const ffmpeg = this.ffmpeg!;

    if (scene.videoUrl) {
      const videoData = await this.fetchBlobAsUint8Array(scene.videoUrl);
      return new Blob([videoData], { type: 'video/mp4' });
    }

    if (scene.imageUrl) {
      let imageData: Uint8Array;
      if (scene.imageUrl.startsWith('data:')) {
        imageData = this.base64ToUint8Array(scene.imageUrl);
      } else {
        imageData = await this.fetchBlobAsUint8Array(scene.imageUrl);
      }

      await ffmpeg.writeFile('preview_input.png', imageData);

      await ffmpeg.exec([
        '-loop', '1',
        '-i', 'preview_input.png',
        '-c:v', 'libx264',
        '-t', '5',
        '-pix_fmt', 'yuv420p',
        '-vf', 'scale=640:360',
        '-r', '30',
        '-y',
        'preview_output.mp4'
      ]);

      const outputData = await ffmpeg.readFile('preview_output.mp4');

      try { await ffmpeg.deleteFile('preview_input.png'); } catch {}
      try { await ffmpeg.deleteFile('preview_output.mp4'); } catch {}

      return new Blob([outputData], { type: 'video/mp4' });
    }

    throw new Error('No media to preview');
  }

  /**
   * Check if FFmpeg is loaded
   */
  isLoaded(): boolean {
    return this.loaded;
  }
}

// Singleton instance
export const videoExporter = new VideoExporterService();
