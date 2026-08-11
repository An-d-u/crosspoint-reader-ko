#pragma once

#include <cstddef>
#include <cstdint>

#include "StudyDeck.h"
#include "StudyFsrs.h"

namespace study {

enum class State : uint8_t { New = 0, Learning = 1, Review = 2, Relearning = 3, Suspended = 4 };

struct Steps {
  float learn[kMaxLearningSteps] = {};
  float relearn[kMaxLearningSteps] = {};
  uint8_t learnCount = 0;
  uint8_t relearnCount = 0;

  static Steps defaults();
};

struct Outcome {
  CardState card;
  int32_t delayMinutes = 0;
  int32_t intervalDays = 0;
};

class Scheduler {
 public:
  Scheduler(const Fsrs& fsrs, const Steps& steps) : fsrs_(&fsrs), steps_(steps) {}

  Outcome answer(const CardState& card, Rating rating, int today, int nowMinute) const;
  void preview(const CardState& card, int today, int nowMinute, Outcome output[4]) const;
  static bool isDue(const CardState& card, int today, int nowMinute);

 private:
  const Fsrs* fsrs_;
  Steps steps_;
};

void formatDelay(int minutes, int days, char* output, size_t size);

}  // namespace study
