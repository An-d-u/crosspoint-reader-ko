#include "StudyDeck.h"

#include <algorithm>
#include <cstring>

namespace study {
namespace {
constexpr uint8_t kDeckMagic[8] = {'X', 'S', 'T', 'U', 'D', 'Y', 'D', 0};
constexpr uint8_t kMetaMagic[8] = {'X', 'S', 'T', 'U', 'D', 'Y', 'M', 0};
constexpr uint16_t kFormatVersion = 2;
constexpr int64_t kSecondsPerDay = 86400;

template <typename T>
T readValue(const uint8_t* source) {
  T value;
  std::memcpy(&value, source, sizeof(value));
  return value;
}
}  // namespace

bool DeckMeta::hasParams() const {
  return std::any_of(params, params + kNumParams, [](const float param) { return param != 0.0f; });
}

Memory CardState::memory() const {
  return Memory{stability, difficulty, stability > 0.0f && difficulty > 0.0f};
}

void CardState::setMemory(const Memory& memory) {
  stability = memory.stability;
  difficulty = memory.difficulty;
}

const char* Note::field(const Field field) const {
  const int index = static_cast<int>(field);
  return index >= 0 && index < kFieldCount ? bytes_ + offsets_[index] : "";
}

uint16_t Note::length(const Field field) const {
  const int index = static_cast<int>(field);
  return index >= 0 && index < kFieldCount ? lengths_[index] : 0;
}

bool StudyDeck::openMeta(ByteSource& meta) {
  constexpr uint32_t kHeaderSize = 8 + 2 + 2 + kNumParams * 4 + 4 + 12 + 8 + 1 + 2 + kMaxLearningSteps * 8 + 1;
  uint8_t header[kHeaderSize];
  if (!meta.read(0, header, sizeof(header)) || std::memcmp(header, kMetaMagic, sizeof(kMetaMagic)) != 0 ||
      readValue<uint16_t>(header + 8) != kFormatVersion) {
    return false;
  }

  const uint8_t* cursor = header + 12;
  for (int index = 0; index < kNumParams; ++index) {
    meta_.params[index] = readValue<float>(cursor + index * 4);
  }
  cursor += kNumParams * 4;
  meta_.desiredRetention = readValue<float>(cursor);
  meta_.maximumInterval = readValue<int32_t>(cursor + 4);
  meta_.newPerDay = readValue<int32_t>(cursor + 8);
  meta_.reviewsPerDay = readValue<int32_t>(cursor + 12);
  cursor += 16;
  meta_.collectionCreated = readValue<int64_t>(cursor);
  meta_.rolloverHour = cursor[8];
  cursor += 9;
  meta_.learnStepCount = cursor[0] > kMaxLearningSteps ? kMaxLearningSteps : cursor[0];
  meta_.relearnStepCount = cursor[1] > kMaxLearningSteps ? kMaxLearningSteps : cursor[1];
  cursor += 2;
  for (int index = 0; index < kMaxLearningSteps; ++index) {
    meta_.learnSteps[index] = readValue<float>(cursor + index * 4);
    meta_.relearnSteps[index] = readValue<float>(cursor + (kMaxLearningSteps + index) * 4);
  }
  cursor += kMaxLearningSteps * 8;

  const uint8_t nameLength = cursor[0];
  uint32_t copyLength = nameLength;
  if (copyLength >= sizeof(meta_.name)) copyLength = sizeof(meta_.name) - 1;
  std::memset(meta_.name, 0, sizeof(meta_.name));
  if (nameLength != 0 && !meta.read(kHeaderSize, meta_.name, copyLength)) return false;

  if (!(meta_.desiredRetention > 0.5f && meta_.desiredRetention < 0.999f)) meta_.desiredRetention = 0.9f;
  if (meta_.maximumInterval < 1) meta_.maximumInterval = 36500;
  if (meta_.newPerDay < 0) meta_.newPerDay = 20;
  if (meta_.reviewsPerDay < 1) meta_.reviewsPerDay = 200;
  if (meta_.rolloverHour > 23) meta_.rolloverHour = 4;
  return true;
}

bool StudyDeck::openDeck(ByteSource& deck) {
  uint8_t header[16];
  if (!deck.read(0, header, sizeof(header)) || std::memcmp(header, kDeckMagic, sizeof(kDeckMagic)) != 0 ||
      readValue<uint16_t>(header + 8) != kFormatVersion || header[10] != kFieldCount) {
    return false;
  }
  const uint32_t count = readValue<uint32_t>(header + 12);
  const uint64_t indexBytes = (static_cast<uint64_t>(count) + 1) * 4;
  if (count == 0 || count > 4000000u || 16 + indexBytes > deck.size()) return false;
  noteCount_ = static_cast<int>(count);
  indexBase_ = 16;
  return true;
}

bool StudyDeck::loadNote(ByteSource& deck, const int index, Note& output) const {
  if (index < 0 || index >= noteCount_) return false;
  uint8_t bounds[8];
  if (!deck.read(indexBase_ + static_cast<uint32_t>(index) * 4, bounds, sizeof(bounds))) return false;
  const uint32_t start = readValue<uint32_t>(bounds);
  const uint32_t end = readValue<uint32_t>(bounds + 4);
  if (end <= start || end > deck.size() || end - start + kFieldCount > kMaxNoteBytes) return false;

  const uint32_t recordLength = end - start;
  uint8_t record[kMaxNoteBytes];
  if (!deck.read(start, record, recordLength)) return false;

  uint32_t sourcePosition = 0;
  uint16_t targetPosition = 0;
  for (int field = 0; field < kFieldCount; ++field) {
    if (sourcePosition + 2 > recordLength) return false;
    uint16_t length = readValue<uint16_t>(record + sourcePosition);
    sourcePosition += 2;
    if (sourcePosition + length > recordLength) return false;

    // 문장 필드 끝의 두 바이트는 CrossPlay 강조 범위이므로 텍스트에서 제외한다.
    const uint16_t storedLength = length;
    if (field == static_cast<int>(Field::Sentence)) {
      if (length < 2) return false;
      length -= 2;
    }
    if (targetPosition + length + 1 > kMaxNoteBytes) return false;
    output.offsets_[field] = targetPosition;
    output.lengths_[field] = length;
    std::memcpy(output.bytes_ + targetPosition, record + sourcePosition, length);
    targetPosition = static_cast<uint16_t>(targetPosition + length);
    output.bytes_[targetPosition++] = '\0';
    sourcePosition += storedLength;
  }
  return true;
}

bool StudyDeck::loadCard(ByteSource& cards, const int index, CardState& output) const {
  if (index < 0 || index >= noteCount_) return false;
  uint8_t record[kCardRecordSize];
  if (!cards.read(static_cast<uint32_t>(index) * kCardRecordSize, record, sizeof(record))) return false;
  output.ankiCardId = readValue<int64_t>(record);
  output.stability = readValue<float>(record + 8);
  output.difficulty = readValue<float>(record + 12);
  output.dueDay = readValue<int32_t>(record + 16);
  output.lastReviewDay = readValue<int32_t>(record + 20);
  output.reps = readValue<uint16_t>(record + 24);
  output.lapses = readValue<uint16_t>(record + 26);
  output.state = record[28];
  output.stepIndex = record[29];
  output.dueMinute = readValue<uint16_t>(record + 30);
  return true;
}

bool StudyDeck::storeCard(WritableByteSource& cards, const int index, const CardState& input) const {
  if (index < 0 || index >= noteCount_) return false;
  uint8_t record[kCardRecordSize] = {};
  std::memcpy(record, &input.ankiCardId, 8);
  std::memcpy(record + 8, &input.stability, 4);
  std::memcpy(record + 12, &input.difficulty, 4);
  std::memcpy(record + 16, &input.dueDay, 4);
  std::memcpy(record + 20, &input.lastReviewDay, 4);
  std::memcpy(record + 24, &input.reps, 2);
  std::memcpy(record + 26, &input.lapses, 2);
  record[28] = input.state;
  record[29] = input.stepIndex;
  std::memcpy(record + 30, &input.dueMinute, 2);
  return cards.write(static_cast<uint32_t>(index) * kCardRecordSize, record, sizeof(record));
}

int dayNumber(const DeckMeta& meta, const int64_t nowEpochSeconds) {
  const int64_t elapsed = nowEpochSeconds - meta.collectionCreated;
  return elapsed < 0 ? 0 : static_cast<int>(elapsed / kSecondsPerDay);
}

}  // namespace study
