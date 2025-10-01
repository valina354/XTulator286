#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include "config.h"
#include "ports.h"
#include "debuglog.h"
#include "cmos.h"

static uint8_t to_bcd(int val) {
	return ((val / 10) << 4) | (val % 10);
}

void cmos_write(void* udata, uint32_t port, uint8_t value) {
	CMOS_t* cmos = (CMOS_t*)udata;
	if (port == 0x70) {
		debug_log(DEBUG_INFO, "[CMOS] Write Port 70h (Select Register): %02Xh\n", value & 0x7F);
		cmos->index = value & 0x7F;
		cmos->nmi_mask = (value >> 7) & 1;
	}
	else if (port == 0x71) {
		debug_log(DEBUG_INFO, "[CMOS] Write Port 71h (Data) to Reg %02Xh: %02Xh\n", cmos->index, value);

		cmos->ram[cmos->index] = value;

		if (cmos->index >= 0x10 && cmos->index <= 0x2D) {
			uint16_t checksum = 0;
			int i;
			for (i = 0x10; i <= 0x2D; i++) {
				checksum += cmos->ram[i];
			}
			cmos->ram[0x2E] = (checksum >> 8) & 0xFF;
			cmos->ram[0x2F] = checksum & 0xFF;
		}
	}
}

uint8_t cmos_read(void* udata, uint32_t port) {
	CMOS_t* cmos = (CMOS_t*)udata;
	time_t t = time(NULL);
	struct tm tm = *localtime(&t);
	uint8_t ret_val = 0xFF;

	if (port != 0x71) return 0xFF;

	switch (cmos->index) {
	case 0x00: ret_val = to_bcd(tm.tm_sec); break;
	case 0x02: ret_val = to_bcd(tm.tm_min); break;
	case 0x04: ret_val = to_bcd(tm.tm_hour); break;
	case 0x06: ret_val = to_bcd(tm.tm_wday + 1); break;
	case 0x07: ret_val = to_bcd(tm.tm_mday); break;
	case 0x08: ret_val = to_bcd(tm.tm_mon + 1); break;
	case 0x09: ret_val = to_bcd(tm.tm_year % 100); break;
	case 0x0A: ret_val = 0x26; break;
	case 0x0B: ret_val = 0x02; break;
	case 0x0C:
	{
		uint8_t val = cmos->ram[cmos->index];
		cmos->ram[cmos->index] = 0x00;
		ret_val = val;
		break;
	}
	case 0x0D: ret_val = 0x80; break;
	case 0x0F:
		ret_val = cmos->ram[0x0F];
		break;
	default:
		debug_log(DEBUG_INFO, "[CMOS] Unhandled Read from Reg %02Xh\n", cmos->index);
		ret_val = cmos->ram[cmos->index];
		break;
	}

	debug_log(DEBUG_INFO, "[CMOS] Read Port 71h (Data) from Reg %02Xh -> %02Xh\n", cmos->index, ret_val);
	return ret_val;
}

void cmos_init(CMOS_t* cmos) {
	debug_log(DEBUG_INFO, "[CMOS] Initializing AT CMOS/RTC\r\n");

	memset(cmos->ram, 0, sizeof(cmos->ram));

	cmos->ram[0x0A] = 0x26;
	cmos->ram[0x0B] = 0x02;
	cmos->ram[0x0D] = 0x80;

	cmos->ram[0x10] = 0x40;
	cmos->ram[0x12] = 18;
	cmos->ram[0x19] = 18;
	cmos->ram[0x1A] = 18;
	cmos->ram[0x1B] = 0;

	cmos->ram[0x14] = 0x25;

	cmos->ram[0x15] = 640 & 0xFF;
	cmos->ram[0x16] = 640 >> 8;

	uint32_t ext_mem_kb = (15 * 1024);
	cmos->ram[0x17] = ext_mem_kb & 0xFF;
	cmos->ram[0x18] = (ext_mem_kb >> 8) & 0xFF;

	cmos->ram[0x30] = ext_mem_kb & 0xFF;
	cmos->ram[0x31] = (ext_mem_kb >> 8) & 0xFF;

	uint16_t checksum = 0;
	for (int i = 0x10; i <= 0x2D; i++) {
		checksum += cmos->ram[i];
	}
	cmos->ram[0x2E] = (checksum >> 8) & 0xFF;
	cmos->ram[0x2F] = checksum & 0xFF;

	ports_cbRegister(0x70, 2, cmos_read, NULL, cmos_write, NULL, cmos);
}
