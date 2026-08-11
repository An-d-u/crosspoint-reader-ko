#pragma once

#include <cstdint>

namespace study {

enum class Rating : uint8_t { Again = 1, Hard = 2, Good = 3, Easy = 4 };

inline constexpr int kNumParams = 19;
extern const float kDefaultParams[kNumParams];

struct Memory {
  float stability = 0.0f;
  float difficulty = 0.0f;
  bool learned = false;
};

class Fsrs {
 public:
  explicit Fsrs(const float* params = nullptr, float desiredRetention = 0.9f);

  float retrievability(const Memory& memory, float elapsedDays) const;
  Memory review(const Memory& memory, Rating rating, int elapsedDays) const;
  int intervalDays(const Memory& memory) const;
  void setMaximumInterval(int days) { maximumInterval_ = days; }

 private:
  float initialStability(Rating rating) const;
  float initialDifficulty(Rating rating) const;
  float nextDifficulty(float difficulty, Rating rating) const;
  float shortTermStability(float stability, Rating rating) const;
  float recallStability(float difficulty, float stability, float retrievability, Rating rating) const;
  float forgetStability(float difficulty, float stability, float retrievability) const;

  const float* weights_;
  float desiredRetention_;
  int maximumInterval_ = 36500;
};

}  // namespace study
