// Minimal stand-ins so DivaState/NetProtocol build on a PC for tests.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
typedef std::recursive_mutex portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED {}
#define portENTER_CRITICAL(m) (m)->lock()
#define portEXIT_CRITICAL(m) (m)->unlock()
typedef void *TaskHandle_t;
inline void xTaskNotifyGive(TaskHandle_t) {}
struct HostSerial {
  template <typename... A> void printf(const char *f, A... a) { std::fprintf(stderr, f, a...); }
};
inline HostSerial Serial;
