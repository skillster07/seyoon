# VIVIDCAM

한국 라이브 크리에이터를 위한 실시간 뷰티·배경·오버레이 가상 카메라 제품의 UX 프로토타입입니다.

현재 프로토타입은 다음 제품 흐름을 검증합니다.

- 장면 및 소스 레이어 구성
- 세로·가로 방송 캔버스 전환
- 브라우저 카메라 미리보기
- 피부·얼굴 보정 컨트롤
- TikTok 안전 영역과 방송 오버레이
- 가상 카메라 출력 준비 흐름
- SOOP·TikTok·OBS용 60p 기본 출력 프로필

> 현재 버전은 브라우저 기반 UX 프로토타입입니다. 실제 Windows 가상 카메라, GPU 뷰티 엔진, NDI 및 플랫폼 이벤트 연동은 네이티브 클라이언트 단계에서 구현해야 합니다.

## 로컬 실행

```bash
npm install
npm run dev
```

## 검증

```bash
npm run build
npx tsc --noEmit
```

## 제품 개발 로드맵

상용 Windows 제품으로 전환하기 위한 단계별 기술·품질 게이트는 [`docs/PRODUCT_ROADMAP.md`](docs/PRODUCT_ROADMAP.md)를 참고하세요.

Windows 네이티브 1080p60 기술 스파이크의 빌드 방법과 현재 범위는 [`native/README.md`](native/README.md)를 참고하세요.

개발과 검증 운영 방식은 [`docs/DEVELOPMENT_PROCESS.md`](docs/DEVELOPMENT_PROCESS.md)를 참고하세요.
