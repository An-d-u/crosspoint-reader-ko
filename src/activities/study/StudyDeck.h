#pragma once

#include <cstddef>
#include <cstdint>

#include "StudyFsrs.h"

namespace study {

class ByteSource {
 public:
  virtual ~ByteSource() = default;
  virtual bool read(uint32_t offset, void* destination, uint32_t length) = 0;
  virtual uint32_t size() const = 0;
};

class WritableByteSource : public ByteSource {
 public:
  virtual bool write(uint32_t offset, const void* source, uint32_t length) = 0;
  virtual bool flush() = 0;
};

inline constexpr int kFieldCount = 7;
inline constexpr int kMaxLearningSteps = 6;
inline constexpr uint32_t kMaxNoteBytes = 1024;
inline constexpr uint32_t kCardRecordSize = 32;

enum class Field : uint8_t {
  Headword = 0,
  Reading,
  Meaning,
  PartOfSpeech,
  Sentence,
  SentenceReading,
  SentenceMeaning,
};

struct DeckMeta {
  float params[kNumParams] = {};
  float desiredRetention = 0.9f;
  int32_t maximumInterval = 36500;
  int32_t newPerDay = 20;
  int32_t reviewsPerDay = 200;
  int64_t collectionCreated = 0;
  uint8_t rolloverHour = 4;
  float learnSteps[kMaxLearningSteps] = {};
  float relearnSteps[kMaxLearningSteps] = {};
  uint8_t learnStepCount = 0;
  uint8_t relearnStepCount = 0;
  char name[64] = {};

  bool hasParams() const;
};

struct CardState {
  int64_t ankiCardId = 0;
  float stability = 0.0f;
  float difficulty = 0.0f;
  int32_t dueDay = 0;
  int32_t lastReviewDay = -1;
  uint16_t reps = 0;
  uint16_t lapses = 0;
  uint8_t state = 0;
  uint8_t stepIndex = 0;
  uint16_t dueMinute = 0;

  Memory memory() const;
  void setMemory(const Memory& memory);
};

class Note {
 public:
  const char* field(Field field) const;
  uint16_t length(Field field) const;
  bool empty(Field field) const { return length(field) == 0; }

 private:
  friend class StudyDeck;
  char bytes_[kMaxNoteBytes] = {};
  uint16_t offsets_[kFieldCount] = {};
  uint16_t lengths_[kFieldCount] = {};
};

class StudyDeck {
 public:
  bool openMeta(ByteSource& meta);
  bool openDeck(ByteSource& deck);
  bool loadNote(ByteSource& deck, int index, Note& output) const;
  bool loadCard(ByteSource& cards, int index, CardState& output) const;
  bool storeCard(WritableByteSource& cards, int index, const CardState& input) const;

  int noteCount() const { return noteCount_; }
  const DeckMeta& meta() const { return meta_; }

 private:
  DeckMeta meta_;
  int noteCount_ = 0;
  uint32_t indexBase_ = 0;
};

int dayNumber(const DeckMeta& meta, int64_t nowEpochSeconds);

}  // namespace study
