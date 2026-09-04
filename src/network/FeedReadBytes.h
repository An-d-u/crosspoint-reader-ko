#pragma once

#include <cstddef>
#include <cstdint>

struct FeedReadResult {
  size_t bytes = 0;
  bool timedOut = false;
};

// TLS의 일시적인 read() == -1은 연결 종료와 다르다. 마지막 수신 이후의 제한 시간까지 기다린다.
// 시계와 클라이언트를 주입하여 패킷 지연·분할·시간 값 순환을 실제 함수로 검증한다.
template <typename Client, typename Clock>
constexpr FeedReadResult readFeedBytes(Client& client, Clock& clock, uint8_t* buffer, const size_t length) {
  FeedReadResult result;
  uint32_t lastProgress = clock.now();
  while (result.bytes < length) {
    const int count = client.read(buffer + result.bytes, length - result.bytes);
    if (count > 0) {
      result.bytes += static_cast<size_t>(count);
      lastProgress = clock.now();
      continue;
    }
    if (!client.connected()) break;
    if (static_cast<uint32_t>(clock.now() - lastProgress) >= client.getTimeout()) {
      result.timedOut = true;
      // HTTPClient가 짧은 읽기를 반복하면서 무한 대기하지 않도록 연결도 종료한다.
      client.stop();
      break;
    }
    clock.wait();
  }
  return result;
}
