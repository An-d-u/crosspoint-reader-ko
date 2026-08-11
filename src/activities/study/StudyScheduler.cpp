#include "StudyScheduler.h"

#include <cstdio>

namespace study {
namespace {
constexpr int kMinutesPerDay = 1440;

int stepMinutes(const float* steps, const uint8_t count, const int index) {
  if (count == 0) return 0;
  const int bounded = index < 0 ? 0 : (index >= count ? count - 1 : index);
  const int minutes = static_cast<int>(steps[bounded] + 0.5f);
  return minutes < 1 ? 1 : minutes;
}

int hardStepMinutes(const float* steps, const uint8_t count, const int index) {
  if (count == 0) return 0;
  const int bounded = index < 0 ? 0 : (index >= count ? count - 1 : index);
  const float current = steps[bounded];
  const float delay = bounded + 1 < count ? (current + steps[bounded + 1]) / 2.0f : current * 1.5f;
  const int minutes = static_cast<int>(delay + 0.5f);
  return minutes < 1 ? 1 : minutes;
}
}  // namespace

Steps Steps::defaults() {
  Steps result;
  result.learn[0] = 1.0f;
  result.learn[1] = 10.0f;
  result.learnCount = 2;
  result.relearn[0] = 10.0f;
  result.relearnCount = 1;
  return result;
}

bool Scheduler::isDue(const CardState& card, const int today, const int nowMinute) {
  const State state = static_cast<State>(card.state);
  if (state == State::Suspended) return false;
  if (state == State::New) return true;
  if (state == State::Learning || state == State::Relearning) {
    if (card.dueDay != today) return card.dueDay < today;
    return card.dueMinute <= nowMinute;
  }
  return card.dueDay <= today;
}

Outcome Scheduler::answer(const CardState& card, const Rating rating, const int today, const int nowMinute) const {
  Outcome result;
  result.card = card;

  const int elapsed = card.lastReviewDay < 0 ? 0 : today - card.lastReviewDay;
  const Memory memory = fsrs_->review(card.memory(), rating, elapsed);
  result.card.setMemory(memory);
  result.card.lastReviewDay = today;
  if (result.card.reps < 0xFFFF) ++result.card.reps;

  const State state = static_cast<State>(card.state);
  const bool review = state == State::Review;
  const bool relearning = state == State::Relearning;
  const bool learning = state == State::Learning || state == State::New;
  const bool useRelearn = relearning || (review && rating == Rating::Again);
  const float* steps = useRelearn ? steps_.relearn : steps_.learn;
  const uint8_t stepCount = useRelearn ? steps_.relearnCount : steps_.learnCount;

  int nextStep = -1;
  if (review) {
    if (rating == Rating::Again) {
      if (result.card.lapses < 0xFFFF) ++result.card.lapses;
      nextStep = stepCount > 0 ? 0 : -1;
    }
  } else if (relearning || learning) {
    switch (rating) {
      case Rating::Again:
        nextStep = stepCount > 0 ? 0 : -1;
        break;
      case Rating::Hard:
        nextStep = stepCount > 0 ? (state == State::New ? 0 : card.stepIndex) : -1;
        break;
      case Rating::Good:
        nextStep = state == State::New ? (stepCount > 1 ? 1 : -1)
                                       : (card.stepIndex + 1 < stepCount ? card.stepIndex + 1 : -1);
        break;
      case Rating::Easy:
        nextStep = -1;
        break;
    }
  }

  if (nextStep < 0) {
    result.card.state = static_cast<uint8_t>(State::Review);
    result.card.stepIndex = 0;
    result.intervalDays = fsrs_->intervalDays(memory);
    result.card.dueDay = today + result.intervalDays;
    result.card.dueMinute = 0;
    return result;
  }

  result.card.state = static_cast<uint8_t>((review || relearning) ? State::Relearning : State::Learning);
  result.card.stepIndex = static_cast<uint8_t>(nextStep);
  result.delayMinutes = rating == Rating::Hard ? hardStepMinutes(steps, stepCount, nextStep)
                                                : stepMinutes(steps, stepCount, nextStep);
  const int dueAt = nowMinute + result.delayMinutes;
  result.card.dueDay = today + dueAt / kMinutesPerDay;
  result.card.dueMinute = static_cast<uint16_t>(dueAt % kMinutesPerDay);
  return result;
}

void Scheduler::preview(const CardState& card, const int today, const int nowMinute, Outcome output[4]) const {
  for (int index = 0; index < 4; ++index) {
    output[index] = answer(card, static_cast<Rating>(index + 1), today, nowMinute);
  }
}

void formatDelay(const int minutes, const int days, char* output, const size_t size) {
  if (minutes > 0) {
    if (minutes < 60) {
      std::snprintf(output, size, "%d분", minutes);
    } else {
      std::snprintf(output, size, "%d시간", (minutes + 30) / 60);
    }
  } else if (days < 31) {
    std::snprintf(output, size, "%d일", days < 1 ? 1 : days);
  } else if (days < 365) {
    std::snprintf(output, size, "%.1f개월", static_cast<double>(days) / 30.4);
  } else {
    std::snprintf(output, size, "%.1f년", static_cast<double>(days) / 365.0);
  }
}

}  // namespace study
