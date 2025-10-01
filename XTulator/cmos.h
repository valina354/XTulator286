#ifndef _CMOS_H_
#define _CMOS_H_

#include <stdint.h>
#include "cpu/cpu.h"

typedef struct {
	uint8_t ram[128];
	uint8_t index;
	uint8_t nmi_mask;
} CMOS_t;

void cmos_init(CMOS_t* cmos);
uint8_t cmos_read(void* udata, uint32_t port);
void cmos_write(void* udata, uint32_t port, uint8_t value);

#endif