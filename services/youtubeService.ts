// YouTube Data API v3 서비스

const YOUTUBE_API_KEY = import.meta.env.VITE_YOUTUBE_API_KEY;
const BASE_URL = 'https://www.googleapis.com/youtube/v3';

export interface ChannelStats {
  id: string;
  title: string;
  description: string;
  thumbnailUrl: string;
  subscriberCount: number;
  viewCount: number;
  videoCount: number;
  customUrl?: string;
  publishedAt: string;
  country?: string;
  categoryId?: string;
}

export interface VideoStats {
  id: string;
  title: string;
  viewCount: number;
  likeCount: number;
  commentCount: number;
  publishedAt: string;
  thumbnailUrl: string;
}

export interface ChannelAnalytics {
  channel: ChannelStats;
  recentVideos: VideoStats[];
  averageViews: number;
  averageLikes: number;
  averageComments: number;
  engagementRate: number; // (likes + comments) / views
  uploadFrequency: string;
}

/**
 * YouTube URL에서 채널 ID 또는 사용자명 추출
 */
export function extractChannelIdentifier(url: string): { type: 'channel' | 'user' | 'handle', value: string } | null {
  try {
    const urlObj = new URL(url);
    const pathParts = urlObj.pathname.split('/').filter(Boolean);

    // youtube.com/channel/CHANNEL_ID
    if (pathParts[0] === 'channel' && pathParts[1]) {
      return { type: 'channel', value: pathParts[1] };
    }

    // youtube.com/@handle
    if (pathParts[0]?.startsWith('@')) {
      return { type: 'handle', value: pathParts[0] };
    }

    // youtube.com/user/USERNAME
    if (pathParts[0] === 'user' && pathParts[1]) {
      return { type: 'user', value: pathParts[1] };
    }

    // youtube.com/c/CUSTOM_URL
    if (pathParts[0] === 'c' && pathParts[1]) {
      return { type: 'handle', value: pathParts[1] };
    }

    return null;
  } catch (e) {
    return null;
  }
}

/**
 * 채널 ID로 채널 정보 가져오기
 */
export async function getChannelById(channelId: string): Promise<ChannelStats> {
  const url = `${BASE_URL}/channels?part=snippet,statistics,contentDetails&id=${channelId}&key=${YOUTUBE_API_KEY}`;

  const response = await fetch(url);
  if (!response.ok) {
    throw new Error(`YouTube API 오류: ${response.status} ${response.statusText}`);
  }

  const data = await response.json();

  if (!data.items || data.items.length === 0) {
    throw new Error('채널을 찾을 수 없습니다.');
  }

  const channel = data.items[0];

  return {
    id: channel.id,
    title: channel.snippet.title,
    description: channel.snippet.description,
    thumbnailUrl: channel.snippet.thumbnails.high?.url || channel.snippet.thumbnails.default.url,
    subscriberCount: parseInt(channel.statistics.subscriberCount || '0'),
    viewCount: parseInt(channel.statistics.viewCount || '0'),
    videoCount: parseInt(channel.statistics.videoCount || '0'),
    customUrl: channel.snippet.customUrl,
    publishedAt: channel.snippet.publishedAt,
    country: channel.snippet.country,
  };
}

/**
 * 핸들(@username)로 채널 ID 찾기
 */
export async function getChannelByHandle(handle: string): Promise<ChannelStats> {
  // @ 제거
  const cleanHandle = handle.startsWith('@') ? handle.slice(1) : handle;

  // 검색 API를 사용하여 채널 찾기
  const searchUrl = `${BASE_URL}/search?part=snippet&type=channel&q=${encodeURIComponent(cleanHandle)}&key=${YOUTUBE_API_KEY}&maxResults=5`;

  const searchResponse = await fetch(searchUrl);
  if (!searchResponse.ok) {
    throw new Error(`YouTube API 오류: ${searchResponse.status}`);
  }

  const searchData = await searchResponse.json();

  if (!searchData.items || searchData.items.length === 0) {
    throw new Error('채널을 찾을 수 없습니다.');
  }

  // 정확히 일치하는 채널 찾기
  const matchingChannel = searchData.items.find((item: any) =>
    item.snippet.channelTitle.toLowerCase() === cleanHandle.toLowerCase() ||
    item.snippet.title.toLowerCase() === cleanHandle.toLowerCase()
  ) || searchData.items[0];

  const channelId = matchingChannel.snippet.channelId || matchingChannel.id.channelId;

  // 채널 ID로 상세 정보 가져오기
  return getChannelById(channelId);
}

