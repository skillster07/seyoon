# YouTube 채널 분석기

유튜브 채널 URL을 입력하면 채널 통계를 분석하고 비슷한 채널을 추천해주는 웹 애플리케이션입니다.

## 주요 기능

### 1. 채널 분석
- **구독자 수**: 채널의 총 구독자 수
- **총 조회수**: 채널의 모든 영상 조회수 합계
- **평균 조회수**: 최근 20개 영상의 평균 조회수
- **평균 좋아요**: 최근 영상들의 평균 좋아요 수
- **인게이지먼트율**: (좋아요 + 댓글) / 조회수 비율
- **업로드 빈도**: 영상 업로드 주기 분석

### 2. 최근 영상 분석
- 최근 업로드된 영상들의 상세 통계
- 각 영상의 조회수, 좋아요, 댓글 수 표시
- 썸네일과 제목으로 한눈에 파악

### 3. 비슷한 채널 추천
- 카테고리와 키워드 기반으로 유사 채널 추천
- 각 채널의 구독자 수와 조회수 표시
- 클릭하면 해당 채널로 바로 이동

## 설치 및 실행

### 1. 환경 설정

YouTube Data API v3 키가 필요합니다:

1. [Google Cloud Console](https://console.cloud.google.com/)에 접속
2. 새 프로젝트 생성 또는 기존 프로젝트 선택
3. **API 및 서비스 > 라이브러리**로 이동
4. "YouTube Data API v3" 검색 및 활성화
5. **사용자 인증 정보 > API 키 만들기**
6. 생성된 API 키 복사

### 2. 환경 변수 설정

프로젝트 루트에 `.env` 파일을 생성:

```bash
cp .env.example .env
```

`.env` 파일을 열고 API 키 입력:

```
VITE_YOUTUBE_API_KEY=your_actual_youtube_api_key_here
```

### 3. 의존성 설치

```bash
npm install
```

### 4. 개발 서버 실행

```bash
npm run dev
```

브라우저에서 `http://localhost:5173` 접속

### 5. 프로덕션 빌드

```bash
npm run build
```

## 사용 방법

1. YouTube 채널 URL 입력
   - `https://youtube.com/@channelname`
   - `https://youtube.com/channel/CHANNEL_ID`
   - `https://youtube.com/c/customname`

2. "분석하기" 버튼 클릭

3. 채널 통계 및 비슷한 채널 확인

## 기술 스택

- **React 19** - UI 프레임워크
- **TypeScript** - 타입 안정성
- **Vite** - 빌드 도구
- **Tailwind CSS** - 스타일링
- **Lucide React** - 아이콘
- **YouTube Data API v3** - 데이터 소스

## 프로젝트 구조

```
seyoon/
├── components/
│   └── ChannelAnalyzer.tsx    # 메인 분석 UI 컴포넌트
├── services/
│   └── youtubeService.ts       # YouTube API 서비스
├── App.tsx                      # 앱 엔트리 포인트
├── index.tsx                    # React 마운트
└── .env.example                 # 환경 변수 예제
```

## API 사용량 제한

YouTube Data API v3는 할당량 제한이 있습니다:
- 무료 할당량: 하루 10,000 단위
- 채널 분석 1회: 약 3-5 단위 소비

일일 사용량을 [Google Cloud Console](https://console.cloud.google.com/apis/api/youtube.googleapis.com/quotas)에서 확인할 수 있습니다.

## 라이선스

MIT

## 기여

이슈 제보 및 풀 리퀘스트를 환영합니다!
