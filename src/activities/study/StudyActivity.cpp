#include "StudyActivity.h"

#include <Arduino.h>
#include <I18n.h>
#include <WiFi.h>
#include <esp_sntp.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr const char* kStudyRoot = "/study";
constexpr int64_t kReasonableEpoch = 1704067200;  // 2024-01-01
constexpr int64_t kSecondsPerDay = 86400;
constexpr unsigned long kTimeSyncTimeoutMs = 8000;

struct StudyTextToken {
  std::string base;
  std::string ruby;
  int width = 0;
};

struct StudyTextLine {
  std::vector<StudyTextToken> tokens;
  int width = 0;
  bool hasRuby = false;
};

struct StudyRubyRun {
  const char* text;
  int preferredX;
  int x;
  int width;
};

void placeStudyRubyCluster(std::vector<StudyRubyRun>& runs, const size_t start, const size_t endExclusive) {
  if (start >= endExclusive) return;
  if (endExclusive - start == 1) {
    runs[start].x = runs[start].preferredX;
    return;
  }

  double weightedCenterSum = 0.0;
  int totalWeight = 0;
  int totalWidth = 0;
  for (size_t index = start; index < endExclusive; ++index) {
    const int weight = std::max(1, runs[index].width);
    weightedCenterSum += (runs[index].preferredX + runs[index].width / 2.0) * weight;
    totalWeight += weight;
    totalWidth += runs[index].width;
  }

  const double clusterCenter = weightedCenterSum / std::max(1, totalWeight);
  int currentX = static_cast<int>(std::lround(clusterCenter - totalWidth / 2.0));
  for (size_t index = start; index < endExclusive; ++index) {
    runs[index].x = currentX;
    currentX += runs[index].width;
  }
}

void resolveStudyRubyOverlaps(std::vector<StudyRubyRun>& runs) {
  if (runs.size() < 2) return;

  size_t clusterStart = 0;
  while (clusterStart < runs.size()) {
    size_t clusterEnd = clusterStart + 1;
    int preferredRight = runs[clusterStart].preferredX + runs[clusterStart].width;
    while (clusterEnd < runs.size() && runs[clusterEnd].preferredX < preferredRight) {
      preferredRight = std::max(preferredRight, runs[clusterEnd].preferredX + runs[clusterEnd].width);
      ++clusterEnd;
    }
    placeStudyRubyCluster(runs, clusterStart, clusterEnd);
    clusterStart = clusterEnd;
  }

  bool merged = true;
  while (merged) {
    merged = false;
    for (size_t index = 1; index < runs.size(); ++index) {
      if (runs[index - 1].x + runs[index - 1].width <= runs[index].x) continue;

      size_t overlapStart = index - 1;
      while (overlapStart > 0 && runs[overlapStart - 1].x + runs[overlapStart - 1].width > runs[overlapStart].x) {
        --overlapStart;
      }
      size_t overlapEnd = index + 1;
      while (overlapEnd < runs.size() && runs[overlapEnd - 1].x + runs[overlapEnd - 1].width > runs[overlapEnd].x) {
        ++overlapEnd;
      }
      placeStudyRubyCluster(runs, overlapStart, overlapEnd);
      merged = true;
      break;
    }
  }
}

void clampStudyRubyRuns(std::vector<StudyRubyRun>& runs, const int left, const int right) {
  const int availableWidth = right - left;
  if (availableWidth <= 0) return;
  for (auto& run : runs) {
    run.x = run.width >= availableWidth ? left : std::max(left, std::min(run.x, right - run.width));
  }
}

uint32_t decodeCodepoint(const std::string& text, const size_t offset, size_t& next) {
  const auto first = static_cast<uint8_t>(text[offset]);
  size_t length = 1;
  uint32_t codepoint = first;
  if ((first & 0xE0) == 0xC0) {
    length = 2;
    codepoint = first & 0x1F;
  } else if ((first & 0xF0) == 0xE0) {
    length = 3;
    codepoint = first & 0x0F;
  } else if ((first & 0xF8) == 0xF0) {
    length = 4;
    codepoint = first & 0x07;
  }
  if (offset + length > text.size()) length = 1;
  for (size_t index = 1; index < length; ++index) {
    const auto continuation = static_cast<uint8_t>(text[offset + index]);
    if ((continuation & 0xC0) != 0x80) {
      length = 1;
      codepoint = first;
      break;
    }
    codepoint = (codepoint << 6) | (continuation & 0x3F);
  }
  next = offset + length;
  return codepoint;
}