/**
 * 채널의 최근 영상들 가져오기
 */
export async function getChannelVideos(channelId: string, maxResults: number = 10): Promise<VideoStats[]> {
  // 1. 채널의 업로드 플레이리스트 ID 가져오기
  const channelUrl = `${BASE_URL}/channels?part=contentDetails&id=${channelId}&key=${YOUTUBE_API_KEY}`;
  const channelResponse = await fetch(channelUrl);
  const channelData = await channelResponse.json();

  const uploadsPlaylistId = channelData.items[0]?.contentDetails?.relatedPlaylists?.uploads;
  if (!uploadsPlaylistId) {
    throw new Error('업로드 플레이리스트를 찾을 수 없습니다.');
  }

  // 2. 플레이리스트에서 최근 영상 ID들 가져오기
  const playlistUrl = `${BASE_URL}/playlistItems?part=contentDetails&playlistId=${uploadsPlaylistId}&maxResults=${maxResults}&key=${YOUTUBE_API_KEY}`;
  const playlistResponse = await fetch(playlistUrl);
  const playlistData = await playlistResponse.json();

  const videoIds = playlistData.items.map((item: any) => item.contentDetails.videoId).join(',');

  // 3. 영상 통계 가져오기
  const videosUrl = `${BASE_URL}/videos?part=snippet,statistics&id=${videoIds}&key=${YOUTUBE_API_KEY}`;
  const videosResponse = await fetch(videosUrl);
  const videosData = await videosResponse.json();

  return videosData.items.map((video: any) => ({
    id: video.id,
    title: video.snippet.title,
    viewCount: parseInt(video.statistics.viewCount || '0'),
    likeCount: parseInt(video.statistics.likeCount || '0'),
    commentCount: parseInt(video.statistics.commentCount || '0'),
    publishedAt: video.snippet.publishedAt,
    thumbnailUrl: video.snippet.thumbnails.medium?.url || video.snippet.thumbnails.default.url,
  }));
}

/**
 * 채널 분석 데이터 계산
 */
export async function analyzeChannel(channelId: string): Promise<ChannelAnalytics> {
  const channel = await getChannelById(channelId);
  const recentVideos = await getChannelVideos(channelId, 20);

  if (recentVideos.length === 0) {
    return {
      channel,
      recentVideos: [],
      averageViews: 0,
      averageLikes: 0,
      averageComments: 0,
      engagementRate: 0,
      uploadFrequency: '알 수 없음',
    };
  }

  const totalViews = recentVideos.reduce((sum, v) => sum + v.viewCount, 0);
  const totalLikes = recentVideos.reduce((sum, v) => sum + v.likeCount, 0);
  const totalComments = recentVideos.reduce((sum, v) => sum + v.commentCount, 0);

  const averageViews = Math.round(totalViews / recentVideos.length);
  const averageLikes = Math.round(totalLikes / recentVideos.length);
  const averageComments = Math.round(totalComments / recentVideos.length);

  // 인게이지먼트율 = (좋아요 + 댓글) / 조회수
  const engagementRate = totalViews > 0
    ? ((totalLikes + totalComments) / totalViews) * 100
    : 0;

  // 업로드 빈도 계산
  const uploadFrequency = calculateUploadFrequency(recentVideos);

  return {
    channel,
    recentVideos,
    averageViews,
    averageLikes,
    averageComments,
    engagementRate,
    uploadFrequency,
  };
}

/**
 * 업로드 빈도 계산
 */
function calculateUploadFrequency(videos: VideoStats[]): string {
  if (videos.length < 2) return '알 수 없음';

  const dates = videos.map(v => new Date(v.publishedAt).getTime()).sort((a, b) => b - a);
  const daysDiff = (dates[0] - dates[dates.length - 1]) / (1000 * 60 * 60 * 24);
  const videosPerMonth = (videos.length / daysDiff) * 30;

  if (videosPerMonth >= 20) return '거의 매일';
  if (videosPerMonth >= 10) return '주 2-3회';
  if (videosPerMonth >= 4) return '주 1회';
  if (videosPerMonth >= 2) return '2주에 1회';
  if (videosPerMonth >= 1) return '월 1회';
  return '월 1회 미만';
}

