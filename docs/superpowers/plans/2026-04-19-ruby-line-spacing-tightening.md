# Ruby Line Spacing Tightening Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 후리가나가 있는 본문 줄도 일반 줄과 거의 같은 줄간격으로 보이게 하고, 후리가나가 달린 한자의 본문 좌우 간격이 과하게 벌어지지 않게 만든다.

**Architecture:** `TextBlock`의 ruby 줄 높이 계산을 `기본 줄 높이 + ruby 전체 높이` 방식에서 `기본 줄 높이 + 작은 보정치` 방식으로 바꾸고, ruby 렌더 위치를 위로 당긴다. 추가로 `ParsedText`의 토큰 폭 계산은 ruby 폭이 아니라 본문 폭을 기준으로 사용해서 줄 배치가 본문 간격을 유지하도록 바꾸고, ruby는 본문 중심 기준으로만 오버행 렌더링한다. 이 레이아웃 의미 변경이 기존 캐시와 섞이지 않게 섹션 캐시 버전도 함께 갱신한다.

**Tech Stack:** C++, `TextBlock`, EPUB reader render path, Python 구조 검증 스크립트, PlatformIO

---

### Task 1: 줄간격 정책 회귀 테스트 추가

**Files:**
- Create: `test/epub/verify_ruby_tight_line_spacing.py`

- [ ] **Step 1: 실패 테스트 작성**

`getRenderedLineHeight()`가 전체 ruby line height를 그대로 더하지 않고, ruby 전용 작은 보정치 상수를 사용해야 통과하는 구조 검증 스크립트를 만든다.

- [ ] **Step 2: 실패 확인**

Run: `python test/epub/verify_ruby_tight_line_spacing.py`
Expected: FAIL with missing tight ruby spacing markers

### Task 2: TextBlock 줄높이/오프셋 조정

**Files:**
- Modify: `lib/Epub/Epub/blocks/TextBlock.cpp`

- [ ] **Step 1: ruby 줄높이 보정치 상수 추가**

`kRubyLineExtraPx`, `kRubyBaseYOffsetPx` 같은 작은 상수로 ruby 줄을 일반 줄에 가깝게 유지한다.

- [ ] **Step 2: 렌더 위치 조정**

본문은 조금만 아래로 내리고 ruby는 현재 줄 박스 상단에 더 밀착되게 그린다.

### Task 3: 검증

**Files:**
- Test: `test/epub/verify_ruby_tight_line_spacing.py`

- [ ] **Step 1: 구조 검증 실행**

Run: `python test/epub/verify_ruby_tight_line_spacing.py`
Expected: PASS

- [ ] **Step 2: 전체 빌드 확인**

Run: `python -m platformio run`
Expected: PASS

### Task 4: ruby 토큰 폭 축소 회귀 테스트 추가

**Files:**
- Create: `test/epub/verify_ruby_compact_token_width.py`

- [ ] **Step 1: 실패 테스트 작성**

`ParsedText`가 ruby 폭으로 토큰 폭을 넓히지 않고 본문 폭을 유지해야 하며, `TextBlock`은 ruby를 본문 중심 기준으로 오버행 렌더링해야 통과하는 구조 검증 스크립트를 만든다.

- [ ] **Step 2: 실패 확인**

Run: `python test/epub/verify_ruby_compact_token_width.py`
Expected: FAIL with ruby token width still tied to ruby width

### Task 5: ruby 본문 간격 복원 + 캐시 무효화

**Files:**
- Modify: `lib/Epub/Epub/ParsedText.cpp`
- Modify: `lib/Epub/Epub/blocks/TextBlock.cpp`
- Modify: `lib/Epub/Epub/Section.cpp`

- [ ] **Step 1: 토큰 폭을 본문 기준으로 고정**

`measureTokenWidth()`가 ruby 폭 때문에 본문 배치를 넓히지 않도록 본문 폭만 반환하게 바꾼다.

- [ ] **Step 2: ruby 중심 정렬 기준 변경**

본문은 본문 폭 기준 자리에 그대로 그리고, ruby는 그 본문 중심에 맞춰 좌우 오버행을 허용하면서 렌더링한다. 이때 ruby는 기존보다 2px 더 위로 올린다.

- [ ] **Step 3: 섹션 캐시 버전 갱신**

기존 section cache가 예전 token width를 재사용하지 않도록 `SECTION_FILE_VERSION`을 올린다.

### Task 6: 최종 검증

**Files:**
- Test: `test/epub/verify_ruby_tight_line_spacing.py`
- Test: `test/epub/verify_ruby_compact_token_width.py`

- [ ] **Step 1: ruby 구조 검증 2종 실행**

Run: `python test/epub/verify_ruby_tight_line_spacing.py`
Expected: PASS

Run: `python test/epub/verify_ruby_compact_token_width.py`
Expected: PASS

- [ ] **Step 2: 전체 빌드 확인**

Run: `python -m platformio run`
Expected: PASS
