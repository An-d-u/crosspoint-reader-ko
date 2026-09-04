// 외부 서버나 기기 없이 실제 수신 함수의 동작을 컴파일 시점에 검증한다.
#include "network/FeedReadBytes.h"

struct FakeClock {
  uint32_t tick = 0;
  constexpr uint32_t now() const { return tick; }
  constexpr void wait() { tick += 2; }
};

struct Event {
  uint32_t at;
  uint8_t byte;
};

template <size_t N>
struct FakeClient {
  FakeClock& clock;
  const Event (&events)[N];
  uint32_t startedAt = clock.now();
  uint32_t timeout = 20;
  size_t cursor = 0;
  bool closeAtEnd = false;
  bool stopped = false;
  int emptyResult = -1;
  constexpr int read(uint8_t* buffer, size_t length) {
    if (!length || stopped || cursor == N || uint32_t(clock.now() - startedAt) < events[cursor].at) return emptyResult;
    buffer[0] = events[cursor++].byte;
    return 1;
  }
  constexpr bool connected() const { return !stopped && !(closeAtEnd && cursor == N); }
  constexpr uint32_t getTimeout() const { return timeout; }
  constexpr void stop() { stopped = true; }
};

constexpr bool delayedDelimiter(uint32_t start) {
  FakeClock clock{start};
  const Event events[] = {{0, '\r'}, {12, '\n'}};
  FakeClient<2> client{clock, events};
  uint8_t buffer[2] = {};
  const auto result = readFeedBytes(client, clock, buffer, 2);
  return result.bytes == 2 && !result.timedOut && buffer[0] == '\r' && buffer[1] == '\n' && !client.stopped;
}
static_assert(delayedDelimiter(0), "분할된 청크 구분자 수신 실패");
static_assert(delayedDelimiter(UINT32_MAX - 5), "시간 값 순환 시 수신 실패");

constexpr bool reproduceOldEarlyReturn() {
  FakeClock clock;
  const Event events[] = {{0, '\r'}, {12, '\n'}};
  FakeClient<2> client{clock, events};
  uint8_t buffer[2] = {};
  size_t received = 0;
  // 설치된 NetworkClient::readBytes의 음수 반환 처리와 같은 실패 조건이다.
  while (received < 2) {
    const int count = client.read(buffer + received, 2 - received);
    if (count < 0) break;
    received += count;
  }
  return received == 1 && clock.now() == 0 && client.connected();
}
static_assert(reproduceOldEarlyReturn(), "수정 전의 조기 실패가 재현되지 않음");

constexpr bool delayedBody() {
  FakeClock clock;
  const Event events[] = {{8, 'a'}, {24, 'b'}, {40, 'c'}};
  FakeClient<3> client{clock, events};
  uint8_t buffer[3] = {};
  const auto result = readFeedBytes(client, clock, buffer, 3);
  return result.bytes == 3 && !result.timedOut && buffer[0] == 'a' && buffer[1] == 'b' && buffer[2] == 'c';
}
static_assert(delayedBody(), "수신 진행 중 제한 시간이 갱신되지 않음");

constexpr bool incompleteBody(bool closeAtEnd, bool emptyReturnsZero = false) {
  FakeClock clock;
  const Event events[] = {{0, 'a'}};
  FakeClient<1> client{clock, events};
  client.closeAtEnd = closeAtEnd;
  if (emptyReturnsZero) client.emptyResult = 0;
  uint8_t buffer[2] = {};
  const auto result = readFeedBytes(client, clock, buffer, 2);
  return result.bytes == 1 && result.timedOut == !closeAtEnd &&
         clock.now() == (closeAtEnd ? 0u : 20u) && client.stopped == !closeAtEnd;
}
static_assert(incompleteBody(true), "실제 연결 종료를 완전 수신으로 오인함");
static_assert(incompleteBody(false), "무응답 연결이 제한 시간 후 종료되지 않음");
static_assert(incompleteBody(false, true), "0바이트 반환 시 무한 대기함");

constexpr bool emptyReadAndTimeout() {
  FakeClock clock;
  const Event events[] = {{40, 'a'}};
  FakeClient<1> client{clock, events};
  uint8_t buffer[1] = {};
  const auto empty = readFeedBytes(client, clock, buffer, 0);
  if (empty.bytes || empty.timedOut || clock.now()) return false;
  const auto timeout = readFeedBytes(client, clock, buffer, 1);
  return timeout.bytes == 0 && timeout.timedOut && client.stopped && clock.now() == 20;
}
static_assert(emptyReadAndTimeout(), "빈 읽기 또는 무응답 처리 실패");
