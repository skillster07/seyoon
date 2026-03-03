/**
 * Pexels API service for searching stock photos and videos.
 * Free API key required: https://www.pexels.com/api/
 */

const getPexelsKey = () => process.env.PEXELS_API_KEY || "";

export interface StockPhoto {
  id: number;
  url: string; // large image URL
  thumbnail: string; // small preview
  photographer: string;
  alt: string;
}

export interface StockVideo {
  id: number;
  url: string; // video file URL
  thumbnail: string; // preview image
  duration: number; // seconds
}

/**
 * Search Pexels for stock photos matching a query.
 */
export async function searchStockPhotos(
  query: string,
  perPage: number = 6
): Promise<StockPhoto[]> {
  const key = getPexelsKey();
  if (!key) throw new Error("PEXELS_API_KEY가 설정되지 않았습니다.");

  const response = await fetch(
    `https://api.pexels.com/v1/search?query=${encodeURIComponent(query)}&per_page=${perPage}&orientation=landscape&size=medium`,
    {
      headers: { Authorization: key },
    }
  );

  if (!response.ok) {
    throw new Error(`Pexels API 오류: ${response.status}`);
  }

  const data = await response.json();

  return (data.photos || []).map((photo: any) => ({
    id: photo.id,
    url: photo.src.large2x || photo.src.large, // ~1920px
    thumbnail: photo.src.medium, // ~350px
    photographer: photo.photographer,
    alt: photo.alt || query,
  }));
}

/**
 * Search Pexels for stock videos matching a query.
 */
export async function searchStockVideos(
  query: string,
  perPage: number = 3
): Promise<StockVideo[]> {
  const key = getPexelsKey();
  if (!key) throw new Error("PEXELS_API_KEY가 설정되지 않았습니다.");

  const response = await fetch(
    `https://api.pexels.com/videos/search?query=${encodeURIComponent(query)}&per_page=${perPage}&orientation=landscape&size=medium`,
    {
      headers: { Authorization: key },
    }
  );

  if (!response.ok) {
    throw new Error(`Pexels Video API 오류: ${response.status}`);
  }

  const data = await response.json();

  return (data.videos || []).map((video: any) => {
    // Prefer HD quality, fallback to first available
    const hdFile = video.video_files?.find(
      (f: any) => f.quality === "hd" && f.width >= 1280
    );
    const sdFile = video.video_files?.find((f: any) => f.quality === "sd");
    const file = hdFile || sdFile || video.video_files?.[0];

    return {
      id: video.id,
      url: file?.link || "",
      thumbnail: video.image || "",
      duration: video.duration || 0,
    };
  });
}

/**
 * Search both photos and videos for a news item.
 */
export async function searchStockMedia(query: string): Promise<{
  photos: StockPhoto[];
  videos: StockVideo[];
}> {
  const [photos, videos] = await Promise.allSettled([
    searchStockPhotos(query, 6),
    searchStockVideos(query, 3),
  ]);

  return {
    photos: photos.status === "fulfilled" ? photos.value : [],
    videos: videos.status === "fulfilled" ? videos.value : [],
  };
}
