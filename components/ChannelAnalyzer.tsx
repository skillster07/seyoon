import React, { useState } from 'react';
import { Search, TrendingUp, Users, Eye, ThumbsUp, MessageCircle, Calendar, BarChart3, ExternalLink } from 'lucide-react';
import { analyzeChannelFromUrl, findSimilarChannels, ChannelAnalytics, ChannelStats } from '../services/youtubeService';

export const ChannelAnalyzer: React.FC = () => {
  const [youtubeUrl, setYoutubeUrl] = useState('');
  const [isLoading, setIsLoading] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [analytics, setAnalytics] = useState<ChannelAnalytics | null>(null);
  const [similarChannels, setSimilarChannels] = useState<ChannelStats[]>([]);
  const [isLoadingSimilar, setIsLoadingSimilar] = useState(false);

  const formatNumber = (num: number): string => {
    if (num >= 1000000) return `${(num / 1000000).toFixed(1)}M`;
    if (num >= 1000) return `${(num / 1000).toFixed(1)}K`;
    return num.toString();
  };

  const handleAnalyze = async () => {
    if (!youtubeUrl.trim()) {
      setError('YouTube URL을 입력해주세요.');
      return;
    }

    setIsLoading(true);
    setError(null);
    setAnalytics(null);
    setSimilarChannels([]);

    try {
      // 채널 분석
      const result = await analyzeChannelFromUrl(youtubeUrl);
      setAnalytics(result);

      // 비슷한 채널 찾기
      setIsLoadingSimilar(true);
      const similar = await findSimilarChannels(result.channel.id, 10);
      setSimilarChannels(similar);
    } catch (e: any) {
      console.error(e);
      setError(e.message || '채널 분석에 실패했습니다.');
    } finally {
      setIsLoading(false);
      setIsLoadingSimilar(false);
    }
  };

  const handleKeyPress = (e: React.KeyboardEvent) => {
    if (e.key === 'Enter') {
      handleAnalyze();
    }
  };

  return (
    <div className="min-h-screen bg-gradient-to-br from-slate-900 via-purple-900 to-slate-900 text-white p-4 md:p-8">
      <div className="max-w-7xl mx-auto">
        {/* Header */}
        <div className="text-center mb-8">
          <h1 className="text-4xl md:text-5xl font-bold mb-4 bg-gradient-to-r from-purple-400 to-pink-400 bg-clip-text text-transparent">
            YouTube 채널 분석기
          </h1>
          <p className="text-gray-300 text-lg">
            채널 URL을 입력하면 통계와 비슷한 채널을 추천해드립니다
          </p>
        </div>

        {/* Input Section */}
        <div className="bg-white/10 backdrop-blur-lg rounded-2xl p-6 mb-8 border border-white/20">
          <div className="flex flex-col md:flex-row gap-4">
            <div className="flex-1">
              <input
                type="text"
                value={youtubeUrl}
                onChange={(e) => setYoutubeUrl(e.target.value)}
                onKeyPress={handleKeyPress}
                placeholder="YouTube 채널 URL을 입력하세요 (예: https://youtube.com/@channel)"
                className="w-full px-6 py-4 bg-white/5 border border-white/20 rounded-xl text-white placeholder-gray-400 focus:outline-none focus:ring-2 focus:ring-purple-500 focus:border-transparent"
              />
            </div>
            <button
              onClick={handleAnalyze}
              disabled={isLoading}
              className={`px-8 py-4 rounded-xl font-semibold flex items-center justify-center gap-2 transition-all ${
                isLoading
                  ? 'bg-gray-600 cursor-not-allowed'
                  : 'bg-gradient-to-r from-purple-600 to-pink-600 hover:from-purple-500 hover:to-pink-500 shadow-lg shadow-purple-500/50'
              }`}
            >
              {isLoading ? (
                <>
                  <div className="animate-spin rounded-full h-5 w-5 border-b-2 border-white"></div>
                  분석 중...
                </>
              ) : (
                <>
                  <Search size={20} />
                  분석하기
                </>
              )}
            </button>
          </div>

          {error && (
            <div className="mt-4 p-4 bg-red-500/20 border border-red-500/50 rounded-lg text-red-200">
              {error}
            </div>
          )}
        </div>

        {/* Analytics Results */}
        {analytics && (
          <div className="space-y-8">
            {/* Channel Info */}
            <div className="bg-white/10 backdrop-blur-lg rounded-2xl p-6 border border-white/20">
              <div className="flex flex-col md:flex-row gap-6">
                <img
                  src={analytics.channel.thumbnailUrl}
                  alt={analytics.channel.title}
                  className="w-32 h-32 rounded-full border-4 border-purple-500"
                />
                <div className="flex-1">
                  <h2 className="text-3xl font-bold mb-2">{analytics.channel.title}</h2>
                  <p className="text-gray-300 mb-4 line-clamp-2">{analytics.channel.description}</p>
                  <div className="flex flex-wrap gap-4">
                    {analytics.channel.customUrl && (
                      <a
                        href={`https://youtube.com/${analytics.channel.customUrl}`}
                        target="_blank"
                        rel="noopener noreferrer"
                        className="flex items-center gap-2 text-purple-400 hover:text-purple-300"
                      >
                        <ExternalLink size={16} />
                        채널 방문
                      </a>
                    )}
                    <span className="text-gray-400">
                      <Calendar size={16} className="inline mr-1" />
                      {new Date(analytics.channel.publishedAt).toLocaleDateString('ko-KR')} 시작
                    </span>
                  </div>
                </div>
              </div>
            </div>

            {/* Statistics Grid */}
            <div className="grid grid-cols-1 md:grid-cols-2 lg:grid-cols-4 gap-4">
              <StatCard
                icon={<Users className="text-purple-400" size={24} />}
                label="구독자"
                value={formatNumber(analytics.channel.subscriberCount)}
                subtitle={`총 ${analytics.channel.videoCount}개 영상`}
              />
              <StatCard
                icon={<Eye className="text-blue-400" size={24} />}
                label="총 조회수"
                value={formatNumber(analytics.channel.viewCount)}
                subtitle={`평균 ${formatNumber(analytics.averageViews)}/영상`}
              />
              <StatCard
                icon={<ThumbsUp className="text-pink-400" size={24} />}
                label="평균 좋아요"
                value={formatNumber(analytics.averageLikes)}
                subtitle={`영상당 평균`}
              />
              <StatCard
                icon={<TrendingUp className="text-green-400" size={24} />}
                label="인게이지먼트"
                value={`${analytics.engagementRate.toFixed(2)}%`}
                subtitle={analytics.uploadFrequency}
              />
            </div>

            {/* Recent Videos */}
            {analytics.recentVideos.length > 0 && (
              <div className="bg-white/10 backdrop-blur-lg rounded-2xl p-6 border border-white/20">
                <h3 className="text-2xl font-bold mb-4 flex items-center gap-2">
                  <BarChart3 className="text-purple-400" />
                  최근 영상 분석
                </h3>
                <div className="grid grid-cols-1 md:grid-cols-2 gap-4">
                  {analytics.recentVideos.slice(0, 6).map((video) => (
                    <div
                      key={video.id}
                      className="bg-white/5 rounded-lg p-4 hover:bg-white/10 transition-colors"
                    >
                      <div className="flex gap-3">
                        <img
                          src={video.thumbnailUrl}
                          alt={video.title}
                          className="w-24 h-16 object-cover rounded"
                        />
                        <div className="flex-1 min-w-0">
                          <h4 className="font-medium mb-2 line-clamp-2 text-sm">
                            {video.title}
                          </h4>
                          <div className="flex flex-wrap gap-3 text-xs text-gray-400">
                            <span className="flex items-center gap-1">
                              <Eye size={12} />
                              {formatNumber(video.viewCount)}
                            </span>
                            <span className="flex items-center gap-1">
                              <ThumbsUp size={12} />
                              {formatNumber(video.likeCount)}
                            </span>
                            <span className="flex items-center gap-1">
                              <MessageCircle size={12} />
                              {formatNumber(video.commentCount)}
                            </span>
                          </div>
                        </div>
                      </div>
                    </div>
                  ))}
                </div>
              </div>
            )}

            {/* Similar Channels */}
            {isLoadingSimilar ? (
              <div className="bg-white/10 backdrop-blur-lg rounded-2xl p-8 border border-white/20 text-center">
                <div className="animate-spin rounded-full h-12 w-12 border-b-2 border-white mx-auto mb-4"></div>
                <p className="text-gray-300">비슷한 채널을 찾는 중...</p>
              </div>
            ) : similarChannels.length > 0 ? (
              <div className="bg-white/10 backdrop-blur-lg rounded-2xl p-6 border border-white/20">
                <h3 className="text-2xl font-bold mb-4 flex items-center gap-2">
                  <Users className="text-purple-400" />
                  비슷한 채널 추천
                </h3>
                <div className="grid grid-cols-1 md:grid-cols-2 lg:grid-cols-3 gap-4">
                  {similarChannels.map((channel) => (
                    <a
                      key={channel.id}
                      href={`https://youtube.com/channel/${channel.id}`}
                      target="_blank"
                      rel="noopener noreferrer"
                      className="bg-white/5 rounded-lg p-4 hover:bg-white/10 transition-all hover:scale-105"
                    >
                      <div className="flex items-start gap-3">
                        <img
                          src={channel.thumbnailUrl}
                          alt={channel.title}
                          className="w-16 h-16 rounded-full"
                        />
                        <div className="flex-1 min-w-0">
                          <h4 className="font-semibold mb-1 line-clamp-1">{channel.title}</h4>
                          <div className="space-y-1 text-xs text-gray-400">
                            <div className="flex items-center gap-1">
                              <Users size={12} />
                              {formatNumber(channel.subscriberCount)} 구독자
                            </div>
                            <div className="flex items-center gap-1">
                              <Eye size={12} />
                              {formatNumber(channel.viewCount)} 조회
                            </div>
                          </div>
                        </div>
                      </div>
                    </a>
                  ))}
                </div>
              </div>
            ) : null}
          </div>
        )}
      </div>
    </div>
  );
};

// Stat Card Component
interface StatCardProps {
  icon: React.ReactNode;
  label: string;
  value: string;
  subtitle: string;
}

const StatCard: React.FC<StatCardProps> = ({ icon, label, value, subtitle }) => {
  return (
    <div className="bg-white/10 backdrop-blur-lg rounded-xl p-6 border border-white/20 hover:bg-white/15 transition-colors">
      <div className="flex items-center gap-3 mb-2">
        {icon}
        <span className="text-gray-400 text-sm font-medium">{label}</span>
      </div>
      <div className="text-3xl font-bold mb-1">{value}</div>
      <div className="text-sm text-gray-400">{subtitle}</div>
    </div>
  );
};
