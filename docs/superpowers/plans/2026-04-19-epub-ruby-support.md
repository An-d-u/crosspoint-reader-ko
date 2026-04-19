# EPUB Ruby Support Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** EPUB 리더 본문에서 `<ruby>/<rt>/<rb>/<rp>` 기반 후리가나를 가로쓰기 기준으로 실제로 렌더링한다.

**Architecture:** HTML 파서가 ruby 본문과 ruby 텍스트를 별도 토큰으로 저장하고, 본문 레이아웃은 ruby가 있는 토큰만 더 넓은 폭과 더 높은 줄 높이를 사용한다. 렌더 단계에서는 본문 폰트와 작은 ruby 폰트를 함께 받아 base text 위에 ruby를 한 번 더 그린다.

**Tech Stack:** C++, Expat HTML 파서, 기존 `ParsedText`/`TextBlock` 레이아웃, `GfxRenderer`, PlatformIO 빌드, Python 구조 검증 스크립트

---

### Task 1: Ruby 구조 회귀 테스트 추가

**Files:**
- Create: `test/epub/verify_ruby_render_plumbing.py`

- [ ] **Step 1: 실패 테스트 작성**

`ruby` 파싱, `rubyTexts` 저장, `rubyFontId` 전달 경로가 코드에 없으면 실패하도록 구조 검증 스크립트를 만든다.

- [ ] **Step 2: 실패 확인**

Run: `python test/epub/verify_ruby_render_plumbing.py`
Expected: FAIL with missing ruby plumbing messages

### Task 2: 파서와 텍스트 모델에 ruby 데이터 추가

**Files:**
- Modify: `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.h`
- Modify: `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp`
- Modify: `lib/Epub/Epub/ParsedText.h`
- Modify: `lib/Epub/Epub/ParsedText.cpp`

- [ ] **Step 1: ruby 상태 필드와 토큰 저장 구조 추가**

`ruby`, `rt`, `rb`, `rp` 상태를 추적하고, 파서가 base text와 ruby text를 분리해 `ParsedText`에 전달하도록 만든다.

- [ ] **Step 2: 본문 토큰과 ruby 토큰이 함께 저장되도록 변경**

`ParsedText`가 단어별 ruby 문자열을 같이 보관하고, 줄 추출 시 `TextBlock`으로 전달하게 만든다.

### Task 3: 레이아웃과 렌더에 ruby 배치 추가

**Files:**
- Modify: `lib/Epub/Epub/blocks/TextBlock.h`
- Modify: `lib/Epub/Epub/blocks/TextBlock.cpp`
- Modify: `lib/Epub/Epub/Page.h`
- Modify: `lib/Epub/Epub/Page.cpp`
- Modify: `src/activities/reader/EpubReaderActivity.cpp`

- [ ] **Step 1: 토큰 폭 계산을 base/ruby 최대값 기준으로 조정**

본문 폭보다 ruby가 더 넓으면 ruby 폭을 기준으로 줄바꿈과 정렬을 계산한다.

- [ ] **Step 2: ruby 줄 높이와 실제 렌더 추가**

line 단위로 ruby reserve를 확보하고, 본문은 아래로 내린 뒤 ruby는 작은 폰트로 위에 centered 배치한다.

- [ ] **Step 3: reader 경로에 ruby 폰트 전달**

EPUB 페이지 렌더 경로가 base font와 ruby font를 함께 넘기도록 바꾼다.

### Task 4: 캐시 호환성과 검증

**Files:**
- Modify: `lib/Epub/Epub/Section.cpp`
- Possibly modify: `test/README`

- [ ] **Step 1: section cache version 갱신**

`TextBlock` 직렬화 포맷이 바뀌므로 section cache 버전을 올린다.

- [ ] **Step 2: 구조 테스트/빌드 검증**

Run:
- `python test/epub/verify_ruby_render_plumbing.py`
- `python -m platformio run`

Expected:
- 구조 검증 PASS
- 전체 빌드 PASS