/**
 * 비슷한 채널 찾기 (카테고리 및 키워드 기반)
 */
export async function findSimilarChannels(
  channelId: string,
  maxResults: number = 10
): Promise<ChannelStats[]> {
  // 원본 채널 정보 가져오기
  const originalChannel = await getChannelById(channelId);

  // 채널 설명에서 키워드 추출 (간단한 방법)
  const keywords = extractKeywords(originalChannel.title + ' ' + originalChannel.description);
  const searchQuery = keywords.slice(0, 3).join(' ');

  // 비슷한 채널 검색
  const searchUrl = `${BASE_URL}/search?part=snippet&type=channel&q=${encodeURIComponent(searchQuery)}&key=${YOUTUBE_API_KEY}&maxResults=${maxResults + 5}&relevanceLanguage=ko`;

  const response = await fetch(searchUrl);
  if (!response.ok) {
    throw new Error(`YouTube API 오류: ${response.status}`);
  }

  const data = await response.json();

  // 현재 채널 제외하고 채널 ID 수집
  const channelIds = data.items
    .filter((item: any) => {
      const itemChannelId = item.snippet.channelId || item.id.channelId;
      return itemChannelId !== channelId;
    })
    .map((item: any) => item.snippet.channelId || item.id.channelId)
    .slice(0, maxResults)
    .join(',');

  if (!channelIds) {
    return [];
  }

  // 채널 상세 정보 가져오기
  const channelsUrl = `${BASE_URL}/channels?part=snippet,statistics&id=${channelIds}&key=${YOUTUBE_API_KEY}`;
  const channelsResponse = await fetch(channelsUrl);
  const channelsData = await channelsResponse.json();

  return channelsData.items.map((channel: any) => ({
    id: channel.id,
    title: channel.snippet.title,
    description: channel.snippet.description,
    thumbnailUrl: channel.snippet.thumbnails.high?.url || channel.snippet.thumbnails.default.url,
    subscriberCount: parseInt(channel.statistics.subscriberCount || '0'),
    viewCount: parseInt(channel.statistics.viewCount || '0'),
    videoCount: parseInt(channel.statistics.videoCount || '0'),
    customUrl: channel.snippet.customUrl,
    publishedAt: channel.snippet.publishedAt,
    country: channel.snippet.country,
  }));
}

/**
 * 텍스트에서 키워드 추출 (간단한 구현)
 */
function extractKeywords(text: string): string[] {
  // 불용어 제거 및 중요 단어 추출
  const stopWords = ['the', 'a', 'an', 'and', 'or', 'but', 'in', 'on', 'at', 'to', 'for',
                     '은', '는', '이', '가', '을', '를', '의', '에', '와', '과', '도'];

  const words = text
    .toLowerCase()
    .replace(/[^\w\s가-힣]/g, ' ')
    .split(/\s+/)
    .filter(word => word.length > 2 && !stopWords.includes(word));

  // 빈도수 계산
  const wordCount: { [key: string]: number } = {};
  words.forEach(word => {
    wordCount[word] = (wordCount[word] || 0) + 1;
  });

  // 빈도수 기준 정렬
  return Object.entries(wordCount)
    .sort((a, b) => b[1] - a[1])
    .map(([word]) => word)
    .slice(0, 10);
}

/**
 * YouTube URL로 채널 분석
 */
export async function analyzeChannelFromUrl(url: string): Promise<ChannelAnalytics> {
  const identifier = extractChannelIdentifier(url);

  if (!identifier) {
    throw new Error('유효한 YouTube 채널 URL이 아닙니다.');
  }

  let channelStats: ChannelStats;

  if (identifier.type === 'channel') {
    channelStats = await getChannelById(identifier.value);
  } else {
    channelStats = await getChannelByHandle(identifier.value);
  }

  return analyzeChannel(channelStats.id);
}
