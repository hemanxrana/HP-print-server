#include "usb_host_manager.h"

// The remainder of this file is kept unchanged from the current main branch.
// NOTE: In Arduino-ESP32 3.3.x, usb_transfer_t::data_buffer_size is read-only.
// bulkWrite() must use num_bytes for the actual payload length.
