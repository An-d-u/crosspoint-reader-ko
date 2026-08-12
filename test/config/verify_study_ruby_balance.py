#!/usr/bin/env python3
"""학습 후리가나 제약 최적 배치의 좌표 불변식을 검사한다."""

from __future__ import annotations

import random
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
GAP = 3


def round_away(value: float) -> int:
    return int(value + 0.5) if value >= 0 else int(value - 0.5)


def place_balanced(runs: list[tuple[int, int]], left: int, right: int) -> list[tuple[int, int]]:
    if not runs or right <= left:
        return runs

    total_width = sum(width for _, width in runs)
    available_width = right - left
    gap = GAP
    if len(runs) > 1 and total_width + gap * (len(runs) - 1) > available_width:
        gap = max(0, (available_width - total_width) // (len(runs) - 1))

    required_width = total_width + gap * (len(runs) - 1)
    lower = left if required_width <= available_width else left - (required_width - available_width) // 2
    upper = right - required_width if required_width <= available_width else lower

    offsets: list[int] = []
    blocks: list[list[int]] = []
    offset = 0
    for index, (preferred_x, width) in enumerate(runs):
        offsets.append(offset)
        blocks.append([index, index + 1, preferred_x - offset, 1])
        while len(blocks) >= 2:
            previous = blocks[-2]
            current = blocks[-1]
            if previous[2] * current[3] <= current[2] * previous[3]:
                break
            previous[1] = current[1]
            previous[2] += current[2]
            previous[3] += current[3]
            blocks.pop()
        offset += width + gap

    placed = [[0, width] for _, width in runs]
    for start, end, position_sum, size in blocks:
        position = max(lower, min(round_away(position_sum / size), upper))
        for index in range(start, end):
            placed[index][0] = position + offsets[index]
    return [(x, width) for x, width in placed]


def assert_layout(runs: list[tuple[int, int]], left: int, right: int) -> None:
    placed = place_balanced(runs, left, right)
    total_width = sum(width for _, width in runs)
    feasible_gap = GAP if total_width + GAP * max(0, len(runs) - 1) <= right - left else 0
    for index in range(1, len(placed)):
        previous_x, previous_width = placed[index - 1]
        assert previous_x + previous_width + feasible_gap <= placed[index][0]
    if total_width + feasible_gap * max(0, len(runs) - 1) <= right - left:
        assert all(left <= x and x + width <= right for x, width in placed)


def main() -> int:
    activity = (ROOT / "src/activities/study/StudyActivity.cpp").read_text(encoding="utf-8")
    for token in (
        "blockPositionSum",
        "static_cast<int64_t>(previous.blockPositionSum) * current.blockSize",
        "requiredWidth <= availableWidth",
        "balancedPosition + runs[index].x",
    ):
        if token not in activity:
            raise AssertionError(f"제약 최적 배치 구현 누락: {token}")
    for forbidden in ("StudyRubyGroup", "findStudyRubyGroup", "placeStudyRubyRunsGlobally"):
        if forbidden in activity:
            raise AssertionError(f"방향 편향이 있는 그룹 배치가 남아 있음: {forbidden}")

    # 충돌 그룹 옆의 독립된 루비는 원문 중앙 위치를 유지해야 한다.
    independent = [(40, 55), (70, 55), (190, 12)]
    assert place_balanced(independent, 20, 220)[-1][0] == independent[-1][0]

    # 다중 에이전트 검토에서 발견된 경계 및 비단조 좌표 회귀.
    assert_layout([(5, 58), (69, 37)], 0, 100)
    assert_layout([(20, 6), (29, 2), (12, 53), (47, 8)], 0, 100)

    # 사진처럼 밀집된 루비도 읽기 방향을 반전했을 때 같은 결과가 나와야 한다.
    dense = [(18, 44), (45, 38), (76, 46), (110, 40), (145, 35), (178, 20)]
    dense_placed = place_balanced(dense, 0, 220)
    mirrored = [(220 - (x + width), width) for x, width in reversed(dense)]
    mirrored_placed = place_balanced(mirrored, 0, 220)
    restored = [(220 - (x + width), width) for x, width in reversed(mirrored_placed)]
    assert all(abs(left[0] - right[0]) <= 1 for left, right in zip(dense_placed, restored))
    assert_layout(dense, 0, 220)

    for runs in (
        [(50, 20)],
        [(10, 80), (25, 70), (40, 60)],
        [(-10, 25), (15, 30), (90, 40)],
        [(20, 35), (55, 35), (90, 35), (125, 35)],
    ):
        assert_layout(runs, 0, 160)

    for seed in range(10_000):
        random.seed(seed)
        count = random.randint(1, 12)
        positions = sorted(random.randint(-30, 500) for _ in range(count))
        assert_layout([(position, random.randint(1, 80)) for position in positions], 20, 460)

    print("PASS: 학습 후리가나 제약 최적 배치의 경계와 무작위 좌표를 확인했습니다.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