bool isHanCodepoint(const uint32_t codepoint) {
  return (codepoint >= 0x3400 && codepoint <= 0x4DBF) ||
         (codepoint >= 0x4E00 && codepoint <= 0x9FFF) ||
         (codepoint >= 0xF900 && codepoint <= 0xFAFF) ||
         (codepoint >= 0x20000 && codepoint <= 0x2FA1F);
}

void appendPlainTokens(std::vector<StudyTextToken>& tokens, const std::string& text) {
  for (size_t offset = 0; offset < text.size();) {
    size_t next = offset + 1;
    decodeCodepoint(text, offset, next);
    tokens.push_back({text.substr(offset, next - offset), {}});
    offset = next;
  }
}

std::vector<StudyTextToken> parseStudyText(const std::string& text) {
  std::vector<StudyTextToken> tokens;
  tokens.reserve(text.size() / 3 + 1);
  for (size_t offset = 0; offset < text.size();) {
    if (text[offset] == '[') {
      const size_t baseEnd = text.find(']', offset + 1);
      if (baseEnd != std::string::npos) {
        const std::string base = text.substr(offset + 1, baseEnd - offset - 1);
        if (baseEnd + 1 < text.size() && text[baseEnd + 1] == '(') {
          const size_t rubyEnd = text.find(')', baseEnd + 2);
          if (rubyEnd != std::string::npos) {
            tokens.push_back({base, text.substr(baseEnd + 2, rubyEnd - baseEnd - 2)});
            offset = rubyEnd + 1;
            continue;
          }
        }
        appendPlainTokens(tokens, base);
        offset = baseEnd + 1;
        continue;
      }
    }

    size_t next = offset + 1;
    const uint32_t codepoint = decodeCodepoint(text, offset, next);
    if (isHanCodepoint(codepoint)) {
      size_t baseEnd = next;
      while (baseEnd < text.size()) {
        size_t following = baseEnd + 1;
        if (!isHanCodepoint(decodeCodepoint(text, baseEnd, following))) break;
        baseEnd = following;
      }
      if (baseEnd < text.size() && text[baseEnd] == '(') {
        const size_t rubyEnd = text.find(')', baseEnd + 1);
        if (rubyEnd != std::string::npos) {
          tokens.push_back({text.substr(offset, baseEnd - offset), text.substr(baseEnd + 1, rubyEnd - baseEnd - 1)});
          offset = rubyEnd + 1;
          continue;
        }
      }
    }

    tokens.push_back({text.substr(offset, next - offset), {}});
    offset = next;
  }
  return tokens;
}

std::string enrichHeadwordRuby(const char* headword, const char* sentence) {
  const std::string source = sentence == nullptr ? std::string{} : sentence;
  std::vector<StudyTextToken> mappings = parseStudyText(source);
  mappings.erase(std::remove_if(mappings.begin(), mappings.end(), [](const StudyTextToken& token) {
                   return token.ruby.empty();
                 }),
                 mappings.end());
  std::sort(mappings.begin(), mappings.end(), [](const StudyTextToken& left, const StudyTextToken& right) {
    return left.base.size() > right.base.size();
  });

  const std::string input = headword == nullptr ? std::string{} : headword;
  std::string output;
  output.reserve(input.size() + 16);
  for (size_t offset = 0; offset < input.size();) {
    const auto mapping = std::find_if(mappings.begin(), mappings.end(), [&](const StudyTextToken& token) {
      return !token.base.empty() && input.compare(offset, token.base.size(), token.base) == 0;
    });
    if (mapping != mappings.end()) {
      output.push_back('[');
      output += mapping->base;
      output += "](";
      output += mapping->ruby;
      output.push_back(')');
      offset += mapping->base.size();
      continue;
    }
    size_t next = offset + 1;
    decodeCodepoint(input, offset, next);
    output.append(input, offset, next - offset);
    offset = next;
  }
  return output;
}

