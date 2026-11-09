#include "nor_info.h"

const char *NorModelName(uint32_t raw_id, uint16_t device_id2, uint16_t device_id3, uint32_t buffer_bytes) {
	uint16_t maker = raw_id & 0xFFFF;
	if ((raw_id >> 16) != 0x227E) return 0;
	if ((device_id2 & 0xFF00) != 0x2200 || device_id3 != 0x2201) {
		if (maker == 0x0001) return "S29GL/S70GL (?)";
		if (maker == 0x0089) return "Intel/Micron (?)";
		if (maker == 0x00C2) return "Macronix (?)";
		return 0;
	}
	uint8_t density = device_id2 & 0xFF;
	if (maker == 0x0001) {
		switch (density) {
			case 0x21:
				if (buffer_bytes == 32) return "S29GL128N";
				if (buffer_bytes == 64) return "S29GL128P";
				if (buffer_bytes == 512) return "S29GL128S";
				return "S29GL128N/P/S";
			case 0x22:
				if (buffer_bytes == 32) return "S29GL256N";
				if (buffer_bytes == 64) return "S29GL256P";
				if (buffer_bytes == 512) return "S29GL256S";
				return "S29GL256N/P/S";
			case 0x23:
				if (buffer_bytes == 32) return "S29GL512N";
				if (buffer_bytes == 64) return "S29GL512P";
				if (buffer_bytes == 512) return "S29GL512S";
				return "S29GL512N/P/S";
			case 0x28:
				if (buffer_bytes == 64) return "S29GL01GP";
				if (buffer_bytes == 512) return "S29GL01GS";
				return "S29GL01GP/GS";
			case 0x48:
				return "S70GL02GS";
			default:
				return "S29GL/S70GL (?)";
		}
	}
	if (maker == 0x0089) {
		if (density == 0x22) return "JS28F/MT28EW256";
		if (density == 0x28) return "MT28EW01G (?)";
		return "Intel/Micron (?)";
	}
	if (maker == 0x00C2) {
		if (density == 0x22) return "MX29GL256E/F";
		return "Macronix (?)";
	}
	return 0;
}
