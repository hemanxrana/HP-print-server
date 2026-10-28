#include "remote_diagnostics.h"

#include <stdarg.h>

namespace {
constexpr size_t LOG_RING_SIZE = 2560;

char logRing[LOG_RING_SIZE] = {};
size_t logLength = 0;

void appendLog(const String &line) {
  const size_t capacity = sizeof(logRing) - 1;
  size_t length = line.length();
  const char *source = line.c_str();
  if (length > capacity) {
    source += (length - capacity);
    length = capacity;
  }
  if (logLength + length > capacity) {
    const size_t drop = logLength + length - capacity;
    memmove(logRing, logRing + drop, logLength - drop);
    logLength -= drop;
  }
  memcpy(logRing + logLength, source, length);
  logLength += length;
  logRing[logLength] = '\0';
}
} // namespace

namespace RemoteDiagnostics {
void begin() {
  Serial.println("[LOG] RAM log ring ready; read it at /log");
}

void publish(const String &message) {
  appendLog(message);
}

void publishf(const char *format, ...) {
  char buffer[256];
  va_list args;
  va_start(args, format);
  vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  publish(String(buffer));
}

String recentLog() {
  return String(logRing);
}
} // namespace RemoteDiagnostics