std::vector<StudyTextLine> wrapStudyText(const GfxRenderer& renderer, const int fontId, const int rubyFontId,
                                         std::vector<StudyTextToken> tokens, const int maxWidth,
                                         const int maxLines, const EpdFontFamily::Style style) {
  (void)rubyFontId;
  const auto measure = [&](StudyTextToken& token) {
    const int baseWidth = renderer.getTextAdvanceX(fontId, token.base.c_str(), style);
    token.width = baseWidth;
  };
  for (auto& token : tokens) measure(token);

  std::vector<StudyTextLine> lines;
  StudyTextLine line;
  auto finishLine = [&]() {
    if (!line.tokens.empty()) lines.push_back(std::move(line));
    line = StudyTextLine{};
  };
  for (StudyTextToken& token : tokens) {
    if (token.base == "\n") {
      finishLine();
      if (static_cast<int>(lines.size()) >= maxLines) break;
      continue;
    }
    if (!line.tokens.empty() && line.width + token.width > maxWidth) {
      if (static_cast<int>(lines.size()) + 1 >= maxLines) {
        StudyTextToken ellipsis{"…", {}};
        measure(ellipsis);
        while (!line.tokens.empty() && line.width + ellipsis.width > maxWidth) {
          line.width -= line.tokens.back().width;
          line.tokens.pop_back();
        }
        line.width += ellipsis.width;
        line.tokens.push_back(std::move(ellipsis));
        finishLine();
        return lines;
      }
      finishLine();
    }
    if (token.width > maxWidth) {
      token.base = renderer.truncatedText(fontId, token.base.c_str(), maxWidth, style);
      token.ruby.clear();
      measure(token);
    }
    line.width += token.width;
    line.hasRuby = line.hasRuby || !token.ruby.empty();
    line.tokens.push_back(std::move(token));
  }
  if (static_cast<int>(lines.size()) < maxLines) finishLine();
  return lines;
}

const char* basenameOf(const char* path) {
  const char* slash = std::strrchr(path, '/');
  return slash == nullptr ? path : slash + 1;
}
}  // namespace

class StudyFileSource final : public study::WritableByteSource {
 public:
  explicit StudyFileSource(HalFile& file, const bool writable = false) : file_(file), writable_(writable) {}

  bool read(const uint32_t offset, void* destination, const uint32_t length) override {
    return file_.seekSet(offset) && file_.read(destination, length) == static_cast<int>(length);
  }

  uint32_t size() const override { return static_cast<uint32_t>(file_.size()); }

  bool write(const uint32_t offset, const void* source, const uint32_t length) override {
    return writable_ && file_.seekSet(offset) && file_.write(source, length) == length;
  }

  bool flush() override {
    if (!writable_) return false;
    file_.flush();
    return true;
  }

 private:
  HalFile& file_;
  bool writable_;
};

StudyActivity::StudyActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("Study", renderer, mappedInput), fsrs_(nullptr), scheduler_(fsrs_, study::Steps::defaults()) {}

StudyActivity::~StudyActivity() = default;

void StudyActivity::onEnter() {
  Activity::onEnter();
  if (!findDecks()) {
    view_ = View::NoDeck;
    requestUpdate();
    return;
  }

  if (static_cast<int64_t>(std::time(nullptr)) >= kReasonableEpoch) {
    openSelectedDeck();
    return;
  }

  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) {
                           if (!result.isCancelled && WiFi.status() == WL_CONNECTED) {
                             beginTimeSync();
                           } else {
                             finishTimeSync();
                           }
                         });
}

void StudyActivity::onExit() {
  if (esp_sntp_enabled()) esp_sntp_stop();
  WiFi.disconnect(false);
  WiFi.mode(WIFI_OFF);
  closeDeck();
  Activity::onExit();
}

void StudyActivity::openSelectedDeck() {
  if (openDeckAt(deckIndex_)) {
    prepareDeck();
    view_ = View::Deck;
  } else {
    view_ = View::NoDeck;
  }
  requestUpdate();
}

void StudyActivity::beginTimeSync() {
  if (esp_sntp_enabled()) esp_sntp_stop();
  esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
  esp_sntp_setservername(0, "pool.ntp.org");
  esp_sntp_init();
  timeSyncStartedAt_ = millis();
  view_ = View::SyncingTime;
  requestUpdate(true);
}

void StudyActivity::finishTimeSync() {
  if (esp_sntp_enabled()) esp_sntp_stop();
  WiFi.disconnect(false);
  WiFi.mode(WIFI_OFF);
  openSelectedDeck();
}

