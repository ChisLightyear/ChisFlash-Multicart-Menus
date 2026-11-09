#ifndef NOR_INFO_H_
#define NOR_INFO_H_

#include <stdint.h>

const char *NorModelName(uint32_t raw_id, uint16_t device_id2, uint16_t device_id3, uint32_t buffer_bytes);

#endif
