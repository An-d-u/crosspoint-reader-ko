#pragma once

#include <HalStorage.h>

#include <memory>

#include "../Activity.h"
#include "StudyDeck.h"
#include "StudyFsrs.h"
#include "StudyScheduler.h"

class StudyFileSource;

class StudyActivity final : public Activity {
 public:
  StudyActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  ~StudyActivity() override;

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return view_ == View::SyncingTime; }

 private:
  enum class View : uint8_t { NoDeck, SyncingTime, Deck, Question, Answer };

  struct PendingCard {
    int index = -1;
    int dueDay = 0;
    int dueMinute = 0;
  };

  static constexpr int kMaxDecks = 24;
  static constexpr int kMaxQueue = 256;
  static constexpr int kMaxPending = 32;

  bool findDecks();
  bool openDeckAt(int index);
  bool openDeck();
  void closeDeck();
  void switchDeck(int direction);
  void openSelectedDeck();
  void beginTimeSync();
  void finishTimeSync();
  void prepareDeck();
  void buildQueue();
  bool takeNext();
  bool loadCurrent();
  void grade(study::Rating rating);
  bool persistReview(const study::CardState& before, const study::Outcome& outcome, study::Rating rating);

  int resolveToday();
  int nowMinute() const;
  int64_t reviewEpochMilliseconds();
  int drawWrapped(int fontId, int y, const char* text, int maxLines, bool bold = false,
                  int bottomY = 0, const char* rubySource = nullptr) const;
  void drawDeckScreen();
  void drawCardScreen(bool answer);

  HalFile metaFile_;
  HalFile deckFile_;
  HalFile cardFile_;
  HalFile revlogFile_;
  std::unique_ptr<StudyFileSource> metaSource_;
  std::unique_ptr<StudyFileSource> deckSource_;
  std::unique_ptr<StudyFileSource> cardSource_;

  study::StudyDeck deck_;
  study::Fsrs fsrs_;
  study::Scheduler scheduler_;
  study::Note note_;
  study::CardState card_;
  study::Outcome previews_[4];

  char deckNames_[kMaxDecks][32] = {};
  char deckDir_[64] = {};
  int deckCount_ = 0;
  int deckIndex_ = 0;

  int queue_[kMaxQueue] = {};
  int queueCount_ = 0;
  int queuePosition_ = 0;
  PendingCard pending_[kMaxPending] = {};
  int pendingCount_ = 0;
  int currentIndex_ = -1;

  int today_ = 0;
  int fallbackMinute_ = 720;
  unsigned long sessionStartedAt_ = 0;
  unsigned long timeSyncStartedAt_ = 0;
  int dueCount_ = 0;
  int newCount_ = 0;
  int reviewedCount_ = 0;
  int64_t lastReviewAtMs_ = 0;
  bool clockReady_ = false;
  bool writeFailed_ = false;
  View view_ = View::NoDeck;
};