bool StudyActivity::findDecks() {
  deckCount_ = 0;
  HalFile root = Storage.open(kStudyRoot, O_RDONLY);
  if (!root.isOpen() || !root.isDirectory()) return false;

  for (HalFile entry = root.openNextFile(); entry && deckCount_ < kMaxDecks; entry = root.openNextFile()) {
    if (!entry.isDirectory()) continue;
    char rawName[96] = {};
    entry.getName(rawName, sizeof(rawName));
    const char* name = basenameOf(rawName);
    if (*name == '\0' || std::strcmp(name, "fonts") == 0) continue;
    char path[128];
    std::snprintf(path, sizeof(path), "%s/%s/meta.dat", kStudyRoot, name);
    if (!Storage.exists(path)) continue;
    std::snprintf(deckNames_[deckCount_], sizeof(deckNames_[0]), "%s", name);
    ++deckCount_;
  }
  for (int index = 1; index < deckCount_; ++index) {
    for (int current = index; current > 0 && std::strcmp(deckNames_[current], deckNames_[current - 1]) < 0;
         --current) {
      char temporary[sizeof(deckNames_[0])];
      std::memcpy(temporary, deckNames_[current], sizeof(temporary));
      std::memcpy(deckNames_[current], deckNames_[current - 1], sizeof(temporary));
      std::memcpy(deckNames_[current - 1], temporary, sizeof(temporary));
    }
  }

  HalFile lastFile;
  if (Storage.openFileForRead("STUDY", "/study/.last", lastFile)) {
    char last[32] = {};
    const int count = lastFile.read(last, sizeof(last) - 1);
    if (count > 0) last[count] = '\0';
    for (int index = 0; index < deckCount_; ++index) {
      if (std::strcmp(last, deckNames_[index]) == 0) deckIndex_ = index;
    }
  }
  return deckCount_ > 0;
}

bool StudyActivity::openDeckAt(const int index) {
  if (index < 0 || index >= deckCount_) return false;
  deckIndex_ = index;
  std::snprintf(deckDir_, sizeof(deckDir_), "%s/%s", kStudyRoot, deckNames_[index]);

  HalFile lastFile;
  if (Storage.openFileForWrite("STUDY", "/study/.last", lastFile)) {
    lastFile.write(deckNames_[index], std::strlen(deckNames_[index]));
  }
  return openDeck();
}

bool StudyActivity::openDeck() {
  char path[128];
  std::snprintf(path, sizeof(path), "%s/meta.dat", deckDir_);
  if (!Storage.openFileForRead("STUDY", path, metaFile_)) return false;
  std::snprintf(path, sizeof(path), "%s/deck.dat", deckDir_);
  if (!Storage.openFileForRead("STUDY", path, deckFile_)) return false;
  std::snprintf(path, sizeof(path), "%s/cards.dat", deckDir_);
  cardFile_ = Storage.open(path, O_RDWR);
  if (!cardFile_.isOpen()) return false;
  std::snprintf(path, sizeof(path), "%s/revlog.dat", deckDir_);
  revlogFile_ = Storage.open(path, O_RDWR | O_CREAT);

  metaSource_ = std::make_unique<StudyFileSource>(metaFile_);
  deckSource_ = std::make_unique<StudyFileSource>(deckFile_);
  cardSource_ = std::make_unique<StudyFileSource>(cardFile_, true);
  return deck_.openMeta(*metaSource_) && deck_.openDeck(*deckSource_);
}

void StudyActivity::closeDeck() {
  if (cardSource_) cardSource_->flush();
  if (revlogFile_.isOpen()) revlogFile_.flush();
  metaSource_.reset();
  deckSource_.reset();
  cardSource_.reset();
  if (metaFile_.isOpen()) metaFile_.close();
  if (deckFile_.isOpen()) deckFile_.close();
  if (cardFile_.isOpen()) cardFile_.close();
  if (revlogFile_.isOpen()) revlogFile_.close();
}

void StudyActivity::switchDeck(const int direction) {
  if (deckCount_ < 2) return;
  closeDeck();
  const int next = (deckIndex_ + direction + deckCount_) % deckCount_;
  if (openDeckAt(next)) {
    prepareDeck();
    view_ = View::Deck;
  } else {
    view_ = View::NoDeck;
  }
  requestUpdate();
}

