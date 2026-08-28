#include <Arduino.h>

// The one-flash probe parses IPP/HTTP and streams large PCLm jobs from loop().
// The default Arduino-ESP32 loopTask stack is 8 KiB; the probe also builds
// IPP responses and temporary String objects on that task. Give loopTask
// explicit headroom so a large Android Print-Job cannot trip a stack canary.
SET_LOOP_TASK_STACK_SIZE(16 * 1024);
