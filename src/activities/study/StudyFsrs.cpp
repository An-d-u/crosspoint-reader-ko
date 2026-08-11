#include "StudyFsrs.h"

#include <cmath>

namespace study {

const float kDefaultParams[kNumParams] = {0.40255f, 1.18385f, 3.173f,  15.69105f, 7.1949f, 0.5345f, 1.4604f,
                                          0.0046f,  1.54575f, 0.1192f, 1.01925f,  1.9395f, 0.11f,   0.29605f,
                                          2.2698f,  0.2315f,  2.9898f, 0.51655f,  0.6621f};

namespace {
constexpr float kDecay = -0.5f;
const float kFactor = std::pow(0.9f, 1.0f / kDecay) - 1.0f;
constexpr float kMinStability = 0.01f;
constexpr float kMaxStability = 36500.0f;

float clampFloat(const float value, const float minimum, const float maximum) {
  return value < minimum ? minimum : (value > maximum ? maximum : value);
}

int ratingValue(const Rating rating) { return static_cast<int>(rating); }
}  // namespace

Fsrs::Fsrs(const float* params, const float desiredRetention)
    : weights_(params != nullptr ? params : kDefaultParams), desiredRetention_(desiredRetention) {}

float Fsrs::initialStability(const Rating rating) const {
  return clampFloat(weights_[ratingValue(rating) - 1], kMinStability, kMaxStability);
}

float Fsrs::initialDifficulty(const Rating rating) const {
  return clampFloat(weights_[4] - std::exp(weights_[5] * static_cast<float>(ratingValue(rating) - 1)) + 1.0f, 1.0f,
                    10.0f);
}

float Fsrs::nextDifficulty(const float difficulty, const Rating rating) const {
  const float delta = -weights_[6] * static_cast<float>(ratingValue(rating) - 3);
  const float damped = difficulty + delta * ((10.0f - difficulty) / 9.0f);
  const float reverted =
      weights_[7] * initialDifficulty(Rating::Easy) + (1.0f - weights_[7]) * damped;
  return clampFloat(reverted, 1.0f, 10.0f);
}

float Fsrs::shortTermStability(const float stability, const Rating rating) const {
  float increase = std::exp(weights_[17] * (static_cast<float>(ratingValue(rating)) - 3.0f + weights_[18]));
  if (ratingValue(rating) >= 3) {
    if (increase < 1.0f) increase = 1.0f;
  } else if (increase > 1.0f) {
    increase = 1.0f;
  }
  return stability * increase;
}

float Fsrs::recallStability(const float difficulty, const float stability, const float retrievability,
                            const Rating rating) const {
  const float hardPenalty = rating == Rating::Hard ? weights_[15] : 1.0f;
  const float easyBonus = rating == Rating::Easy ? weights_[16] : 1.0f;
  return stability *
         (1.0f + std::exp(weights_[8]) * (11.0f - difficulty) * std::pow(stability, -weights_[9]) *
                     std::expm1((1.0f - retrievability) * weights_[10]) * hardPenalty * easyBonus);
}

float Fsrs::forgetStability(const float difficulty, const float stability, const float retrievability) const {
  return weights_[11] * std::pow(difficulty, -weights_[12]) *
         (std::pow(stability + 1.0f, weights_[13]) - 1.0f) *
         std::exp((1.0f - retrievability) * weights_[14]);
}

float Fsrs::retrievability(const Memory& memory, const float elapsedDays) const {
  if (!memory.learned || memory.stability <= 0.0f) return 1.0f;
  const float elapsed = elapsedDays < 0.0f ? 0.0f : elapsedDays;
  return std::pow(1.0f + kFactor * elapsed / memory.stability, kDecay);
}

Memory Fsrs::review(const Memory& memory, const Rating rating, const int elapsedDays) const {
  Memory result;
  result.learned = true;
  if (!memory.learned) {
    result.stability = initialStability(rating);
    result.difficulty = initialDifficulty(rating);
    return result;
  }

  if (elapsedDays <= 0) {
    result.stability = shortTermStability(memory.stability, rating);
  } else {
    const float recall = retrievability(memory, static_cast<float>(elapsedDays));
    if (rating == Rating::Again) {
      const float forgotten = forgetStability(memory.difficulty, memory.stability, recall);
      result.stability = forgotten < memory.stability ? forgotten : memory.stability;
    } else {
      result.stability = recallStability(memory.difficulty, memory.stability, recall, rating);
    }
  }

  result.stability = clampFloat(result.stability, kMinStability, kMaxStability);
  result.difficulty = nextDifficulty(memory.difficulty, rating);
  return result;
}

int Fsrs::intervalDays(const Memory& memory) const {
  if (!memory.learned || memory.stability <= 0.0f) return 1;
  const float days =
      (memory.stability / kFactor) * (std::pow(desiredRetention_, 1.0f / kDecay) - 1.0f);
  const int rounded = static_cast<int>(days + 0.5f);
  if (rounded < 1) return 1;
  return rounded > maximumInterval_ ? maximumInterval_ : rounded;
}

}  // namespace study