void StudyActivity::prepareDeck() {
  clockReady_ = static_cast<int64_t>(std::time(nullptr)) >= kReasonableEpoch;
  today_ = resolveToday();
  fallbackMinute_ = 720;
  sessionStartedAt_ = millis();
  fsrs_ = study::Fsrs(deck_.meta().hasParams() ? deck_.meta().params : nullptr, deck_.meta().desiredRetention);
  fsrs_.setMaximumInterval(deck_.meta().maximumInterval);

  study::Steps steps;
  steps.learnCount = deck_.meta().learnStepCount;
  steps.relearnCount = deck_.meta().relearnStepCount;
  for (int index = 0; index < study::kMaxLearningSteps; ++index) {
    steps.learn[index] = deck_.meta().learnSteps[index];
    steps.relearn[index] = deck_.meta().relearnSteps[index];
  }
  if (steps.learnCount == 0 && steps.relearnCount == 0) steps = study::Steps::defaults();
  scheduler_ = study::Scheduler(fsrs_, steps);
  pendingCount_ = 0;
  reviewedCount_ = 0;
  lastReviewAtMs_ = 0;
  writeFailed_ = false;
  buildQueue();
}

int StudyActivity::resolveToday() {
  const int64_t now = static_cast<int64_t>(std::time(nullptr));
  if (now >= kReasonableEpoch && now >= deck_.meta().collectionCreated) {
    return study::dayNumber(deck_.meta(), now);
  }

  // 시계가 설정되지 않은 기기에서는 마지막 Anki 복습일을 안전한 하한으로 사용한다.
  int latestReviewDay = 0;
  study::CardState probe;
  for (int index = 0; index < deck_.noteCount(); ++index) {
    if (deck_.loadCard(*cardSource_, index, probe) && probe.lastReviewDay > latestReviewDay) {
      latestReviewDay = probe.lastReviewDay;
    }
  }
  return latestReviewDay;
}

int StudyActivity::nowMinute() const {
  const int64_t now = static_cast<int64_t>(std::time(nullptr));
  if (now >= kReasonableEpoch) {
    const int64_t dayStart = deck_.meta().collectionCreated + static_cast<int64_t>(today_) * kSecondsPerDay;
    const int64_t seconds = now - dayStart;
    if (seconds >= 0 && seconds < kSecondsPerDay) return static_cast<int>(seconds / 60);
  }
  const unsigned long elapsedMinutes = (millis() - sessionStartedAt_) / 60000UL;
  const int minute = fallbackMinute_ + static_cast<int>(elapsedMinutes);
  return minute > 1439 ? 1439 : minute;
}

int64_t StudyActivity::reviewEpochMilliseconds() {
  const int64_t now = static_cast<int64_t>(std::time(nullptr));
  int64_t candidate;
  if (now >= kReasonableEpoch) {
    candidate = now * 1000 + static_cast<int64_t>(millis() % 1000UL);
  } else {
    const int64_t dayStart = deck_.meta().collectionCreated + static_cast<int64_t>(today_) * kSecondsPerDay;
    candidate = (dayStart + fallbackMinute_ * 60) * 1000 + static_cast<int64_t>(millis() - sessionStartedAt_);
  }
  if (candidate <= lastReviewAtMs_) candidate = lastReviewAtMs_ + 1;
  lastReviewAtMs_ = candidate;
  return candidate;
}

void StudyActivity::buildQueue() {
  queueCount_ = 0;
  queuePosition_ = 0;
  dueCount_ = 0;
  newCount_ = 0;
  const int minute = nowMinute();
  study::CardState probe;

  for (int index = 0; index < deck_.noteCount(); ++index) {
    if (!deck_.loadCard(*cardSource_, index, probe)) continue;
    const study::State state = static_cast<study::State>(probe.state);
    if (state == study::State::Suspended) continue;
    if (state == study::State::New) {
      ++newCount_;
    } else if (study::Scheduler::isDue(probe, today_, minute)) {
      ++dueCount_;
    }
  }

  for (int pass = 0; pass < 2 && queueCount_ < kMaxQueue; ++pass) {
    const int limit = pass == 0 ? deck_.meta().reviewsPerDay : deck_.meta().newPerDay;
    int added = 0;
    for (int index = 0; index < deck_.noteCount() && queueCount_ < kMaxQueue && added < limit; ++index) {
      if (!deck_.loadCard(*cardSource_, index, probe)) continue;
      const study::State state = static_cast<study::State>(probe.state);
      if (state == study::State::Suspended) continue;
      const bool isNew = state == study::State::New;
      if ((pass == 0 && (isNew || !study::Scheduler::isDue(probe, today_, minute))) ||
          (pass == 1 && !isNew)) {
        continue;
      }
      queue_[queueCount_++] = index;
      ++added;
    }
  }
}

