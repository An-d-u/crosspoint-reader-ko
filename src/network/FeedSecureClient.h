#pragma once

#include <NetworkClientSecure.h>

#include "FeedReadBytes.h"

// 설치된 Arduino NetworkClient::readBytes는 TLS의 일시적인 -1에도 즉시 반환한다.
// 다른 네트워크 기능에는 영향을 주지 않도록 피드 전용 클라이언트에서만 보완한다.
class FeedSecureClient final : public NetworkClientSecure {
 public:
  size_t readBytes(char* buffer, const size_t length) override {
    return readBytes(reinterpret_cast<uint8_t*>(buffer), length);
  }

  size_t readBytes(uint8_t* buffer, const size_t length) override {
    Clock clock;
    const auto result = readFeedBytes(*this, clock, buffer, length);
    readTimedOut_ = readTimedOut_ || result.timedOut;
    return result.bytes;
  }

  bool readTimedOut() const { return readTimedOut_; }

 private:
  struct Clock {
    uint32_t now() const { return millis(); }
    void wait() const { delay(2); }
  };
  bool readTimedOut_ = false;
};
