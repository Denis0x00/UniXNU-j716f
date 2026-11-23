// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef J716F_R41_ROOT_WINDOW_DEVICE_H
#define J716F_R41_ROOT_WINDOW_DEVICE_H

#include <stddef.h>
#include <stdint.h>

class IOService;
namespace j716f_r41 {
class Transport;
}

extern "C" {
size_t j716f_r41_base_class_size(void);
IOService *j716f_r41_create_window_device(j716f_r41::Transport *transport,
                                          unsigned window_id,
                                          const char *label,
                                          uint64_t abs_offset_bytes,
                                          uint64_t window_bytes);
void j716f_r41_window_stats(unsigned window_id,
                            uint64_t *reads, uint64_t *bytes_read,
                            uint64_t *writes_refused, uint64_t *errors);
}

#endif