bool StudyActivity::takeNext() {
  const int minute = nowMinute();
  int selectedPending = -1;
  for (int index = 0; index < pendingCount_; ++index) {
    study::CardState probe;
    probe.state = static_cast<uint8_t>(study::State::Learning);
    probe.dueDay = pending_[index].dueDay;
    probe.dueMinute = static_cast<uint16_t>(pending_[index].dueMinute);
    if (study::Scheduler::isDue(probe, today_, minute)) {
      selectedPending = index;
      break;
    }
  }

  if (selectedPending < 0 && queuePosition_ < queueCount_) {
    currentIndex_ = queue_[queuePosition_++];
    return loadCurrent();
  }
  if (selectedPending < 0 && pendingCount_ > 0) {
    selectedPending = 0;
    for (int index = 1; index < pendingCount_; ++index) {
      if (pending_[index].dueDay < pending_[selectedPending].dueDay ||
          (pending_[index].dueDay == pending_[selectedPending].dueDay &&
           pending_[index].dueMinute < pending_[selectedPending].dueMinute)) {
        selectedPending = index;
      }
    }
  }
  if (selectedPending < 0) return false;
  currentIndex_ = pending_[selectedPending].index;
  pending_[selectedPending] = pending_[--pendingCount_];
  return loadCurrent();
}

bool StudyActivity::loadCurrent() {
  if (!deck_.loadNote(*deckSource_, currentIndex_, note_) ||
      !deck_.loadCard(*cardSource_, currentIndex_, card_)) {
    return false;
  }
  scheduler_.preview(card_, today_, nowMinute(), previews_);
  return true;
}

bool StudyActivity::persistReview(const study::CardState& before, const study::Outcome& outcome,
                                  const study::Rating rating) {
  bool success = deck_.storeCard(*cardSource_, currentIndex_, outcome.card);
  if (revlogFile_.isOpen()) {
    uint8_t record[32] = {};
    const int64_t reviewedAtMs = reviewEpochMilliseconds();
    const int elapsed = before.lastReviewDay < 0 ? 0 : today_ - before.lastReviewDay;
    const int16_t elapsedDays = static_cast<int16_t>(elapsed < -32768 ? -32768 : (elapsed > 32767 ? 32767 : elapsed));
    const int32_t interval = outcome.intervalDays > 0 ? outcome.intervalDays : -outcome.delayMinutes * 60;
    std::memcpy(record, &before.ankiCardId, 8);
    std::memcpy(record + 8, &reviewedAtMs, 8);
    record[16] = static_cast<uint8_t>(rating);
    record[17] = before.state;
    std::memcpy(record + 18, &elapsedDays, 2);
    std::memcpy(record + 20, &interval, 4);
    const size_t end = revlogFile_.size();
    success = revlogFile_.seekSet(end) && revlogFile_.write(record, sizeof(record)) == sizeof(record) && success;
  } else {
    success = false;
  }
  cardSource_->flush();
  if (revlogFile_.isOpen()) revlogFile_.flush();
  return success;
}

void StudyActivity::grade(const study::Rating rating) {
  const study::Outcome outcome = scheduler_.answer(card_, rating, today_, nowMinute());
  if (!persistReview(card_, outcome, rating)) writeFailed_ = true;
  if (outcome.delayMinutes > 0 && pendingCount_ < kMaxPending) {
    pending_[pendingCount_++] = {currentIndex_, outcome.card.dueDay, outcome.card.dueMinute};
  }
  ++reviewedCount_;
  if (takeNext()) {
    view_ = View::Question;
  } else {
    buildQueue();
    view_ = View::Deck;
  }
  requestUpdate();
}

void StudyActivity::loop() {
  if (view_ == View::SyncingTime) {
    const bool cancelled = mappedInput.wasReleased(MappedInputManager::Button::Back);
    const bool clockUpdated = static_cast<int64_t>(std::time(nullptr)) >= kReasonableEpoch;
    const bool timedOut = millis() - timeSyncStartedAt_ >= kTimeSyncTimeoutMs;
    if (cancelled || clockUpdated || timedOut) finishTimeSync();
    return;
  }

  if (view_ == View::NoDeck) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) onGoHome();
    return;
  }

  if (view_ == View::Deck) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      onGoHome();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      switchDeck(-1);
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      switchDeck(1);
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      buildQueue();
      if (takeNext()) view_ = View::Question;
      requestUpdate();
    }
    return;
  }

  if (view_ == View::Question) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      view_ = View::Deck;
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      view_ = View::Answer;
      requestUpdate();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    grade(study::Rating::Again);
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    grade(study::Rating::Hard);
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    grade(study::Rating::Good);
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    grade(study::Rating::Easy);
  }
}

