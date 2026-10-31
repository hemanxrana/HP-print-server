#include "logger.h"

#include "remote_diagnostics.h"

Logger Log;

void Logger::begin(unsigned long baud) {
  Serial.begin(baud);
}

size_t Logger::write(uint8_t c) {
  return write(&c, 1);
}

size_t Logger::write(const uint8_t *buffer, size_t size) {
  if (!buffer || !size) return 0;
  RemoteDiagnostics::appendRaw(reinterpret_cast<const char *>(buffer), size);
  return Serial.write(buffer, size);
}