int StudyActivity::drawWrapped(const int fontId, const int y, const char* text, int maxLines,
                               const bool bold, const int bottomY, const char* rubySource) const {
  if (text == nullptr || *text == '\0') return y;
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth() - metrics.contentSidePadding * 2;
  const auto style = bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
  const std::string enriched = rubySource == nullptr ? std::string(text) : enrichHeadwordRuby(text, rubySource);
  auto tokens = parseStudyText(enriched);
  const bool hasRuby = std::any_of(tokens.begin(), tokens.end(), [](const StudyTextToken& token) {
    return !token.ruby.empty();
  });
  const int baseLineHeight = renderer.getTextHeight(fontId) + 6;
  const int rubyLineHeight = hasRuby ? renderer.getTextHeight(UI_10_FONT_ID) + 3 : 0;
  if (bottomY > y) {
    maxLines = std::min(maxLines, (bottomY - y) / std::max(1, baseLineHeight + rubyLineHeight));
  }
  if (maxLines <= 0) return y;
  const auto lines = wrapStudyText(renderer, fontId, UI_10_FONT_ID, std::move(tokens), width, maxLines, style);
  int currentY = y;
  for (const StudyTextLine& line : lines) {
    const int lineRubyHeight = line.hasRuby ? renderer.getTextHeight(UI_10_FONT_ID) + 3 : 0;
    int x = (renderer.getScreenWidth() - line.width) / 2;
    std::vector<StudyRubyRun> rubyRuns;
    if (line.hasRuby) rubyRuns.reserve(line.tokens.size());
    for (const StudyTextToken& token : line.tokens) {
      const int baseWidth = renderer.getTextAdvanceX(fontId, token.base.c_str(), style);
      renderer.drawText(fontId, x, currentY + lineRubyHeight, token.base.c_str(), true, style);
      if (!token.ruby.empty()) {
        const int rubyWidth = renderer.getTextAdvanceX(UI_10_FONT_ID, token.ruby.c_str());
        const int preferredX = x + (baseWidth - rubyWidth) / 2;
        rubyRuns.push_back({token.ruby.c_str(), preferredX, preferredX, rubyWidth});
      }
      x += token.width;
    }
    resolveStudyRubyOverlaps(rubyRuns);
    clampStudyRubyRuns(rubyRuns, metrics.contentSidePadding, renderer.getScreenWidth() - metrics.contentSidePadding);
    for (const StudyRubyRun& run : rubyRuns) {
      renderer.drawText(UI_10_FONT_ID, run.x, currentY, run.text);
    }
    currentY += baseLineHeight + lineRubyHeight;
  }
  return currentY;
}

void StudyActivity::drawDeckScreen() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, width, metrics.headerHeight}, tr(STR_STUDY_TITLE),
                 deck_.meta().name);

  int y = metrics.topPadding + metrics.headerHeight + 35;
  y = drawWrapped(KOPUB_14_FONT_ID, y, deck_.meta().name, 2, true) + 25;
  char summary[96];
  std::snprintf(summary, sizeof(summary), tr(STR_STUDY_SUMMARY), dueCount_, newCount_, deck_.noteCount());
  y = drawWrapped(UI_10_FONT_ID, y, summary, 2) + 18;
  if (reviewedCount_ > 0) {
    char reviewed[64];
    std::snprintf(reviewed, sizeof(reviewed), tr(STR_STUDY_REVIEWED), reviewedCount_);
    y = drawWrapped(UI_10_FONT_ID, y, reviewed, 1) + 12;
  }
  if (writeFailed_) y = drawWrapped(UI_10_FONT_ID, y, tr(STR_STUDY_WRITE_FAILED), 2, true);
  if (!clockReady_) y = drawWrapped(UI_10_FONT_ID, y + 8, tr(STR_STUDY_CLOCK_WARNING), 3, true);
  if (queueCount_ == 0) drawWrapped(UI_10_FONT_ID, y + 15, tr(STR_STUDY_NOTHING_DUE), 2);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_STUDY_START), tr(STR_STUDY_PREV_DECK),
                                            tr(STR_STUDY_NEXT_DECK));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void StudyActivity::drawCardScreen(const bool answer) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  char remaining[32];
  std::snprintf(remaining, sizeof(remaining), "%d", queueCount_ - queuePosition_ + pendingCount_ + 1);
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, width, metrics.headerHeight}, deck_.meta().name, remaining);

  int y = metrics.topPadding + metrics.headerHeight + 30;
  const char* headwordRubySource =
      note_.empty(study::Field::Reading) ? note_.field(study::Field::Sentence) : nullptr;
  y = drawWrapped(KOPUB_14_FONT_ID, y, note_.field(study::Field::Headword), 3, true, 0, headwordRubySource);
  if (answer) {
    y += 15;
    renderer.drawLine(metrics.contentSidePadding, y, width - metrics.contentSidePadding, y);
    y += 20;
    y = drawWrapped(UI_10_FONT_ID, y, note_.field(study::Field::Reading), 2);
    y = drawWrapped(KOPUB_14_FONT_ID, y + 4, note_.field(study::Field::Meaning), 4);
    y = drawWrapped(UI_10_FONT_ID, y, note_.field(study::Field::PartOfSpeech), 1);
    if (!note_.empty(study::Field::Sentence)) {
      y += 15;
      renderer.drawLine(metrics.contentSidePadding, y, width - metrics.contentSidePadding, y);
      y += 18;
      const int contentBottom = renderer.getScreenHeight() - metrics.buttonHintsHeight - metrics.verticalSpacing - 4;
      const int smallLineHeight = renderer.getTextHeight(UI_10_FONT_ID) + 6;
      const int readingReserve = note_.empty(study::Field::SentenceReading) ? 0 : smallLineHeight;
      const int meaningReserve = note_.empty(study::Field::SentenceMeaning) ? 0 : smallLineHeight;
      y = drawWrapped(KOPUB_14_FONT_ID, y, note_.field(study::Field::Sentence), 8, false,
                      contentBottom - readingReserve - meaningReserve);
      y = drawWrapped(UI_10_FONT_ID, y, note_.field(study::Field::SentenceReading), 4, false,
                      contentBottom - meaningReserve);
      drawWrapped(UI_10_FONT_ID, y, note_.field(study::Field::SentenceMeaning), 8, false, contentBottom);
    }

    char again[20], hard[20], good[20], easy[20];
    study::formatDelay(previews_[0].delayMinutes, previews_[0].intervalDays, again, sizeof(again));
    study::formatDelay(previews_[1].delayMinutes, previews_[1].intervalDays, hard, sizeof(hard));
    study::formatDelay(previews_[2].delayMinutes, previews_[2].intervalDays, good, sizeof(good));
    study::formatDelay(previews_[3].delayMinutes, previews_[3].intervalDays, easy, sizeof(easy));
    char againLabel[32], hardLabel[32], goodLabel[32], easyLabel[32];
    std::snprintf(againLabel, sizeof(againLabel), "%s %s", tr(STR_STUDY_AGAIN), again);
    std::snprintf(hardLabel, sizeof(hardLabel), "%s %s", tr(STR_STUDY_HARD), hard);
    std::snprintf(goodLabel, sizeof(goodLabel), "%s %s", tr(STR_STUDY_GOOD), good);
    std::snprintf(easyLabel, sizeof(easyLabel), "%s %s", tr(STR_STUDY_EASY), easy);
    const auto labels = mappedInput.mapLabels(againLabel, goodLabel, hardLabel, easyLabel);
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_STUDY_SHOW_ANSWER), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
}

void StudyActivity::render(RenderLock&&) {
  renderer.clearScreen();
  if (view_ == View::NoDeck) {
    const auto& metrics = UITheme::getInstance().getMetrics();
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight},
                   tr(STR_STUDY_TITLE));
    int y = metrics.topPadding + metrics.headerHeight + 45;
    y = drawWrapped(UI_12_FONT_ID, y, tr(STR_STUDY_NO_DECK), 2, true) + 20;
    drawWrapped(UI_10_FONT_ID, y, tr(STR_STUDY_INSTALL_HINT), 5);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (view_ == View::SyncingTime) {
    const auto& metrics = UITheme::getInstance().getMetrics();
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight},
                   tr(STR_STUDY_TITLE));
    renderer.drawCenteredText(UI_12_FONT_ID, renderer.getScreenHeight() / 2, tr(STR_SYNCING_TIME), true,
                              EpdFontFamily::BOLD);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (view_ == View::Deck) {
    drawDeckScreen();
  } else {
    drawCardScreen(view_ == View::Answer);
  }
  renderer.displayBuffer();
}
