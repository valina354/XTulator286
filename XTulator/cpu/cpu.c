/*
  XTulator: A portable, open-source 80186 PC emulator.
  Copyright (C)2020 Mike Chambers

  This program is free software; you can redistribute it and/or
  modify it under the terms of the GNU General Public License
  as published by the Free Software Foundation; either version 2
  of the License, or (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include "cpu.h"
#include "fpu.h"
#include "../chipset/i8042.h" 
#include "../config.h"
#include "../debuglog.h"

const uint8_t byteregtable[8] = { regal, regcl, regdl, regbl, regah, regch, regdh, regbh };

const uint8_t parity[0x100] = {
	1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0,
	0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1,
	0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1,
	1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0,
	0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1,
	1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0,
	1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0,
	0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1
};

void load_tr(CPU_t* cpu, uint16_t selector) {
	if ((selector & 0xFFFC) == 0) {
		debug_log(DEBUG_ERROR, "[CPU] LTR: GPF(#0) - NULL selector.\n");
		cpu_intcall(cpu, 0);
		return;
	}

	uint32_t table_base;
	uint16_t table_limit;

	if (selector & 0x0004) {
		if (!cpu->ldtr_cache.valid) {
			debug_log(DEBUG_ERROR, "[CPU] LTR: GPF(#13) - LDTR not valid.\n");
			cpu_intcall(cpu, 13);
			return;
		}
		table_base = cpu->ldtr_cache.base;
		table_limit = cpu->ldtr_cache.limit;
	}
	else {
		table_base = cpu->gdtr.base;
		table_limit = cpu->gdtr.limit;
	}

	uint16_t index = selector >> 3;
	if ((index * 8) + 7 > table_limit) {
		debug_log(DEBUG_ERROR, "[CPU] LTR: GPF(#13) - Selector exceeds table limit.\n");
		cpu_intcall(cpu, 13);
		return;
	}

	uint32_t addr = table_base + index * 8;
	uint8_t access = cpu_read(cpu, addr + 5);
	uint8_t type = access & 0x0F;

	if (type != 0x01 && type != 0x03) {
		debug_log(DEBUG_ERROR, "[CPU] LTR: GPF(#13) - Invalid 286 TSS descriptor type. Type: 0x%02X, Access byte: 0x%02X\n", type, access);
		cpu_intcall(cpu, 13);
		return;
	}

	if (!(access & 0x80)) {
		debug_log(DEBUG_ERROR, "[CPU] LTR: NPF(#11) - TSS descriptor not present. Access byte: 0x%02X\n", access);
		cpu_intcall(cpu, 11);
		return;
	}

	cpu->tr_cache.limit = cpu_readw(cpu, addr);
	cpu->tr_cache.base = cpu_read(cpu, addr + 2) | (cpu_read(cpu, addr + 3) << 8) | (cpu_read(cpu, addr + 4) << 16);
	cpu->tr_cache.access = access | 0x02;
	cpu->tr_cache.valid = 1;
	cpu->tr = selector;
	cpu->tr_cache.sp0 = cpu_readw(cpu, cpu->tr_cache.base + 2);
	cpu->tr_cache.ss0 = cpu_readw(cpu, cpu->tr_cache.base + 4);

	cpu_write(cpu, addr + 5, access | 0x02);
}

void load_ldtr(CPU_t* cpu, uint16_t selector) {
	if ((selector & 0xFFFC) == 0) {
		cpu->ldtr_cache.valid = 0;
		return;
	}

	uint16_t cpl = cpu->segregs[regcs] & 3;
	if (cpl != 0) {
		debug_log(DEBUG_ERROR, "[CPU] LLDT: GPF(#0) - CPL != 0\n");
		cpu_intcall(cpu, 0);
		return;
	}

	if ((selector & 0xFFFC) > cpu->gdtr.limit) {
		debug_log(DEBUG_ERROR, "[CPU] LLDT: GPF(#13) - Selector 0x%04X exceeds GDT limit.\n", selector);
		cpu_intcall(cpu, 13);
		return;
	}

	uint16_t index = selector >> 3;
	uint32_t addr = cpu->gdtr.base + index * 8;
	uint8_t access = cpu_read(cpu, addr + 5);

	if ((access & 0x1F) != 0x02) {
		debug_log(DEBUG_ERROR, "[CPU] LLDT: GPF(#13) - Not an LDT descriptor. Access byte: 0x%02X\n", access);
		cpu_intcall(cpu, 13);
		return;
	}

	if (!(access & 0x80)) {
		debug_log(DEBUG_ERROR, "[CPU] LLDT: NPF(#11) - LDT descriptor not present.\n");
		cpu_intcall(cpu, 11);
		return;
	}

	cpu->ldtr_cache.limit = cpu_readw(cpu, addr);
	cpu->ldtr_cache.base = cpu_read(cpu, addr + 2) | (cpu_read(cpu, addr + 3) << 8) | (cpu_read(cpu, addr + 4) << 16);
	cpu->ldtr_cache.access = access;
	cpu->ldtr_cache.valid = 1;
}

void load_descriptor(CPU_t* cpu, uint8_t seg_reg, uint16_t selector) {
	DESCRIPTOR_CACHE* cache = &cpu->segcache[seg_reg];
	uint16_t cpl = cpu->segregs[regcs] & 3;

	if ((selector & 0xFFFC) == 0) {
		if (seg_reg == regss) {
			debug_log(DEBUG_ERROR, "[CPU] GPF(#13): Attempted to load SS with a null selector.\n");
			cpu_intcall(cpu, 13);
			return;
		}

		cache->valid = 0;
		cpu->segregs[seg_reg] = selector;
		return;
	}

	uint32_t table_base;
	uint16_t table_limit;

	if (selector & 0x0004) {
		if (!cpu->ldtr_cache.valid) {
			debug_log(DEBUG_ERROR, "[CPU] GPF(#13): LDT is not valid but selector %04X references it.\n", selector);
			cpu_intcall(cpu, 13);
			return;
		}
		table_base = cpu->ldtr_cache.base;
		table_limit = cpu->ldtr_cache.limit;
	}
	else {
		table_base = cpu->gdtr.base;
		table_limit = cpu->gdtr.limit;
	}

	uint16_t index = selector >> 3;
	if ((index * 8) + 7 > table_limit) {
		debug_log(DEBUG_ERROR, "[CPU] GPF(#13): Selector %04X exceeds table limit %04X\n", selector, table_limit);
		cpu_intcall(cpu, 13);
		cache->valid = 0;
		return;
	}

	uint32_t addr = table_base + index * 8;
	uint8_t  access = cpu_read(cpu, addr + 5);
	uint16_t limit = cpu_readw(cpu, addr);
	uint32_t base = cpu_read(cpu, addr + 2) | (cpu_read(cpu, addr + 3) << 8) | (cpu_read(cpu, addr + 4) << 16);

	uint16_t rpl = selector & 3;
	uint8_t  dpl = (access >> 5) & 3;

	if (!(access & 0x80)) {
		debug_log(DEBUG_ERROR, "[CPU] NPF(#11): Segment %04X not present. Access byte: 0x%02X\n", selector, access);
		cpu_intcall(cpu, 11);
		return;
	}

	if (seg_reg == regss) {
		bool is_writable_data = !(access & 0x08) && (access & 0x02);
		if (rpl != cpl || dpl != cpl || !is_writable_data) {
			debug_log(DEBUG_ERROR, "[CPU] GPF(#13): Invalid SS selector %04X. CPL=%d, RPL=%d, DPL=%d, Access=0x%02X\n", selector, cpl, rpl, dpl, access);
			push(cpu, selector);
			cpu_intcall(cpu, 13);
			return;
		}
	}
	else if (seg_reg == regcs) {
		if (!(access & 0x08)) {
			debug_log(DEBUG_ERROR, "[CPU] GPF(#13): Attempted to load CS with a non-code segment selector %04X.\n", selector);
			cpu_intcall(cpu, 13);
			return;
		}
		if (dpl > cpl) {
			debug_log(DEBUG_ERROR, "[CPU] GPF(#13): Cannot load CS with selector %04X due to privilege mismatch (DPL > CPL).\n", selector);
			cpu_intcall(cpu, 13);
			return;
		}
	}
	else {
		bool is_data = !(access & 0x08);
		bool is_readable_code = (access & 0x0A) == 0x0A;
		if (!is_data && !is_readable_code) {
			debug_log(DEBUG_ERROR, "[CPU] GPF(#13): Attempted to load DS/ES with invalid segment type %04X.\n", selector);
			cpu_intcall(cpu, 13);
			return;
		}
		if ((cpl > dpl) || (rpl > dpl)) {
			debug_log(DEBUG_ERROR, "[CPU] GPF(#13): Privilege violation loading DS/ES with selector %04X. CPL=%d, RPL=%d, DPL=%d\n", selector, cpl, rpl, dpl);
			cpu_intcall(cpu, 13);
			return;
		}
	}

	cache->limit = limit;
	cache->base = base;
	cache->access = access;
	cache->valid = 1;
	cpu->segregs[seg_reg] = selector;
}

FUNC_INLINE void cpu_writew(CPU_t* cpu, uint32_t addr32, uint16_t value) {
	cpu_write(cpu, addr32, (uint8_t)value);
	cpu_write(cpu, addr32 + 1, (uint8_t)(value >> 8));
}

FUNC_INLINE uint16_t cpu_readw(CPU_t* cpu, uint32_t addr32) {
	return ((uint16_t)cpu_read(cpu, addr32) | (uint16_t)(cpu_read(cpu, addr32 + 1) << 8));
}

int get_descriptor_info(CPU_t* cpu, uint16_t selector, uint32_t* base, uint16_t* limit, uint8_t* access) {
	uint32_t table_base;
	uint16_t table_limit;

	if ((selector & 0xFFFC) == 0) {
		return 0;
	}

	if (selector & 0x0004) {
		if (!cpu->ldtr_cache.valid) {
			return 0;
		}
		table_base = cpu->ldtr_cache.base;
		table_limit = cpu->ldtr_cache.limit;
	}
	else {
		table_base = cpu->gdtr.base;
		table_limit = cpu->gdtr.limit;
	}

	uint16_t index = selector >> 3;
	if ((index * 8) + 7 > table_limit) {
		return 0;
	}

	uint32_t addr = table_base + (index * 8);
	*limit = cpu_readw(cpu, addr);
	*base = cpu_read(cpu, addr + 2) | (cpu_read(cpu, addr + 3) << 8) | (cpu_read(cpu, addr + 4) << 16);
	*access = cpu_read(cpu, addr + 5);

	debug_log(DEBUG_INFO, "[CPU] get_descriptor_info(sel=%04X): Found at %08X -> base=%06X, limit=%04X, access=%02X\n",
		selector, addr, *base, *limit, *access);

	return 1;
}

FUNC_INLINE void flag_szp8(CPU_t* cpu, uint8_t value) {
	if (!value) {
		cpu->zf = 1;
	}
	else {
		cpu->zf = 0;	/* set or clear zero flag */
	}

	if (value & 0x80) {
		cpu->sf = 1;
	}
	else {
		cpu->sf = 0;	/* set or clear sign flag */
	}

	cpu->pf = parity[value]; /* retrieve parity state from lookup table */
}

FUNC_INLINE void flag_szp16(CPU_t* cpu, uint16_t value) {
	if (!value) {
		cpu->zf = 1;
	}
	else {
		cpu->zf = 0;	/* set or clear zero flag */
	}

	if (value & 0x8000) {
		cpu->sf = 1;
	}
	else {
		cpu->sf = 0;	/* set or clear sign flag */
	}

	cpu->pf = parity[value & 255];	/* retrieve parity state from lookup table */
}

FUNC_INLINE void flag_log8(CPU_t* cpu, uint8_t value) {
	flag_szp8(cpu, value);
	cpu->cf = 0;
	cpu->of = 0; /* bitwise logic ops always clear carry and overflow */
}

FUNC_INLINE void flag_log16(CPU_t* cpu, uint16_t value) {
	flag_szp16(cpu, value);
	cpu->cf = 0;
	cpu->of = 0; /* bitwise logic ops always clear carry and overflow */
}

FUNC_INLINE void flag_adc8(CPU_t* cpu, uint8_t v1, uint8_t v2, uint8_t v3) {

	/* v1 = destination operand, v2 = source operand, v3 = carry flag */
	uint16_t	dst;

	dst = (uint16_t)v1 + (uint16_t)v2 + (uint16_t)v3;
	flag_szp8(cpu, (uint8_t)dst);
	if (((dst ^ v1) & (dst ^ v2) & 0x80) == 0x80) {
		cpu->of = 1;
	}
	else {
		cpu->of = 0; /* set or clear overflow flag */
	}

	if (dst & 0xFF00) {
		cpu->cf = 1;
	}
	else {
		cpu->cf = 0; /* set or clear carry flag */
	}

	if (((v1 ^ v2 ^ dst) & 0x10) == 0x10) {
		cpu->af = 1;
	}
	else {
		cpu->af = 0; /* set or clear auxilliary flag */
	}
}

FUNC_INLINE void flag_adc16(CPU_t* cpu, uint16_t v1, uint16_t v2, uint16_t v3) {

	uint32_t	dst;

	dst = (uint32_t)v1 + (uint32_t)v2 + (uint32_t)v3;
	flag_szp16(cpu, (uint16_t)dst);
	if ((((dst ^ v1) & (dst ^ v2)) & 0x8000) == 0x8000) {
		cpu->of = 1;
	}
	else {
		cpu->of = 0;
	}

	if (dst & 0xFFFF0000) {
		cpu->cf = 1;
	}
	else {
		cpu->cf = 0;
	}

	if (((v1 ^ v2 ^ dst) & 0x10) == 0x10) {
		cpu->af = 1;
	}
	else {
		cpu->af = 0;
	}
}

FUNC_INLINE void flag_add8(CPU_t* cpu, uint8_t v1, uint8_t v2) {
	/* v1 = destination operand, v2 = source operand */
	uint16_t	dst;

	dst = (uint16_t)v1 + (uint16_t)v2;
	flag_szp8(cpu, (uint8_t)dst);
	if (dst & 0xFF00) {
		cpu->cf = 1;
	}
	else {
		cpu->cf = 0;
	}

	if (((dst ^ v1) & (dst ^ v2) & 0x80) == 0x80) {
		cpu->of = 1;
	}
	else {
		cpu->of = 0;
	}

	if (((v1 ^ v2 ^ dst) & 0x10) == 0x10) {
		cpu->af = 1;
	}
	else {
		cpu->af = 0;
	}
}

FUNC_INLINE void flag_add16(CPU_t* cpu, uint16_t v1, uint16_t v2) {
	/* v1 = destination operand, v2 = source operand */
	uint32_t	dst;

	dst = (uint32_t)v1 + (uint32_t)v2;
	flag_szp16(cpu, (uint16_t)dst);
	if (dst & 0xFFFF0000) {
		cpu->cf = 1;
	}
	else {
		cpu->cf = 0;
	}

	if (((dst ^ v1) & (dst ^ v2) & 0x8000) == 0x8000) {
		cpu->of = 1;
	}
	else {
		cpu->of = 0;
	}

	if (((v1 ^ v2 ^ dst) & 0x10) == 0x10) {
		cpu->af = 1;
	}
	else {
		cpu->af = 0;
	}
}

FUNC_INLINE void flag_sbb8(CPU_t* cpu, uint8_t v1, uint8_t v2, uint8_t v3) {

	/* v1 = destination operand, v2 = source operand, v3 = carry flag */
	uint16_t	dst;

	v2 += v3;
	dst = (uint16_t)v1 - (uint16_t)v2;
	flag_szp8(cpu, (uint8_t)dst);
	if (dst & 0xFF00) {
		cpu->cf = 1;
	}
	else {
		cpu->cf = 0;
	}

	if ((dst ^ v1) & (v1 ^ v2) & 0x80) {
		cpu->of = 1;
	}
	else {
		cpu->of = 0;
	}

	if ((v1 ^ v2 ^ dst) & 0x10) {
		cpu->af = 1;
	}
	else {
		cpu->af = 0;
	}
}

FUNC_INLINE void flag_sbb16(CPU_t* cpu, uint16_t v1, uint16_t v2, uint16_t v3) {

	/* v1 = destination operand, v2 = source operand, v3 = carry flag */
	uint32_t	dst;

	v2 += v3;
	dst = (uint32_t)v1 - (uint32_t)v2;
	flag_szp16(cpu, (uint16_t)dst);
	if (dst & 0xFFFF0000) {
		cpu->cf = 1;
	}
	else {
		cpu->cf = 0;
	}

	if ((dst ^ v1) & (v1 ^ v2) & 0x8000) {
		cpu->of = 1;
	}
	else {
		cpu->of = 0;
	}

	if ((v1 ^ v2 ^ dst) & 0x10) {
		cpu->af = 1;
	}
	else {
		cpu->af = 0;
	}
}

FUNC_INLINE void flag_sub8(CPU_t* cpu, uint8_t v1, uint8_t v2) {

	/* v1 = destination operand, v2 = source operand */
	uint16_t	dst;

	dst = (uint16_t)v1 - (uint16_t)v2;
	flag_szp8(cpu, (uint8_t)dst);
	if (dst & 0xFF00) {
		cpu->cf = 1;
	}
	else {
		cpu->cf = 0;
	}

	if ((dst ^ v1) & (v1 ^ v2) & 0x80) {
		cpu->of = 1;
	}
	else {
		cpu->of = 0;
	}

	if ((v1 ^ v2 ^ dst) & 0x10) {
		cpu->af = 1;
	}
	else {
		cpu->af = 0;
	}
}

FUNC_INLINE void flag_sub16(CPU_t* cpu, uint16_t v1, uint16_t v2) {

	/* v1 = destination operand, v2 = source operand */
	uint32_t	dst;

	dst = (uint32_t)v1 - (uint32_t)v2;
	flag_szp16(cpu, (uint16_t)dst);
	if (dst & 0xFFFF0000) {
		cpu->cf = 1;
	}
	else {
		cpu->cf = 0;
	}

	if ((dst ^ v1) & (v1 ^ v2) & 0x8000) {
		cpu->of = 1;
	}
	else {
		cpu->of = 0;
	}

	if ((v1 ^ v2 ^ dst) & 0x10) {
		cpu->af = 1;
	}
	else {
		cpu->af = 0;
	}
}

FUNC_INLINE void op_adc8(CPU_t* cpu) {
	cpu->res8 = cpu->oper1b + cpu->oper2b + cpu->cf;
	flag_adc8(cpu, cpu->oper1b, cpu->oper2b, cpu->cf);
}

FUNC_INLINE void op_adc16(CPU_t* cpu) {
	cpu->res16 = cpu->oper1 + cpu->oper2 + cpu->cf;
	flag_adc16(cpu, cpu->oper1, cpu->oper2, cpu->cf);
}

FUNC_INLINE void op_add8(CPU_t* cpu) {
	cpu->res8 = cpu->oper1b + cpu->oper2b;
	flag_add8(cpu, cpu->oper1b, cpu->oper2b);
}

FUNC_INLINE void op_add16(CPU_t* cpu) {
	cpu->res16 = cpu->oper1 + cpu->oper2;
	flag_add16(cpu, cpu->oper1, cpu->oper2);
}

FUNC_INLINE void op_and8(CPU_t* cpu) {
	cpu->res8 = cpu->oper1b & cpu->oper2b;
	flag_log8(cpu, cpu->res8);
}

FUNC_INLINE void op_and16(CPU_t* cpu) {
	cpu->res16 = cpu->oper1 & cpu->oper2;
	flag_log16(cpu, cpu->res16);
}

FUNC_INLINE void op_or8(CPU_t* cpu) {
	cpu->res8 = cpu->oper1b | cpu->oper2b;
	flag_log8(cpu, cpu->res8);
}

FUNC_INLINE void op_or16(CPU_t* cpu) {
	cpu->res16 = cpu->oper1 | cpu->oper2;
	flag_log16(cpu, cpu->res16);
}

FUNC_INLINE void op_xor8(CPU_t* cpu) {
	cpu->res8 = cpu->oper1b ^ cpu->oper2b;
	flag_log8(cpu, cpu->res8);
}

FUNC_INLINE void op_xor16(CPU_t* cpu) {
	cpu->res16 = cpu->oper1 ^ cpu->oper2;
	flag_log16(cpu, cpu->res16);
}

FUNC_INLINE void op_sub8(CPU_t* cpu) {
	cpu->res8 = cpu->oper1b - cpu->oper2b;
	flag_sub8(cpu, cpu->oper1b, cpu->oper2b);
}

FUNC_INLINE void op_sub16(CPU_t* cpu) {
	cpu->res16 = cpu->oper1 - cpu->oper2;
	flag_sub16(cpu, cpu->oper1, cpu->oper2);
}

FUNC_INLINE void op_sbb8(CPU_t* cpu) {
	cpu->res8 = cpu->oper1b - (cpu->oper2b + cpu->cf);
	flag_sbb8(cpu, cpu->oper1b, cpu->oper2b, cpu->cf);
}

FUNC_INLINE void op_sbb16(CPU_t* cpu) {
	cpu->res16 = cpu->oper1 - (cpu->oper2 + cpu->cf);
	flag_sbb16(cpu, cpu->oper1, cpu->oper2, cpu->cf);
}

FUNC_INLINE void getea(CPU_t* cpu, uint8_t rmval) {
	uint32_t	tempea;

	tempea = 0;
	switch (cpu->mode) {
	case 0:
		switch (rmval) {
		case 0: tempea = cpu->regs.wordregs[regbx] + cpu->regs.wordregs[regsi]; break;
		case 1: tempea = cpu->regs.wordregs[regbx] + cpu->regs.wordregs[regdi]; break;
		case 2: tempea = cpu->regs.wordregs[regbp] + cpu->regs.wordregs[regsi]; break;
		case 3: tempea = cpu->regs.wordregs[regbp] + cpu->regs.wordregs[regdi]; break;
		case 4: tempea = cpu->regs.wordregs[regsi]; break;
		case 5: tempea = cpu->regs.wordregs[regdi]; break;
		case 6: tempea = cpu->disp16; break;
		case 7: tempea = cpu->regs.wordregs[regbx]; break;
		}
		break;
	case 1:
	case 2:
		switch (rmval) {
		case 0: tempea = cpu->regs.wordregs[regbx] + cpu->regs.wordregs[regsi] + cpu->disp16; break;
		case 1: tempea = cpu->regs.wordregs[regbx] + cpu->regs.wordregs[regdi] + cpu->disp16; break;
		case 2: tempea = cpu->regs.wordregs[regbp] + cpu->regs.wordregs[regsi] + cpu->disp16; break;
		case 3: tempea = cpu->regs.wordregs[regbp] + cpu->regs.wordregs[regdi] + cpu->disp16; break;
		case 4: tempea = cpu->regs.wordregs[regsi] + cpu->disp16; break;
		case 5: tempea = cpu->regs.wordregs[regdi] + cpu->disp16; break;
		case 6: tempea = cpu->regs.wordregs[regbp] + cpu->disp16; break;
		case 7: tempea = cpu->regs.wordregs[regbx] + cpu->disp16; break;
		}
		break;
	}

	uint16_t offset = tempea & 0xFFFF;

	if (cpu->protected_mode) {
		int seg_reg_index = -1;

		if (cpu->useseg == cpu->segregs[regss]) seg_reg_index = regss;
		else if (cpu->useseg == cpu->segregs[regds]) seg_reg_index = regds;
		else if (cpu->useseg == cpu->segregs[reges]) seg_reg_index = reges;
		else if (cpu->useseg == cpu->segregs[regcs]) seg_reg_index = regcs;

		if (seg_reg_index != -1 && cpu->segcache[seg_reg_index].valid) {
			cpu->ea = cpu->segcache[seg_reg_index].base + offset;
		}
		else {
			cpu->ea = 0;
		}
	}
	else {
		uint32_t addr = (cpu->useseg << 4) + offset;
		if (!a20_enabled) {
			addr &= 0x000FFFFF;
		}
		cpu->ea = addr;
	}
}

FUNC_INLINE void push(CPU_t* cpu, uint16_t pushval) {
	cpu->regs.wordregs[regsp] = cpu->regs.wordregs[regsp] - 2;
	putmem16(cpu, cpu->segregs[regss], cpu->regs.wordregs[regsp], pushval);
}

FUNC_INLINE uint16_t pop(CPU_t* cpu) {

	uint16_t	tempval;

	tempval = getmem16(cpu, cpu->segregs[regss], cpu->regs.wordregs[regsp]);
	cpu->regs.wordregs[regsp] = cpu->regs.wordregs[regsp] + 2;
	return tempval;
}

uint32_t translate_address_safe(CPU_t* cpu, uint16_t seg, uint16_t off, int* fault) {
	if (!cpu->protected_mode) {
		if (fault) *fault = 0;
		return (uint32_t)((seg << 4) + off);
	}

	DESCRIPTOR_CACHE* cache = NULL;
	if (seg == cpu->segregs[regcs]) cache = &cpu->segcache[regcs];
	else if (seg == cpu->segregs[regds]) cache = &cpu->segcache[regds];
	else if (seg == cpu->segregs[reges]) cache = &cpu->segcache[reges];
	else if (seg == cpu->segregs[regss]) cache = &cpu->segcache[regss];

	if (!cache || !cache->valid) {
		if (fault) *fault = 1;
		return 0xFFFFFFFF;
	}

	if (off > cache->limit) {
		if (fault) *fault = 1;
		return 0xFFFFFFFF;
	}

	if (fault) *fault = 0;
	return cache->base + off;
}

uint32_t get_real_address(CPU_t* cpu, uint16_t seg, uint16_t off) {
	if (cpu->protected_mode) {
		int fault = 0;
		uint32_t addr = translate_address_safe(cpu, seg, off, &fault);
		if (fault) {
			cpu_intcall(cpu, 13);
			return 0;
		}
		return addr;
	}
	else {
		uint32_t addr = (uint32_t)((seg << 4) + off);

		if (!a20_enabled) {
			return addr & 0x000FFFFF;
		}

		return addr;
	}
}

void cpu_reset(CPU_t* cpu) {
	uint16_t i;
	for (i = 0; i < 256; i++) {
		cpu->int_callback[i] = NULL;
	}
	memset(&cpu->regs, 0, sizeof(cpu->regs));
	memset(cpu->segcache, 0, sizeof(cpu->segcache));
	memset(&cpu->ldtr_cache, 0, sizeof(cpu->ldtr_cache));
	memset(&cpu->tr_cache, 0, sizeof(cpu->tr_cache));
	cpu->msw = 0xFFF0;
	cpu->gdtr.base = 0;
	cpu->gdtr.limit = 0xFFFF;
	cpu->idtr.base = 0;
	cpu->idtr.limit = 0x03FF;
	cpu->handling_fault = 0;
	cpu->ldtr = 0;
	cpu->tr = 0;
	cpu->protected_mode = 0;
	a20_enabled = 0;
	OpFinit(cpu);
	cpu->segregs[regcs] = 0xF000;
	cpu->ip = 0xFFF0;
	cpu->hltstate = 0;
	cpu->trap_toggle = 0;
}

FUNC_INLINE uint16_t readrm16(CPU_t* cpu, uint8_t rmval) {
	if (cpu->mode < 3) {
		getea(cpu, rmval);
		return cpu_read(cpu, cpu->ea) | ((uint16_t)cpu_read(cpu, cpu->ea + 1) << 8);
	}
	else {
		return getreg16(cpu, rmval);
	}
}

FUNC_INLINE uint8_t readrm8(CPU_t* cpu, uint8_t rmval) {
	if (cpu->mode < 3) {
		getea(cpu, rmval);
		return cpu_read(cpu, cpu->ea);
	}
	else {
		return getreg8(cpu, rmval);
	}
}

FUNC_INLINE void writerm16(CPU_t* cpu, uint8_t rmval, uint16_t value) {
	if (cpu->mode < 3) {
		getea(cpu, rmval);
		cpu_write(cpu, cpu->ea, value & 0xFF);
		cpu_write(cpu, cpu->ea + 1, value >> 8);
	}
	else {
		putreg16(cpu, rmval, value);
	}
}

FUNC_INLINE void writerm8(CPU_t* cpu, uint8_t rmval, uint8_t value) {
	if (cpu->mode < 3) {
		getea(cpu, rmval);
		cpu_write(cpu, cpu->ea, value);
	}
	else {
		putreg8(cpu, rmval, value);
	}
}

FUNC_INLINE uint8_t op_grp2_8(CPU_t* cpu, uint8_t cnt) {

	uint16_t	s;
	uint16_t	shift;
	uint16_t	oldcf;
	uint16_t	msb;

	s = cpu->oper1b;
	oldcf = cpu->cf;
	cnt &= 0x1F;
	switch (cpu->reg) {
	case 0: /* ROL r/m8 */
		for (shift = 1; shift <= cnt; shift++) {
			if (s & 0x80) {
				cpu->cf = 1;
			}
			else {
				cpu->cf = 0;
			}

			s = s << 1;
			s = s | cpu->cf;
		}

		if (cnt == 1) {
			//cpu->of = cpu->cf ^ ( (s >> 7) & 1);
			if ((s & 0x80) && cpu->cf) cpu->of = 1; else cpu->of = 0;
		}
		else cpu->of = 0;
		break;

	case 1: /* ROR r/m8 */
		for (shift = 1; shift <= cnt; shift++) {
			cpu->cf = s & 1;
			s = (s >> 1) | (cpu->cf << 7);
		}

		if (cnt == 1) {
			cpu->of = (s >> 7) ^ ((s >> 6) & 1);
		}
		break;

	case 2: /* RCL r/m8 */
		for (shift = 1; shift <= cnt; shift++) {
			oldcf = cpu->cf;
			if (s & 0x80) {
				cpu->cf = 1;
			}
			else {
				cpu->cf = 0;
			}

			s = s << 1;
			s = s | oldcf;
		}

		if (cnt == 1) {
			cpu->of = cpu->cf ^ ((s >> 7) & 1);
		}
		break;

	case 3: /* RCR r/m8 */
		for (shift = 1; shift <= cnt; shift++) {
			oldcf = cpu->cf;
			cpu->cf = s & 1;
			s = (s >> 1) | (oldcf << 7);
		}

		if (cnt == 1) {
			cpu->of = (s >> 7) ^ ((s >> 6) & 1);
		}
		break;

	case 4:
	case 6: /* SHL r/m8 */
		for (shift = 1; shift <= cnt; shift++) {
			if (s & 0x80) {
				cpu->cf = 1;
			}
			else {
				cpu->cf = 0;
			}

			s = (s << 1) & 0xFF;
		}

		if ((cnt == 1) && (cpu->cf == (s >> 7))) {
			cpu->of = 0;
		}
		else {
			cpu->of = 1;
		}

		flag_szp8(cpu, (uint8_t)s);
		break;

	case 5: /* SHR r/m8 */
		if ((cnt == 1) && (s & 0x80)) {
			cpu->of = 1;
		}
		else {
			cpu->of = 0;
		}

		for (shift = 1; shift <= cnt; shift++) {
			cpu->cf = s & 1;
			s = s >> 1;
		}

		flag_szp8(cpu, (uint8_t)s);
		break;

	case 7: /* SAR r/m8 */
		for (shift = 1; shift <= cnt; shift++) {
			msb = s & 0x80;
			cpu->cf = s & 1;
			s = (s >> 1) | msb;
		}

		cpu->of = 0;
		flag_szp8(cpu, (uint8_t)s);
		break;
	}

	return s & 0xFF;
}

FUNC_INLINE uint16_t op_grp2_16(CPU_t* cpu, uint8_t cnt) {

	uint32_t	s;
	uint32_t	shift;
	uint32_t	oldcf;
	uint32_t	msb;

	s = cpu->oper1;
	oldcf = cpu->cf;
	cnt &= 0x1F;
	switch (cpu->reg) {
	case 0: /* ROL r/m8 */
		for (shift = 1; shift <= cnt; shift++) {
			if (s & 0x8000) {
				cpu->cf = 1;
			}
			else {
				cpu->cf = 0;
			}

			s = s << 1;
			s = s | cpu->cf;
		}

		if (cnt == 1) {
			cpu->of = cpu->cf ^ ((s >> 15) & 1);
		}
		break;

	case 1: /* ROR r/m8 */
		for (shift = 1; shift <= cnt; shift++) {
			cpu->cf = s & 1;
			s = (s >> 1) | (cpu->cf << 15);
		}

		if (cnt == 1) {
			cpu->of = (s >> 15) ^ ((s >> 14) & 1);
		}
		break;

	case 2: /* RCL r/m8 */
		for (shift = 1; shift <= cnt; shift++) {
			oldcf = cpu->cf;
			if (s & 0x8000) {
				cpu->cf = 1;
			}
			else {
				cpu->cf = 0;
			}

			s = s << 1;
			s = s | oldcf;
		}

		if (cnt == 1) {
			cpu->of = cpu->cf ^ ((s >> 15) & 1);
		}
		break;

	case 3: /* RCR r/m8 */
		for (shift = 1; shift <= cnt; shift++) {
			oldcf = cpu->cf;
			cpu->cf = s & 1;
			s = (s >> 1) | (oldcf << 15);
		}

		if (cnt == 1) {
			cpu->of = (s >> 15) ^ ((s >> 14) & 1);
		}
		break;

	case 4:
	case 6: /* SHL r/m8 */
		for (shift = 1; shift <= cnt; shift++) {
			if (s & 0x8000) {
				cpu->cf = 1;
			}
			else {
				cpu->cf = 0;
			}

			s = (s << 1) & 0xFFFF;
		}

		if ((cnt == 1) && (cpu->cf == (s >> 15))) {
			cpu->of = 0;
		}
		else {
			cpu->of = 1;
		}

		flag_szp16(cpu, (uint16_t)s);
		break;

	case 5: /* SHR r/m8 */
		if ((cnt == 1) && (s & 0x8000)) {
			cpu->of = 1;
		}
		else {
			cpu->of = 0;
		}

		for (shift = 1; shift <= cnt; shift++) {
			cpu->cf = s & 1;
			s = s >> 1;
		}

		flag_szp16(cpu, (uint16_t)s);
		break;

	case 7: /* SAR r/m8 */
		for (shift = 1; shift <= cnt; shift++) {
			msb = s & 0x8000;
			cpu->cf = s & 1;
			s = (s >> 1) | msb;
		}

		cpu->of = 0;
		flag_szp16(cpu, (uint16_t)s);
		break;
	}

	return (uint16_t)s & 0xFFFF;
}

FUNC_INLINE void op_div8(CPU_t* cpu, uint16_t valdiv, uint8_t divisor) {
	if (divisor == 0) {
		cpu_intcall(cpu, 0);
		return;
	}

	if ((valdiv / (uint16_t)divisor) > 0xFF) {
		cpu_intcall(cpu, 0);
		return;
	}

	cpu->regs.byteregs[regah] = valdiv % (uint16_t)divisor;
	cpu->regs.byteregs[regal] = valdiv / (uint16_t)divisor;
}

FUNC_INLINE void op_idiv8(CPU_t* cpu, uint16_t valdiv, uint8_t divisor) {
	//TODO: Rewrite IDIV code, I wrote this in 2011. It can be made far more efficient.
	uint16_t	s1;
	uint16_t	s2;
	uint16_t	d1;
	uint16_t	d2;
	int	sign;

	if (divisor == 0) {
		cpu_intcall(cpu, 0);
		return;
	}

	s1 = valdiv;
	s2 = divisor;
	sign = (((s1 ^ s2) & 0x8000) != 0);
	s1 = (s1 < 0x8000) ? s1 : ((~s1 + 1) & 0xffff);
	s2 = (s2 < 0x8000) ? s2 : ((~s2 + 1) & 0xffff);
	d1 = s1 / s2;
	d2 = s1 % s2;
	if (d1 & 0xFF00) {
		cpu_intcall(cpu, 0);
		return;
	}

	if (sign) {
		d1 = (~d1 + 1) & 0xff;
		d2 = (~d2 + 1) & 0xff;
	}

	cpu->regs.byteregs[regah] = (uint8_t)d2;
	cpu->regs.byteregs[regal] = (uint8_t)d1;
}

FUNC_INLINE void op_grp3_8(CPU_t* cpu) {
	cpu->oper1 = signext(cpu->oper1b);
	cpu->oper2 = signext(cpu->oper2b);
	switch (cpu->reg) {
	case 0:
	case 1: /* TEST */
		flag_log8(cpu, cpu->oper1b & getmem8(cpu, cpu->segregs[regcs], cpu->ip));
		StepIP(cpu, 1);
		break;

	case 2: /* NOT */
		cpu->res8 = ~cpu->oper1b;
		flag_log8(cpu, cpu->res8);
		break;

	case 3: /* NEG */
		cpu->res8 = 0 - cpu->oper1b; // (~cpu->oper1b) + 1;
		flag_sub8(cpu, 0, cpu->oper1b);
		if (cpu->res8 == 0) {
			cpu->cf = 0;
		}
		else {
			cpu->cf = 1;
		}
		break;

	case 4: /* MUL */
		cpu->temp1 = (uint32_t)cpu->oper1b * (uint32_t)cpu->regs.byteregs[regal];
		cpu->regs.wordregs[regax] = cpu->temp1 & 0xFFFF;
		flag_szp8(cpu, (uint8_t)cpu->oper1b);
		if (cpu->regs.byteregs[regah]) {
			cpu->cf = 1;
			cpu->of = 1;
		}
		else {
			cpu->cf = 0;
			cpu->of = 0;
		}
		break;

	case 5: /* IMUL */
		cpu->oper1 = signext(cpu->oper1b);
		cpu->temp1 = signext(cpu->regs.byteregs[regal]);
		cpu->temp2 = cpu->oper1;
		if ((cpu->temp1 & 0x80) == 0x80) {
			cpu->temp1 = cpu->temp1 | 0xFFFFFF00;
		}

		if ((cpu->temp2 & 0x80) == 0x80) {
			cpu->temp2 = cpu->temp2 | 0xFFFFFF00;
		}

		cpu->temp3 = (uint32_t)((int32_t)cpu->temp1 * (int32_t)cpu->temp2);
		cpu->regs.wordregs[regax] = cpu->temp3 & 0xFFFF;
		if (cpu->regs.byteregs[regah]) {
			cpu->cf = 1;
			cpu->of = 1;
		}
		else {
			cpu->cf = 0;
			cpu->of = 0;
		}
		break;

	case 6: /* DIV */
		op_div8(cpu, cpu->regs.wordregs[regax], cpu->oper1b);
		break;

	case 7: /* IDIV */
		op_idiv8(cpu, cpu->regs.wordregs[regax], cpu->oper1b);
		break;
	}
}

FUNC_INLINE void op_div16(CPU_t* cpu, uint32_t valdiv, uint16_t divisor) {
	if (divisor == 0) {
		cpu_intcall(cpu, 0);
		return;
	}

	if ((valdiv / (uint32_t)divisor) > 0xFFFF) {
		cpu_intcall(cpu, 0);
		return;
	}

	cpu->regs.wordregs[regdx] = valdiv % (uint32_t)divisor;
	cpu->regs.wordregs[regax] = valdiv / (uint32_t)divisor;
}

FUNC_INLINE void op_idiv16(CPU_t* cpu, uint32_t valdiv, uint16_t divisor) {
	//TODO: Rewrite IDIV code, I wrote this in 2011. It can be made far more efficient.
	uint32_t	d1;
	uint32_t	d2;
	uint32_t	s1;
	uint32_t	s2;
	int	sign;

	if (divisor == 0) {
		cpu_intcall(cpu, 0);
		return;
	}

	s1 = valdiv;
	s2 = divisor;
	s2 = (s2 & 0x8000) ? (s2 | 0xffff0000) : s2;
	sign = (((s1 ^ s2) & 0x80000000) != 0);
	s1 = (s1 < 0x80000000) ? s1 : ((~s1 + 1) & 0xffffffff);
	s2 = (s2 < 0x80000000) ? s2 : ((~s2 + 1) & 0xffffffff);
	d1 = s1 / s2;
	d2 = s1 % s2;
	if (d1 & 0xFFFF0000) {
		cpu_intcall(cpu, 0);
		return;
	}

	if (sign) {
		d1 = (~d1 + 1) & 0xffff;
		d2 = (~d2 + 1) & 0xffff;
	}

	cpu->regs.wordregs[regax] = d1;
	cpu->regs.wordregs[regdx] = d2;
}

FUNC_INLINE void op_grp3_16(CPU_t* cpu) {
	switch (cpu->reg) {
	case 0:
	case 1: /* TEST */
		flag_log16(cpu, cpu->oper1 & getmem16(cpu, cpu->segregs[regcs], cpu->ip));
		StepIP(cpu, 2);
		break;

	case 2: /* NOT */
		cpu->res16 = ~cpu->oper1;
		flag_log16(cpu, cpu->res16);
		break;

	case 3: /* NEG */
		cpu->res16 = 0 - cpu->oper1; // (~cpu->oper1) + 1;
		flag_sub16(cpu, 0, cpu->oper1);
		if (cpu->res16) {
			cpu->cf = 1;
		}
		else {
			cpu->cf = 0;
		}
		break;

	case 4: /* MUL */
		cpu->temp1 = (uint32_t)cpu->oper1 * (uint32_t)cpu->regs.wordregs[regax];
		cpu->regs.wordregs[regax] = cpu->temp1 & 0xFFFF;
		cpu->regs.wordregs[regdx] = cpu->temp1 >> 16;
		flag_szp16(cpu, (uint16_t)cpu->oper1);
		if (cpu->regs.wordregs[regdx]) {
			cpu->cf = 1;
			cpu->of = 1;
		}
		else {
			cpu->cf = 0;
			cpu->of = 0;
		}
		break;

	case 5: /* IMUL */
		cpu->temp1 = cpu->regs.wordregs[regax];
		cpu->temp2 = cpu->oper1;
		if (cpu->temp1 & 0x8000) {
			cpu->temp1 |= 0xFFFF0000;
		}

		if (cpu->temp2 & 0x8000) {
			cpu->temp2 |= 0xFFFF0000;
		}

		cpu->temp3 = (uint32_t)((int32_t)cpu->temp1 * (int32_t)cpu->temp2);
		cpu->regs.wordregs[regax] = cpu->temp3 & 0xFFFF;	/* into register ax */
		cpu->regs.wordregs[regdx] = cpu->temp3 >> 16;	/* into register dx */
		if (cpu->regs.wordregs[regdx]) {
			cpu->cf = 1;
			cpu->of = 1;
		}
		else {
			cpu->cf = 0;
			cpu->of = 0;
		}
		break;

	case 6: /* DIV */
		op_div16(cpu, ((uint32_t)cpu->regs.wordregs[regdx] << 16) + cpu->regs.wordregs[regax], cpu->oper1);
		break;

	case 7: /* DIV */
		op_idiv16(cpu, ((uint32_t)cpu->regs.wordregs[regdx] << 16) + cpu->regs.wordregs[regax], cpu->oper1);
		break;
	}
}

FUNC_INLINE void op_grp5(CPU_t* cpu) {
	switch (cpu->reg) {
	case 0: /* INC Ev */
		cpu->oper2 = 1;
		cpu->tempcf = cpu->cf;
		op_add16(cpu);
		cpu->cf = cpu->tempcf;
		writerm16(cpu, cpu->rm, cpu->res16);
		break;

	case 1: /* DEC Ev */
		cpu->oper2 = 1;
		cpu->tempcf = cpu->cf;
		op_sub16(cpu);
		cpu->cf = cpu->tempcf;
		writerm16(cpu, cpu->rm, cpu->res16);
		break;

	case 2: /* CALL Ev */
		push(cpu, cpu->ip);
		cpu->ip = cpu->oper1;
		break;

	case 3: /* CALL Mp */
		push(cpu, cpu->segregs[regcs]);
		push(cpu, cpu->ip);
		getea(cpu, cpu->rm);
		cpu->ip = (uint16_t)cpu_read(cpu, cpu->ea) + (uint16_t)cpu_read(cpu, cpu->ea + 1) * 256;
		cpu->segregs[regcs] = (uint16_t)cpu_read(cpu, cpu->ea + 2) + (uint16_t)cpu_read(cpu, cpu->ea + 3) * 256;
		break;

	case 4: /* JMP Ev */
		cpu->ip = cpu->oper1;
		break;

	case 5: /* JMP Mp */
	{
		getea(cpu, cpu->rm);
		cpu->ip = cpu_readw(cpu, cpu->ea);
		cpu->segregs[regcs] = cpu_readw(cpu, cpu->ea + 2);
		if (cpu->protected_mode) {
			load_descriptor(cpu, regcs, cpu->segregs[regcs]);
		}
		break;
	}

	case 6: /* PUSH Ev */
		push(cpu, cpu->oper1);
		break;
	}
}

void cpu_intcall(CPU_t* cpu, uint8_t intnum) {
	if (cpu->handling_fault) {
		if (intnum == 8) {
			debug_log(DEBUG_ERROR, "[CPU] Triple Fault triggered. Resetting system.\n");
			cpu_reset(cpu);
			return;
		}
		debug_log(DEBUG_ERROR, "[CPU] Double Fault triggered (INT %u while handling another fault).\n", intnum);
		cpu_intcall(cpu, 8);
		return;
	}

	if (intnum == 8 || intnum == 10 || intnum == 11 || intnum == 12 || intnum == 13) {
		cpu->handling_fault = 1;
	}

	// We cant post 286 bioses yet, we can emulate high level the interrupts required for himem
	if (intnum == 0x15) {
		uint8_t ah = cpu->regs.byteregs[regah];
		if (ah == 0x88) {
			debug_log(DEBUG_INFO, "[BIOS] INT 15h, AH=88h: Get Extended Memory Size\n");
			cpu->regs.wordregs[regax] = 15360;
			cpu->cf = 0;
			return;
		}
		if (ah == 0x87) {
			uint16_t count = cpu->regs.wordregs[regcx];
			uint32_t num_bytes = count * 2;
			uint32_t table_addr = get_real_address(cpu, cpu->segregs[reges], cpu->regs.wordregs[regsi]);
			uint32_t source_base = cpu_read(cpu, table_addr + 10) | (cpu_read(cpu, table_addr + 11) << 8) | (cpu_read(cpu, table_addr + 12) << 16);
			uint32_t dest_base = cpu_read(cpu, table_addr + 18) | (cpu_read(cpu, table_addr + 19) << 8) | (cpu_read(cpu, table_addr + 20) << 16);
			debug_log(DEBUG_INFO, "[BIOS] INT 15h, AH=87h: Move %u words from %06X to %06X\n", count, source_base, dest_base);
			if (num_bytes > 0) {
				for (uint32_t i = 0; i < num_bytes; i++) {
					cpu_write(cpu, dest_base + i, cpu_read(cpu, source_base + i));
				}
			}
			cpu->cf = 0;
			cpu->regs.byteregs[regah] = 0x00;
			cpu->zf = 1;
			return;
		}
	}

	if (cpu->int_callback[intnum] != NULL) {
		(*cpu->int_callback[intnum])(cpu, intnum);
		cpu->handling_fault = 0;
		return;
	}

	if (cpu->protected_mode) {
		uint32_t gate_offset = intnum * 8;
		if (gate_offset + 7 > cpu->idtr.limit) {
			debug_log(DEBUG_ERROR, "[CPU] GPF(#13): INT %u is outside IDT limit.\n", intnum);
			cpu_intcall(cpu, 8);
			return;
		}

		uint32_t gate_addr = cpu->idtr.base + gate_offset;
		uint8_t access = cpu_read(cpu, gate_addr + 5);

		if (!(access & 0x80)) {
			debug_log(DEBUG_ERROR, "[CPU] NPF(#11): Gate for INT %u is not present.\n", intnum);
			cpu_intcall(cpu, 11);
			return;
		}

		uint16_t new_ip = cpu_readw(cpu, gate_addr);
		uint16_t new_cs = cpu_readw(cpu, gate_addr + 2);
		uint8_t gate_type = access & 0x1F;
		uint8_t gate_dpl = (access >> 5) & 3;

		uint32_t target_desc_base;
		uint16_t target_desc_limit;
		uint8_t target_desc_access;

		if (!get_descriptor_info(cpu, new_cs, &target_desc_base, &target_desc_limit, &target_desc_access)) {
			debug_log(DEBUG_ERROR, "[CPU] GPF(#13): Invalid CS selector 0x%04X in gate for INT %u.\n", new_cs, intnum);
			cpu_intcall(cpu, 13);
			return;
		}

		uint8_t target_dpl = (target_desc_access >> 5) & 3;
		uint8_t cpl = cpu->segregs[regcs] & 3;

		uint16_t old_flags = makeflagsword(cpu);
		uint16_t old_cs = cpu->segregs[regcs];
		uint16_t old_ip = cpu->ip;

		if (target_dpl < cpl) {
			if (!cpu->tr_cache.valid) {
				debug_log(DEBUG_ERROR, "[CPU] GPF(#13): Invalid TSS during privilege change for INT %u.\n", intnum);
				cpu_intcall(cpu, 8);
				return;
			}
			uint16_t new_sp = cpu->tr_cache.sp0;
			uint16_t new_ss = cpu->tr_cache.ss0;
			uint16_t old_ss = cpu->segregs[regss];
			uint16_t old_sp = cpu->regs.wordregs[regsp];

			load_descriptor(cpu, regss, new_ss);
			cpu->segregs[regss] = new_ss;
			cpu->regs.wordregs[regsp] = new_sp;

			push(cpu, old_ss);
			push(cpu, old_sp);
			push(cpu, old_flags);
			push(cpu, old_cs);
			push(cpu, old_ip);
			if (intnum == 8 || (intnum >= 10 && intnum <= 13)) {
				push(cpu, 0);
			}

		}
		else {
			push(cpu, old_flags);
			push(cpu, old_cs);
			push(cpu, old_ip);
			if (intnum == 8 || (intnum >= 10 && intnum <= 13)) {
				push(cpu, 0);
			}
		}

		load_descriptor(cpu, regcs, new_cs);
		cpu->segregs[regcs] = new_cs;
		cpu->ip = new_ip;

		cpu->tf = 0;
		if (gate_type == 0x06) {
			cpu->ifl = 0;
		}

		cpu->handling_fault = 0;
		return;
	}
	else {
		uint16_t flags_to_push = makeflagsword(cpu);
		cpu->ifl = 0;
		cpu->tf = 0;
		push(cpu, flags_to_push);
		push(cpu, cpu->segregs[regcs]);
		push(cpu, cpu->ip);
		cpu->segregs[regcs] = getmem16(cpu, 0, (uint16_t)intnum * 4 + 2);
		cpu->ip = getmem16(cpu, 0, (uint16_t)intnum * 4);
	}
	cpu->handling_fault = 0;
}

void cpu_interruptCheck(CPU_t* cpu, I8259_t* i8259) {
	/* get next interrupt from the i8259, if any */
	if (!cpu->trap_toggle && (cpu->ifl && (i8259->irr & (~i8259->imr)))) {
		cpu->hltstate = 0;
		cpu_intcall(cpu, i8259_nextintr(i8259));
	}
}

static uint32_t read_24bit_base(CPU_t* cpu, uint32_t addr) {
	return cpu_read(cpu, addr) | (cpu_read(cpu, addr + 1) << 8) | (cpu_read(cpu, addr + 2) << 16);
}

void cpu_exec(CPU_t* cpu, uint32_t execloops) {

	uint32_t loopcount;
	uint8_t docontinue;
	static uint16_t firstip;

	for (loopcount = 0; loopcount < execloops; loopcount++) {

		if (cpu->trap_toggle) {
			cpu_intcall(cpu, 1);
		}

		if (cpu->tf) {
			cpu->trap_toggle = 1;
		}
		else {
			cpu->trap_toggle = 0;
		}

		if (cpu->hltstate) goto skipexecution;

		cpu->reptype = 0;
		cpu->segoverride = 0;
		cpu->useseg = cpu->segregs[regds];
		docontinue = 0;
		firstip = cpu->ip;
		uint8_t prefix_count = 0;

		while (!docontinue) {
			cpu->segregs[regcs] = cpu->segregs[regcs] & 0xFFFF;
			cpu->ip = cpu->ip & 0xFFFF;
			cpu->savecs = cpu->segregs[regcs];
			cpu->saveip = cpu->ip;
			cpu->opcode = getmem8(cpu, cpu->segregs[regcs], cpu->ip);
			StepIP(cpu, 1);

			prefix_count++;
			if (prefix_count > 10) {
				cpu_intcall(cpu, 13);
				docontinue = 1;
				break;
			}

			switch (cpu->opcode) {
				/* segment prefix check */
			case 0x2E:	/* segment cpu->segregs[regcs] */
				cpu->useseg = cpu->segregs[regcs];
				cpu->segoverride = 1;
				break;

			case 0x3E:	/* segment cpu->segregs[regds] */
				cpu->useseg = cpu->segregs[regds];
				cpu->segoverride = 1;
				break;

			case 0x26:	/* segment cpu->segregs[reges] */
				cpu->useseg = cpu->segregs[reges];
				cpu->segoverride = 1;
				break;

			case 0x36:	/* segment cpu->segregs[regss] */
				cpu->useseg = cpu->segregs[regss];
				cpu->segoverride = 1;
				break;

			case 0xF0: /* LOCK prefix */
				break;

				/* repetition prefix check */
			case 0xF3:	/* REP/REPE/REPZ */
				cpu->reptype = 1;
				break;

			case 0xF2:	/* REPNE/REPNZ */
				cpu->reptype = 2;
				break;

			default:
				docontinue = 1;
				break;
			}
		}

#if 0
		printf("%04X:%04X  %02X %02X %02X %02X\n",
			cpu->savecs,
			firstip,
			cpu->opcode,
			getmem8(cpu, cpu->segregs[regcs], cpu->ip + 0),
			getmem8(cpu, cpu->segregs[regcs], cpu->ip + 1),
			getmem8(cpu, cpu->segregs[regcs], cpu->ip + 2)
		);
#endif

		cpu->totalexec++;

		switch (cpu->opcode) {
		case 0x0:	/* 00 ADD Eb Gb */
			modregrm(cpu);
			cpu->oper1b = readrm8(cpu, cpu->rm);
			cpu->oper2b = getreg8(cpu, cpu->reg);
			op_add8(cpu);
			writerm8(cpu, cpu->rm, cpu->res8);
			break;

		case 0x1:	/* 01 ADD Ev Gv */
			modregrm(cpu);
			cpu->oper1 = readrm16(cpu, cpu->rm);
			cpu->oper2 = getreg16(cpu, cpu->reg);
			op_add16(cpu);
			writerm16(cpu, cpu->rm, cpu->res16);
			break;

		case 0x2:	/* 02 ADD Gb Eb */
			modregrm(cpu);
			cpu->oper1b = getreg8(cpu, cpu->reg);
			cpu->oper2b = readrm8(cpu, cpu->rm);
			op_add8(cpu);
			putreg8(cpu, cpu->reg, cpu->res8);
			break;

		case 0x3:	/* 03 ADD Gv Ev */
			modregrm(cpu);
			cpu->oper1 = getreg16(cpu, cpu->reg);
			cpu->oper2 = readrm16(cpu, cpu->rm);
			op_add16(cpu);
			putreg16(cpu, cpu->reg, cpu->res16);
			break;

		case 0x4:	/* 04 ADD cpu->regs.byteregs[regal] Ib */
			cpu->oper1b = cpu->regs.byteregs[regal];
			cpu->oper2b = getmem8(cpu, cpu->segregs[regcs], cpu->ip);
			StepIP(cpu, 1);
			op_add8(cpu);
			cpu->regs.byteregs[regal] = cpu->res8;
			break;

		case 0x5:	/* 05 ADD eAX Iv */
			cpu->oper1 = cpu->regs.wordregs[regax];
			cpu->oper2 = getmem16(cpu, cpu->segregs[regcs], cpu->ip);
			StepIP(cpu, 2);
			op_add16(cpu);
			cpu->regs.wordregs[regax] = cpu->res16;
			break;

		case 0x6:	/* 06 PUSH cpu->segregs[reges] */
			push(cpu, cpu->segregs[reges]);
			break;

		case 0x7:	/* 07 POP cpu->segregs[reges] */
			cpu->oper1 = pop(cpu);
			if (cpu->protected_mode) {
				load_descriptor(cpu, reges, cpu->oper1);
			}
			cpu->segregs[reges] = cpu->oper1;
			break;

		case 0x8:	/* 08 OR Eb Gb */
			modregrm(cpu);
			cpu->oper1b = readrm8(cpu, cpu->rm);
			cpu->oper2b = getreg8(cpu, cpu->reg);
			op_or8(cpu);
			writerm8(cpu, cpu->rm, cpu->res8);
			break;

		case 0x9:	/* 09 OR Ev Gv */
			modregrm(cpu);
			cpu->oper1 = readrm16(cpu, cpu->rm);
			cpu->oper2 = getreg16(cpu, cpu->reg);
			op_or16(cpu);
			writerm16(cpu, cpu->rm, cpu->res16);
			break;

		case 0xA:	/* 0A OR Gb Eb */
			modregrm(cpu);
			cpu->oper1b = getreg8(cpu, cpu->reg);
			cpu->oper2b = readrm8(cpu, cpu->rm);
			op_or8(cpu);
			putreg8(cpu, cpu->reg, cpu->res8);
			break;

		case 0xB:	/* 0B OR Gv Ev */
			modregrm(cpu);
			cpu->oper1 = getreg16(cpu, cpu->reg);
			cpu->oper2 = readrm16(cpu, cpu->rm);
			op_or16(cpu);
			putreg16(cpu, cpu->reg, cpu->res16);
			break;

		case 0xC:	/* 0C OR cpu->regs.byteregs[regal] Ib */
			cpu->oper1b = cpu->regs.byteregs[regal];
			cpu->oper2b = getmem8(cpu, cpu->segregs[regcs], cpu->ip);
			StepIP(cpu, 1);
			op_or8(cpu);
			cpu->regs.byteregs[regal] = cpu->res8;
			break;

		case 0xD:	/* 0D OR eAX Iv */
			cpu->oper1 = cpu->regs.wordregs[regax];
			cpu->oper2 = getmem16(cpu, cpu->segregs[regcs], cpu->ip);
			StepIP(cpu, 2);
			op_or16(cpu);
			cpu->regs.wordregs[regax] = cpu->res16;
			break;

		case 0xE:	/* 0E PUSH cpu->segregs[regcs] */
			push(cpu, cpu->segregs[regcs]);
			break;

		case 0x0F: /* 286 "extended" opcodes */
			cpu->opcode = getmem8(cpu, cpu->segregs[regcs], cpu->ip);
			StepIP(cpu, 1);
			debug_log(DEBUG_INFO, "[CPU] Opcode 0Fh, %02Xh\n", cpu->opcode);

			switch (cpu->opcode) {
			case 0x00: /* Group 6 Instructions */
				modregrm(cpu);
				if (cpu->protected_mode) {
					switch (cpu->reg) {
					case 0: // SLDT
						writerm16(cpu, cpu->rm, cpu->ldtr);
						break;
					case 1: // STR
						writerm16(cpu, cpu->rm, cpu->tr);
						break;
					case 2: // LLDT
						cpu->ldtr = readrm16(cpu, cpu->rm);
						load_ldtr(cpu, cpu->ldtr);
						break;
					case 3: // LTR
						if ((cpu->segregs[regcs] & 3) != 0) {
							debug_log(DEBUG_ERROR, "[CPU] LTR: GPF(#13) - CPL != 0\n");
							cpu_intcall(cpu, 13);
							break;
						}
						cpu->tr = readrm16(cpu,	 cpu->rm);
						load_tr(cpu, cpu->tr);
						break;
					case 0x04: // VERR
					case 0x05: // VERW
					{
						uint32_t base;
						uint16_t limit;
						uint8_t access;
						uint16_t selector = readrm16(cpu, cpu->rm);
						uint8_t cpl = cpu->segregs[regcs] & 3;
						cpu->zf = 0;
						if (selector != 0 && get_descriptor_info(cpu, selector, &base, &limit, &access)) {
							bool is_system = (access & 0x10) == 0x00;
							if (!is_system) {
								bool is_code = access & 0x08;
								bool readable = access & 0x02;
								bool writable = access & 0x02;
								uint8_t seg_dpl = (access >> 5) & 3;
								uint8_t rpl = selector & 3;
								if ((seg_dpl >= cpl) && (seg_dpl >= rpl)) {
									if (cpu->opcode == 0x04 && is_code && readable) cpu->zf = 1;
									if (cpu->opcode == 0x05 && !is_code && writable) cpu->zf = 1;
								}
							}
						}
						break;
					}
					default:
						debug_log(DEBUG_ERROR, "[CPU] Unhandled Group 6 /0Fh opcode reg=%d (rm=%d)\n", cpu->reg, cpu->rm);
						cpu_intcall(cpu, 6);
						break;
					}
				}
				else {
					cpu_intcall(cpu, 6);
				}
				break;
			case 0x01: /* Group 7 Instructions */
				modregrm(cpu);
				switch (cpu->reg) {
				case 0: // SGDT
					getea(cpu, cpu->rm);
					cpu_writew(cpu, cpu->ea, cpu->gdtr.limit);
					cpu_write(cpu, cpu->ea + 2, (cpu->gdtr.base) & 0xFF);
					cpu_write(cpu, cpu->ea + 3, (cpu->gdtr.base >> 8) & 0xFF);
					cpu_write(cpu, cpu->ea + 4, (cpu->gdtr.base >> 16) & 0xFF);
					break;
				case 1: // SIDT
					getea(cpu, cpu->rm);
					cpu_writew(cpu, cpu->ea, cpu->idtr.limit);
					cpu_write(cpu, cpu->ea + 2, (cpu->idtr.base) & 0xFF);
					cpu_write(cpu, cpu->ea + 3, (cpu->idtr.base >> 8) & 0xFF);
					cpu_write(cpu, cpu->ea + 4, (cpu->idtr.base >> 16) & 0xFF);
					break;
				case 2: // LGDT
					getea(cpu, cpu->rm);
					uint16_t gdt_limit = cpu_readw(cpu, cpu->ea);
					uint32_t gdt_base = cpu_read(cpu, cpu->ea + 2)
						| (cpu_read(cpu, cpu->ea + 3) << 8)
						| (cpu_read(cpu, cpu->ea + 4) << 16);
					cpu->gdtr.limit = gdt_limit;
					cpu->gdtr.base = gdt_base;
					break;
				case 3: // LIDT
					getea(cpu, cpu->rm);
					uint16_t idt_limit = cpu_readw(cpu, cpu->ea);
					uint32_t idt_base = cpu_read(cpu, cpu->ea + 2)
						| (cpu_read(cpu, cpu->ea + 3) << 8)
						| (cpu_read(cpu, cpu->ea + 4) << 16);
					cpu->idtr.limit = idt_limit;
					cpu->idtr.base = idt_base;
					break;
				case 4: // SMSW Ew
					writerm16(cpu, cpu->rm, cpu->msw);
					break;
				case 6: // LMSW Ew
					cpu->oper1 = readrm16(cpu, cpu->rm);

					if (cpu->msw & 1) {
						cpu->oper1 |= 1;
					}

					cpu->msw = (cpu->msw & 0xFFF0) | (cpu->oper1 & 0x000F);

					if (!cpu->protected_mode && (cpu->msw & 1)) {
						debug_log(DEBUG_INFO, "[CPU] Entering Protected Mode\n");
						cpu->protected_mode = 1;

						cpu->segcache[regcs].base = (uint32_t)cpu->segregs[regcs] << 4;
						cpu->segcache[regcs].limit = 0xFFFF;
						cpu->segcache[regcs].access = 0x93;
						cpu->segcache[regcs].valid = 1;

						cpu->segcache[regds].base = (uint32_t)cpu->segregs[regds] << 4;
						cpu->segcache[regds].limit = 0xFFFF;
						cpu->segcache[regds].access = 0x93;
						cpu->segcache[regds].valid = 1;

						cpu->segcache[reges].base = (uint32_t)cpu->segregs[reges] << 4;
						cpu->segcache[reges].limit = 0xFFFF;
						cpu->segcache[reges].access = 0x93;
						cpu->segcache[reges].valid = 1;

						cpu->segcache[regss].base = (uint32_t)cpu->segregs[regss] << 4;
						cpu->segcache[regss].limit = 0xFFFF;
						cpu->segcache[regss].access = 0x93;
						cpu->segcache[regss].valid = 1;
					}
					break;
				default:
					debug_log(DEBUG_ERROR, "[CPU] Unhandled Group 7 /0Fh opcode reg=%d (rm=%d)\n", cpu->reg, cpu->rm);
					cpu_intcall(cpu, 6);
					break;
				}
				break;
			case 0x02: // LAR
			case 0x03: // LSL
			{
				modregrm(cpu);
				uint32_t base;
				uint16_t limit;
				uint8_t access;
				uint16_t sel = readrm16(cpu, cpu->rm);
				uint8_t cpl = cpu->segregs[regcs] & 3;
				uint8_t rpl = sel & 3;

				cpu->zf = 0;

				if (get_descriptor_info(cpu, sel, &base, &limit, &access)) {
					uint8_t type = (access >> 0) & 0x1F;
					uint8_t dpl = (access >> 5) & 3;

					if (dpl >= cpl && dpl >= rpl) {
						bool valid_type = false;
						if (cpu->opcode == 0x02) { // LAR
							if (type != 0x00 && type != 0x08 && type != 0x0A && type != 0x0D) {
								valid_type = true;
							}
						}
						else { // LSL
							if (type != 0x00 && type != 0x04 && type != 0x05 && type != 0x06 &&
								type != 0x07 && type != 0x0C && type != 0x0E && type != 0x0F) {
								valid_type = true;
							}
						}

						if (valid_type) {
							cpu->zf = 1;
							if (cpu->opcode == 0x02) { // LAR
								putreg16(cpu, cpu->reg, access << 8);
							}
							else { // LSL
								putreg16(cpu, cpu->reg, limit);
							}
						}
					}
				}
				break;
			}
			case 0x04: /* STOREALL - 286 version */
			{
				// This isnt really used and shuts down cpu after storing debug stuff, lets just halt
				cpu->hltstate = 1;
				break;
			}
			case 0x05: /* LOADALL - 286 version */
				if (cpu->protected_mode) {
					cpu_intcall(cpu, 6);
					break;
				}

				uint32_t addr = 0x800;

				cpu->segcache[reges].limit = cpu_readw(cpu, addr + 0x1E);
				cpu->segcache[reges].base = read_24bit_base(cpu, addr + 0x1B);
				cpu->segcache[reges].access = cpu_read(cpu, addr + 0x1A);
				cpu->segcache[reges].valid = 1;

				cpu->segcache[regcs].limit = cpu_readw(cpu, addr + 0x24);
				cpu->segcache[regcs].base = read_24bit_base(cpu, addr + 0x21);
				cpu->segcache[regcs].access = cpu_read(cpu, addr + 0x20);
				cpu->segcache[regcs].valid = 1;

				cpu->segcache[regss].limit = cpu_readw(cpu, addr + 0x2A);
				cpu->segcache[regss].base = read_24bit_base(cpu, addr + 0x27);
				cpu->segcache[regss].access = cpu_read(cpu, addr + 0x26);
				cpu->segcache[regss].valid = 1;

				cpu->segcache[regds].limit = cpu_readw(cpu, addr + 0x30);
				cpu->segcache[regds].base = read_24bit_base(cpu, addr + 0x2D);
				cpu->segcache[regds].access = cpu_read(cpu, addr + 0x2C);
				cpu->segcache[regds].valid = 1;

				cpu->regs.wordregs[regdi] = cpu_readw(cpu, addr + 0x32);
				cpu->regs.wordregs[regsi] = cpu_readw(cpu, addr + 0x34);
				cpu->regs.wordregs[regbp] = cpu_readw(cpu, addr + 0x36);
				cpu->regs.wordregs[regsp] = cpu_readw(cpu, addr + 0x38);
				cpu->regs.wordregs[regbx] = cpu_readw(cpu, addr + 0x3A);
				cpu->regs.wordregs[regdx] = cpu_readw(cpu, addr + 0x3C);
				cpu->regs.wordregs[regcx] = cpu_readw(cpu, addr + 0x3E);
				cpu->regs.wordregs[regax] = cpu_readw(cpu, addr + 0x40);

				decodeflagsword(cpu, cpu_readw(cpu, addr + 0x42));
				cpu->ip = cpu_readw(cpu, addr + 0x44);
				cpu->ldtr = cpu_readw(cpu, addr + 0x46);
				cpu->tr = cpu_readw(cpu, addr + 0x54);
				cpu->segregs[regds] = cpu_readw(cpu, addr + 0x48);
				cpu->segregs[regss] = cpu_readw(cpu, addr + 0x4A);
				cpu->segregs[regcs] = cpu_readw(cpu, addr + 0x4C);
				cpu->segregs[reges] = cpu_readw(cpu, addr + 0x4E);

				cpu->gdtr.limit = cpu_readw(cpu, addr + 0x56);
				cpu->gdtr.base = read_24bit_base(cpu, addr + 0x58);
				cpu->idtr.limit = cpu_readw(cpu, addr + 0x5C);
				cpu->idtr.base = read_24bit_base(cpu, addr + 0x5E);

				cpu->msw = cpu_readw(cpu, addr + 0x66);
				if (!cpu->protected_mode && (cpu->msw & 1)) {
					debug_log(DEBUG_INFO, "[CPU] Entering Protected Mode\n");
				}
				cpu->protected_mode = (cpu->msw & 1);
				break;
			case 0x06: /* CLTS */
				cpu->msw &= ~0x0008;
				break;
			default:
				debug_log(DEBUG_ERROR, "[CPU] Unhandled 0Fh opcode: %02Xh\n", cpu->opcode);
				cpu_intcall(cpu, 6);
				break;
			}
			break;

		case 0x10:	/* 10 ADC Eb Gb */
			modregrm(cpu);
			cpu->oper1b = readrm8(cpu, cpu->rm);
			cpu->oper2b = getreg8(cpu, cpu->reg);
			op_adc8(cpu);
			writerm8(cpu, cpu->rm, cpu->res8);
			break;

		case 0x11:	/* 11 ADC Ev Gv */
			modregrm(cpu);
			cpu->oper1 = readrm16(cpu, cpu->rm);
			cpu->oper2 = getreg16(cpu, cpu->reg);
			op_adc16(cpu);
			writerm16(cpu, cpu->rm, cpu->res16);
			break;

		case 0x12:	/* 12 ADC Gb Eb */
			modregrm(cpu);
			cpu->oper1b = getreg8(cpu, cpu->reg);
			cpu->oper2b = readrm8(cpu, cpu->rm);
			op_adc8(cpu);
			putreg8(cpu, cpu->reg, cpu->res8);
			break;

		case 0x13:	/* 13 ADC Gv Ev */
			modregrm(cpu);
			cpu->oper1 = getreg16(cpu, cpu->reg);
			cpu->oper2 = readrm16(cpu, cpu->rm);
			op_adc16(cpu);
			putreg16(cpu, cpu->reg, cpu->res16);
			break;

		case 0x14:	/* 14 ADC cpu->regs.byteregs[regal] Ib */
			cpu->oper1b = cpu->regs.byteregs[regal];
			cpu->oper2b = getmem8(cpu, cpu->segregs[regcs], cpu->ip);
			StepIP(cpu, 1);
			op_adc8(cpu);
			cpu->regs.byteregs[regal] = cpu->res8;
			break;

		case 0x15:	/* 15 ADC eAX Iv */
			cpu->oper1 = cpu->regs.wordregs[regax];
			cpu->oper2 = getmem16(cpu, cpu->segregs[regcs], cpu->ip);
			StepIP(cpu, 2);
			op_adc16(cpu);
			cpu->regs.wordregs[regax] = cpu->res16;
			break;

		case 0x16:	/* 16 PUSH cpu->segregs[regss] */
			push(cpu, cpu->segregs[regss]);
			break;

		case 0x17:	/* 17 POP cpu->segregs[regss] */
			cpu->oper1 = pop(cpu);
			if (cpu->protected_mode) {
				load_descriptor(cpu, regss, cpu->oper1);
			}
			cpu->segregs[regss] = cpu->oper1;
			break;

		case 0x18:	/* 18 SBB Eb Gb */
			modregrm(cpu);
			cpu->oper1b = readrm8(cpu, cpu->rm);
			cpu->oper2b = getreg8(cpu, cpu->reg);
			op_sbb8(cpu);
			writerm8(cpu, cpu->rm, cpu->res8);
			break;

		case 0x19:	/* 19 SBB Ev Gv */
			modregrm(cpu);
			cpu->oper1 = readrm16(cpu, cpu->rm);
			cpu->oper2 = getreg16(cpu, cpu->reg);
			op_sbb16(cpu);
			writerm16(cpu, cpu->rm, cpu->res16);
			break;

		case 0x1A:	/* 1A SBB Gb Eb */
			modregrm(cpu);
			cpu->oper1b = getreg8(cpu, cpu->reg);
			cpu->oper2b = readrm8(cpu, cpu->rm);
			op_sbb8(cpu);
			putreg8(cpu, cpu->reg, cpu->res8);
			break;

		case 0x1B:	/* 1B SBB Gv Ev */
			modregrm(cpu);
			cpu->oper1 = getreg16(cpu, cpu->reg);
			cpu->oper2 = readrm16(cpu, cpu->rm);
			op_sbb16(cpu);
			putreg16(cpu, cpu->reg, cpu->res16);
			break;

		case 0x1C:	/* 1C SBB cpu->regs.byteregs[regal] Ib */
			cpu->oper1b = cpu->regs.byteregs[regal];
			cpu->oper2b = getmem8(cpu, cpu->segregs[regcs], cpu->ip);
			StepIP(cpu, 1);
			op_sbb8(cpu);
			cpu->regs.byteregs[regal] = cpu->res8;
			break;

		case 0x1D:	/* 1D SBB eAX Iv */
			cpu->oper1 = cpu->regs.wordregs[regax];
			cpu->oper2 = getmem16(cpu, cpu->segregs[regcs], cpu->ip);
			StepIP(cpu, 2);
			op_sbb16(cpu);
			cpu->regs.wordregs[regax] = cpu->res16;
			break;

		case 0x1E:	/* 1E PUSH cpu->segregs[regds] */
			push(cpu, cpu->segregs[regds]);
			break;

		case 0x1F:	/* 1F POP cpu->segregs[regds] */
			cpu->oper1 = pop(cpu);
			if (cpu->protected_mode) {
				load_descriptor(cpu, regds, cpu->oper1);
			}
			cpu->segregs[regds] = cpu->oper1;
			break;

		case 0x20:	/* 20 AND Eb Gb */
			modregrm(cpu);
			cpu->oper1b = readrm8(cpu, cpu->rm);
			cpu->oper2b = getreg8(cpu, cpu->reg);
			op_and8(cpu);
			writerm8(cpu, cpu->rm, cpu->res8);
			break;

		case 0x21:	/* 21 AND Ev Gv */
			modregrm(cpu);
			cpu->oper1 = readrm16(cpu, cpu->rm);
			cpu->oper2 = getreg16(cpu, cpu->reg);
			op_and16(cpu);
			writerm16(cpu, cpu->rm, cpu->res16);
			break;

		case 0x22:	/* 22 AND Gb Eb */
			modregrm(cpu);
			cpu->oper1b = getreg8(cpu, cpu->reg);
			cpu->oper2b = readrm8(cpu, cpu->rm);
			op_and8(cpu);
			putreg8(cpu, cpu->reg, cpu->res8);
			break;

		case 0x23:	/* 23 AND Gv Ev */
			modregrm(cpu);
			cpu->oper1 = getreg16(cpu, cpu->reg);
			cpu->oper2 = readrm16(cpu, cpu->rm);
			op_and16(cpu);
			putreg16(cpu, cpu->reg, cpu->res16);
			break;

		case 0x24:	/* 24 AND cpu->regs.byteregs[regal] Ib */
			cpu->oper1b = cpu->regs.byteregs[regal];
			cpu->oper2b = getmem8(cpu, cpu->segregs[regcs], cpu->ip);
			StepIP(cpu, 1);
			op_and8(cpu);
			cpu->regs.byteregs[regal] = cpu->res8;
			break;

		case 0x25:	/* 25 AND eAX Iv */
			cpu->oper1 = cpu->regs.wordregs[regax];
			cpu->oper2 = getmem16(cpu, cpu->segregs[regcs], cpu->ip);
			StepIP(cpu, 2);
			op_and16(cpu);
			cpu->regs.wordregs[regax] = cpu->res16;
			break;

		case 0x27:	/* 27 DAA */
		{
			uint8_t old_al;
			old_al = cpu->regs.byteregs[regal];
			if (((cpu->regs.byteregs[regal] & 0x0F) > 9) || cpu->af) {
				cpu->oper1 = (uint16_t)cpu->regs.byteregs[regal] + 0x06;
				cpu->regs.byteregs[regal] = cpu->oper1 & 0xFF;
				if (cpu->oper1 & 0xFF00) cpu->cf = 1;
				if ((cpu->oper1 & 0x000F) < (old_al & 0x0F)) cpu->af = 1;
			}
			if (((cpu->regs.byteregs[regal] & 0xF0) > 0x90) || cpu->cf) {
				cpu->oper1 = (uint16_t)cpu->regs.byteregs[regal] + 0x60;
				cpu->regs.byteregs[regal] = cpu->oper1 & 0xFF;
				if (cpu->oper1 & 0xFF00) cpu->cf = 1; else cpu->cf = 0;
			}
			flag_szp8(cpu, cpu->regs.byteregs[regal]);
			break;
		}

		case 0x28:	/* 28 SUB Eb Gb */
			modregrm(cpu);
			cpu->oper1b = readrm8(cpu, cpu->rm);
			cpu->oper2b = getreg8(cpu, cpu->reg);
			op_sub8(cpu);
			writerm8(cpu, cpu->rm, cpu->res8);
			break;

		case 0x29:	/* 29 SUB Ev Gv */
			modregrm(cpu);
			cpu->oper1 = readrm16(cpu, cpu->rm);
			cpu->oper2 = getreg16(cpu, cpu->reg);
			op_sub16(cpu);
			writerm16(cpu, cpu->rm, cpu->res16);
			break;

		case 0x2A:	/* 2A SUB Gb Eb */
			modregrm(cpu);
			cpu->oper1b = getreg8(cpu, cpu->reg);
			cpu->oper2b = readrm8(cpu, cpu->rm);
			op_sub8(cpu);
			putreg8(cpu, cpu->reg, cpu->res8);
			break;

		case 0x2B:	/* 2B SUB Gv Ev */
			modregrm(cpu);
			cpu->oper1 = getreg16(cpu, cpu->reg);
			cpu->oper2 = readrm16(cpu, cpu->rm);
			op_sub16(cpu);
			putreg16(cpu, cpu->reg, cpu->res16);
			break;

		case 0x2C:	/* 2C SUB cpu->regs.byteregs[regal] Ib */
			cpu->oper1b = cpu->regs.byteregs[regal];
			cpu->oper2b = getmem8(cpu, cpu->segregs[regcs], cpu->ip);
			StepIP(cpu, 1);
			op_sub8(cpu);
			cpu->regs.byteregs[regal] = cpu->res8;
			break;

		case 0x2D:	/* 2D SUB eAX Iv */
			cpu->oper1 = cpu->regs.wordregs[regax];
			cpu->oper2 = getmem16(cpu, cpu->segregs[regcs], cpu->ip);
			StepIP(cpu, 2);
			op_sub16(cpu);
			cpu->regs.wordregs[regax] = cpu->res16;
			break;

		case 0x2F:	/* 2F DAS */
		{
			uint8_t old_al;
			old_al = cpu->regs.byteregs[regal];
			if (((cpu->regs.byteregs[regal] & 0x0F) > 9) || cpu->af) {
				cpu->oper1 = (uint16_t)cpu->regs.byteregs[regal] - 0x06;
				cpu->regs.byteregs[regal] = cpu->oper1 & 0xFF;
				if (cpu->oper1 & 0xFF00) cpu->cf = 1;
				if ((cpu->oper1 & 0x000F) >= (old_al & 0x0F)) cpu->af = 1;
			}
			if (((cpu->regs.byteregs[regal] & 0xF0) > 0x90) || cpu->cf) {
				cpu->oper1 = (uint16_t)cpu->regs.byteregs[regal] - 0x60;
				cpu->regs.byteregs[regal] = cpu->oper1 & 0xFF;
				if (cpu->oper1 & 0xFF00) cpu->cf = 1; else cpu->cf = 0;
			}
			flag_szp8(cpu, cpu->regs.byteregs[regal]);
			break;
		}

		case 0x30:	/* 30 XOR Eb Gb */
			modregrm(cpu);
			cpu->oper1b = readrm8(cpu, cpu->rm);
			cpu->oper2b = getreg8(cpu, cpu->reg);
			op_xor8(cpu);
			writerm8(cpu, cpu->rm, cpu->res8);
			break;

		case 0x31:	/* 31 XOR Ev Gv */
			modregrm(cpu);
			cpu->oper1 = readrm16(cpu, cpu->rm);
			cpu->oper2 = getreg16(cpu, cpu->reg);
			op_xor16(cpu);
			writerm16(cpu, cpu->rm, cpu->res16);
			break;

		case 0x32:	/* 32 XOR Gb Eb */
			modregrm(cpu);
			cpu->oper1b = getreg8(cpu, cpu->reg);
			cpu->oper2b = readrm8(cpu, cpu->rm);
			op_xor8(cpu);
			putreg8(cpu, cpu->reg, cpu->res8);
			break;

		case 0x33:	/* 33 XOR Gv Ev */
			modregrm(cpu);
			cpu->oper1 = getreg16(cpu, cpu->reg);
			cpu->oper2 = readrm16(cpu, cpu->rm);
			op_xor16(cpu);
			putreg16(cpu, cpu->reg, cpu->res16);
			break;

		case 0x34:	/* 34 XOR cpu->regs.byteregs[regal] Ib */
			cpu->oper1b = cpu->regs.byteregs[regal];
			cpu->oper2b = getmem8(cpu, cpu->segregs[regcs], cpu->ip);
			StepIP(cpu, 1);
			op_xor8(cpu);
			cpu->regs.byteregs[regal] = cpu->res8;
			break;

		case 0x35:	/* 35 XOR eAX Iv */
			cpu->oper1 = cpu->regs.wordregs[regax];
			cpu->oper2 = getmem16(cpu, cpu->segregs[regcs], cpu->ip);
			StepIP(cpu, 2);
			op_xor16(cpu);
			cpu->regs.wordregs[regax] = cpu->res16;
			break;

		case 0x37:	/* 37 AAA ASCII */
			if (((cpu->regs.byteregs[regal] & 0xF) > 9) || (cpu->af == 1)) {
				cpu->regs.wordregs[regax] = cpu->regs.wordregs[regax] + 0x106;
				cpu->af = 1;
				cpu->cf = 1;
			}
			else {
				cpu->af = 0;
				cpu->cf = 0;
			}

			cpu->regs.byteregs[regal] = cpu->regs.byteregs[regal] & 0xF;
			break;

		case 0x38:	/* 38 CMP Eb Gb */
			modregrm(cpu);
			cpu->oper1b = readrm8(cpu, cpu->rm);
			cpu->oper2b = getreg8(cpu, cpu->reg);
			flag_sub8(cpu, cpu->oper1b, cpu->oper2b);
			break;

		case 0x39:	/* 39 CMP Ev Gv */
			modregrm(cpu);
			cpu->oper1 = readrm16(cpu, cpu->rm);
			cpu->oper2 = getreg16(cpu, cpu->reg);
			flag_sub16(cpu, cpu->oper1, cpu->oper2);
			break;

		case 0x3A:	/* 3A CMP Gb Eb */
			modregrm(cpu);
			cpu->oper1b = getreg8(cpu, cpu->reg);
			cpu->oper2b = readrm8(cpu, cpu->rm);
			flag_sub8(cpu, cpu->oper1b, cpu->oper2b);
			break;

		case 0x3B:	/* 3B CMP Gv Ev */
			modregrm(cpu);
			cpu->oper1 = getreg16(cpu, cpu->reg);
			cpu->oper2 = readrm16(cpu, cpu->rm);
			flag_sub16(cpu, cpu->oper1, cpu->oper2);
			break;

		case 0x3C:	/* 3C CMP cpu->regs.byteregs[regal] Ib */
			cpu->oper1b = cpu->regs.byteregs[regal];
			cpu->oper2b = getmem8(cpu, cpu->segregs[regcs], cpu->ip);
			StepIP(cpu, 1);
			flag_sub8(cpu, cpu->oper1b, cpu->oper2b);
			break;

		case 0x3D:	/* 3D CMP eAX Iv */
			cpu->oper1 = cpu->regs.wordregs[regax];
			cpu->oper2 = getmem16(cpu, cpu->segregs[regcs], cpu->ip);
			StepIP(cpu, 2);
			flag_sub16(cpu, cpu->oper1, cpu->oper2);
			break;

		case 0x3F:	/* 3F AAS ASCII */
			if (((cpu->regs.byteregs[regal] & 0xF) > 9) || (cpu->af == 1)) {
				cpu->regs.wordregs[regax] = cpu->regs.wordregs[regax] - 6;
				cpu->regs.byteregs[regah] = cpu->regs.byteregs[regah] - 1;
				cpu->af = 1;
				cpu->cf = 1;
			}
			else {
				cpu->af = 0;
				cpu->cf = 0;
			}

			cpu->regs.byteregs[regal] = cpu->regs.byteregs[regal] & 0xF;
			break;

		case 0x40:	/* 40 INC eAX */
			cpu->oldcf = cpu->cf;
			cpu->oper1 = cpu->regs.wordregs[regax];
			cpu->oper2 = 1;
			op_add16(cpu);
			cpu->cf = cpu->oldcf;
			cpu->regs.wordregs[regax] = cpu->res16;
			break;

		case 0x41:	/* 41 INC eCX */
			cpu->oldcf = cpu->cf;
			cpu->oper1 = cpu->regs.wordregs[regcx];
			cpu->oper2 = 1;
			op_add16(cpu);
			cpu->cf = cpu->oldcf;
			cpu->regs.wordregs[regcx] = cpu->res16;
			break;

		case 0x42:	/* 42 INC eDX */
			cpu->oldcf = cpu->cf;
			cpu->oper1 = cpu->regs.wordregs[regdx];
			cpu->oper2 = 1;
			op_add16(cpu);
			cpu->cf = cpu->oldcf;
			cpu->regs.wordregs[regdx] = cpu->res16;
			break;

		case 0x43:	/* 43 INC eBX */
			cpu->oldcf = cpu->cf;
			cpu->oper1 = cpu->regs.wordregs[regbx];
			cpu->oper2 = 1;
			op_add16(cpu);
			cpu->cf = cpu->oldcf;
			cpu->regs.wordregs[regbx] = cpu->res16;
			break;

		case 0x44:	/* 44 INC eSP */
			cpu->oldcf = cpu->cf;
			cpu->oper1 = cpu->regs.wordregs[regsp];
			cpu->oper2 = 1;
			op_add16(cpu);
			cpu->cf = cpu->oldcf;
			cpu->regs.wordregs[regsp] = cpu->res16;
			break;

		case 0x45:	/* 45 INC eBP */
			cpu->oldcf = cpu->cf;
			cpu->oper1 = cpu->regs.wordregs[regbp];
			cpu->oper2 = 1;
			op_add16(cpu);
			cpu->cf = cpu->oldcf;
			cpu->regs.wordregs[regbp] = cpu->res16;
			break;

		case 0x46:	/* 46 INC eSI */
			cpu->oldcf = cpu->cf;
			cpu->oper1 = cpu->regs.wordregs[regsi];
			cpu->oper2 = 1;
			op_add16(cpu);
			cpu->cf = cpu->oldcf;
			cpu->regs.wordregs[regsi] = cpu->res16;
			break;

		case 0x47:	/* 47 INC eDI */
			cpu->oldcf = cpu->cf;
			cpu->oper1 = cpu->regs.wordregs[regdi];
			cpu->oper2 = 1;
			op_add16(cpu);
			cpu->cf = cpu->oldcf;
			cpu->regs.wordregs[regdi] = cpu->res16;
			break;

		case 0x48:	/* 48 DEC eAX */
			cpu->oldcf = cpu->cf;
			cpu->oper1 = cpu->regs.wordregs[regax];
			cpu->oper2 = 1;
			op_sub16(cpu);
			cpu->cf = cpu->oldcf;
			cpu->regs.wordregs[regax] = cpu->res16;
			break;

		case 0x49:	/* 49 DEC eCX */
			cpu->oldcf = cpu->cf;
			cpu->oper1 = cpu->regs.wordregs[regcx];
			cpu->oper2 = 1;
			op_sub16(cpu);
			cpu->cf = cpu->oldcf;
			cpu->regs.wordregs[regcx] = cpu->res16;
			break;

		case 0x4A:	/* 4A DEC eDX */
			cpu->oldcf = cpu->cf;
			cpu->oper1 = cpu->regs.wordregs[regdx];
			cpu->oper2 = 1;
			op_sub16(cpu);
			cpu->cf = cpu->oldcf;
			cpu->regs.wordregs[regdx] = cpu->res16;
			break;

		case 0x4B:	/* 4B DEC eBX */
			cpu->oldcf = cpu->cf;
			cpu->oper1 = cpu->regs.wordregs[regbx];
			cpu->oper2 = 1;
			op_sub16(cpu);
			cpu->cf = cpu->oldcf;
			cpu->regs.wordregs[regbx] = cpu->res16;
			break;

		case 0x4C:	/* 4C DEC eSP */
			cpu->oldcf = cpu->cf;
			cpu->oper1 = cpu->regs.wordregs[regsp];
			cpu->oper2 = 1;
			op_sub16(cpu);
			cpu->cf = cpu->oldcf;
			cpu->regs.wordregs[regsp] = cpu->res16;
			break;

		case 0x4D:	/* 4D DEC eBP */
			cpu->oldcf = cpu->cf;
			cpu->oper1 = cpu->regs.wordregs[regbp];
			cpu->oper2 = 1;
			op_sub16(cpu);
			cpu->cf = cpu->oldcf;
			cpu->regs.wordregs[regbp] = cpu->res16;
			break;

		case 0x4E:	/* 4E DEC eSI */
			cpu->oldcf = cpu->cf;
			cpu->oper1 = cpu->regs.wordregs[regsi];
			cpu->oper2 = 1;
			op_sub16(cpu);
			cpu->cf = cpu->oldcf;
			cpu->regs.wordregs[regsi] = cpu->res16;
			break;

		case 0x4F:	/* 4F DEC eDI */
			cpu->oldcf = cpu->cf;
			cpu->oper1 = cpu->regs.wordregs[regdi];
			cpu->oper2 = 1;
			op_sub16(cpu);
			cpu->cf = cpu->oldcf;
			cpu->regs.wordregs[regdi] = cpu->res16;
			break;

		case 0x50:	/* 50 PUSH eAX */
			push(cpu, cpu->regs.wordregs[regax]);
			break;

		case 0x51:	/* 51 PUSH eCX */
			push(cpu, cpu->regs.wordregs[regcx]);
			break;

		case 0x52:	/* 52 PUSH eDX */
			push(cpu, cpu->regs.wordregs[regdx]);
			break;

		case 0x53:	/* 53 PUSH eBX */
			push(cpu, cpu->regs.wordregs[regbx]);
			break;

		case 0x54:	/* 54 PUSH eSP */
			push(cpu, cpu->regs.wordregs[regsp]);
			break;

		case 0x55:	/* 55 PUSH eBP */
			push(cpu, cpu->regs.wordregs[regbp]);
			break;

		case 0x56:	/* 56 PUSH eSI */
			push(cpu, cpu->regs.wordregs[regsi]);
			break;

		case 0x57:	/* 57 PUSH eDI */
			push(cpu, cpu->regs.wordregs[regdi]);
			break;

		case 0x58:	/* 58 POP eAX */
			cpu->regs.wordregs[regax] = pop(cpu);
			break;

		case 0x59:	/* 59 POP eCX */
			cpu->regs.wordregs[regcx] = pop(cpu);
			break;

		case 0x5A:	/* 5A POP eDX */
			cpu->regs.wordregs[regdx] = pop(cpu);
			break;

		case 0x5B:	/* 5B POP eBX */
			cpu->regs.wordregs[regbx] = pop(cpu);
			break;

		case 0x5C:	/* 5C POP eSP */
			cpu->regs.wordregs[regsp] = pop(cpu);
			break;

		case 0x5D:	/* 5D POP eBP */
			cpu->regs.wordregs[regbp] = pop(cpu);
			break;

		case 0x5E:	/* 5E POP eSI */
			cpu->regs.wordregs[regsi] = pop(cpu);
			break;

		case 0x5F:	/* 5F POP eDI */
			cpu->regs.wordregs[regdi] = pop(cpu);
			break;

		case 0x60:	/* 60 PUSHA (80186+) */
			cpu->oldsp = cpu->regs.wordregs[regsp];
			push(cpu, cpu->regs.wordregs[regax]);
			push(cpu, cpu->regs.wordregs[regcx]);
			push(cpu, cpu->regs.wordregs[regdx]);
			push(cpu, cpu->regs.wordregs[regbx]);
			push(cpu, cpu->oldsp);
			push(cpu, cpu->regs.wordregs[regbp]);
			push(cpu, cpu->regs.wordregs[regsi]);
			push(cpu, cpu->regs.wordregs[regdi]);
			break;

		case 0x61:	/* 61 POPA (80186+) */
			cpu->regs.wordregs[regdi] = pop(cpu);
			cpu->regs.wordregs[regsi] = pop(cpu);
			cpu->regs.wordregs[regbp] = pop(cpu);
			cpu->regs.wordregs[regsp] += 2;
			cpu->regs.wordregs[regbx] = pop(cpu);
			cpu->regs.wordregs[regdx] = pop(cpu);
			cpu->regs.wordregs[regcx] = pop(cpu);
			cpu->regs.wordregs[regax] = pop(cpu);
			break;

		case 0x62: /* 62 BOUND Gv, Ev (80186+) */
			modregrm(cpu);
			getea(cpu, cpu->rm);
			if (signext32(getreg16(cpu, cpu->reg)) < signext32(getmem16(cpu, cpu->ea >> 4, cpu->ea & 15))) {
				cpu_intcall(cpu, 5); //bounds check exception
			}
			else {
				cpu->ea += 2;
				if (signext32(getreg16(cpu, cpu->reg)) > signext32(getmem16(cpu, cpu->ea >> 4, cpu->ea & 15))) {
					cpu_intcall(cpu, 5); //bounds check exception
				}
			}
			break;

		case 0x63:	/* ARPL Ew, Gw */
			debug_log(DEBUG_INFO, "[CPU] 286 Opcode: ARPL (63h)\n");
			if (!cpu->protected_mode) {
				cpu_intcall(cpu, 6);
			}
			else {
				modregrm(cpu);
				cpu->oper1 = readrm16(cpu, cpu->rm);
				cpu->oper2 = getreg16(cpu, cpu->reg);

				if ((cpu->oper2 & 0xFFFC) == 0) {
					cpu_intcall(cpu, 13);
				}
				else if ((cpu->oper1 & 3) < (cpu->oper2 & 3)) {
					cpu->zf = 1;
					cpu->oper1 = (cpu->oper1 & 0xFFFC) | (cpu->oper2 & 3);
					writerm16(cpu, cpu->rm, cpu->oper1);
				}
				else {
					cpu->zf = 0;
				}
			}
			break;

		case 0x68:	/* 68 PUSH Iv (80186+) */
			push(cpu, getmem16(cpu, cpu->segregs[regcs], cpu->ip));
			StepIP(cpu, 2);
			break;

		case 0x69:	/* 69 IMUL Gv Ev Iv (80186+) */
			modregrm(cpu);
			cpu->temp1 = readrm16(cpu, cpu->rm);
			cpu->temp2 = getmem16(cpu, cpu->segregs[regcs], cpu->ip);
			StepIP(cpu, 2);
			if ((cpu->temp1 & 0x8000L) == 0x8000L) {
				cpu->temp1 = cpu->temp1 | 0xFFFF0000L;
			}

			if ((cpu->temp2 & 0x8000L) == 0x8000L) {
				cpu->temp2 = cpu->temp2 | 0xFFFF0000L;
			}

			cpu->temp3 = (uint32_t)((int32_t)cpu->temp1 * (int32_t)cpu->temp2);
			putreg16(cpu, cpu->reg, cpu->temp3 & 0xFFFFL);
			if (cpu->temp3 & 0xFFFF0000L) {
				cpu->cf = 1;
				cpu->of = 1;
			}
			else {
				cpu->cf = 0;
				cpu->of = 0;
			}
			break;

		case 0x6A:	/* 6A PUSH Ib (80186+) */
			push(cpu, (uint16_t)signext(getmem8(cpu, cpu->segregs[regcs], cpu->ip)));
			StepIP(cpu, 1);
			break;

		case 0x6B:	/* 6B IMUL Gv Eb Ib (80186+) */
			modregrm(cpu);
			cpu->temp1 = readrm16(cpu, cpu->rm);
			cpu->temp2 = signext(getmem8(cpu, cpu->segregs[regcs], cpu->ip));
			StepIP(cpu, 1);
			if ((cpu->temp1 & 0x8000L) == 0x8000L) {
				cpu->temp1 = cpu->temp1 | 0xFFFF0000L;
			}

			if ((cpu->temp2 & 0x8000L) == 0x8000L) {
				cpu->temp2 = cpu->temp2 | 0xFFFF0000L;
			}

			cpu->temp3 = (uint32_t)((int32_t)cpu->temp1 * (int32_t)cpu->temp2);
			putreg16(cpu, cpu->reg, cpu->temp3 & 0xFFFFL);
			if (cpu->temp3 & 0xFFFF0000L) {
				cpu->cf = 1;
				cpu->of = 1;
			}
			else {
				cpu->cf = 0;
				cpu->of = 0;
			}
			break;

		case 0x6C:	/* 6E INSB */
			if (cpu->reptype && (cpu->regs.wordregs[regcx] == 0)) {
				break;
			}

			putmem8(cpu, cpu->segregs[reges], cpu->regs.wordregs[regdi], port_read(cpu, cpu->regs.wordregs[regdx]));
			if (cpu->df) {
				cpu->regs.wordregs[regsi] = cpu->regs.wordregs[regsi] - 1;
				cpu->regs.wordregs[regdi] = cpu->regs.wordregs[regdi] - 1;
			}
			else {
				cpu->regs.wordregs[regsi] = cpu->regs.wordregs[regsi] + 1;
				cpu->regs.wordregs[regdi] = cpu->regs.wordregs[regdi] + 1;
			}

			if (cpu->reptype) {
				cpu->regs.wordregs[regcx] = cpu->regs.wordregs[regcx] - 1;
			}

			loopcount++;
			if (!cpu->reptype) {
				break;
			}

			cpu->ip = firstip;
			break;

		case 0x6D:	/* 6F INSW */
			if (cpu->reptype && (cpu->regs.wordregs[regcx] == 0)) {
				break;
			}

			putmem16(cpu, cpu->segregs[reges], cpu->regs.wordregs[regdi], port_readw(cpu, cpu->regs.wordregs[regdx]));
			if (cpu->df) {
				cpu->regs.wordregs[regsi] = cpu->regs.wordregs[regsi] - 2;
				cpu->regs.wordregs[regdi] = cpu->regs.wordregs[regdi] - 2;
			}
			else {
				cpu->regs.wordregs[regsi] = cpu->regs.wordregs[regsi] + 2;
				cpu->regs.wordregs[regdi] = cpu->regs.wordregs[regdi] + 2;
			}

			if (cpu->reptype) {
				cpu->regs.wordregs[regcx] = cpu->regs.wordregs[regcx] - 1;
			}

			loopcount++;
			if (!cpu->reptype) {
				break;
			}

			cpu->ip = firstip;
			break;

		case 0x6E:	/* 6E OUTSB */
			if (cpu->reptype && (cpu->regs.wordregs[regcx] == 0)) {
				break;
			}

			port_write(cpu, cpu->regs.wordregs[regdx], getmem8(cpu, cpu->useseg, cpu->regs.wordregs[regsi]));
			if (cpu->df) {
				cpu->regs.wordregs[regsi] = cpu->regs.wordregs[regsi] - 1;
				cpu->regs.wordregs[regdi] = cpu->regs.wordregs[regdi] - 1;
			}
			else {
				cpu->regs.wordregs[regsi] = cpu->regs.wordregs[regsi] + 1;
				cpu->regs.wordregs[regdi] = cpu->regs.wordregs[regdi] + 1;
			}

			if (cpu->reptype) {
				cpu->regs.wordregs[regcx] = cpu->regs.wordregs[regcx] - 1;
			}

			loopcount++;
			if (!cpu->reptype) {
				break;
			}

			cpu->ip = firstip;
			break;

		case 0x6F:	/* 6F OUTSW */
			if (cpu->reptype && (cpu->regs.wordregs[regcx] == 0)) {
				break;
			}

			port_writew(cpu, cpu->regs.wordregs[regdx], getmem16(cpu, cpu->useseg, cpu->regs.wordregs[regsi]));
			if (cpu->df) {
				cpu->regs.wordregs[regsi] = cpu->regs.wordregs[regsi] - 2;
				cpu->regs.wordregs[regdi] = cpu->regs.wordregs[regdi] - 2;
			}
			else {
				cpu->regs.wordregs[regsi] = cpu->regs.wordregs[regsi] + 2;
				cpu->regs.wordregs[regdi] = cpu->regs.wordregs[regdi] + 2;
			}

			if (cpu->reptype) {
				cpu->regs.wordregs[regcx] = cpu->regs.wordregs[regcx] - 1;
			}

			loopcount++;
			if (!cpu->reptype) {
				break;
			}

			cpu->ip = firstip;
			break;

		case 0x70:	/* 70 JO Jb */
			cpu->temp16 = signext(getmem8(cpu, cpu->segregs[regcs], cpu->ip));
			StepIP(cpu, 1);
			if (cpu->of) {
				cpu->ip = cpu->ip + cpu->temp16;
			}
			break;

		case 0x71:	/* 71 JNO Jb */
			cpu->temp16 = signext(getmem8(cpu, cpu->segregs[regcs], cpu->ip));
			StepIP(cpu, 1);
			if (!cpu->of) {
				cpu->ip = cpu->ip + cpu->temp16;
			}
			break;

		case 0x72:	/* 72 JB Jb */
			cpu->temp16 = signext(getmem8(cpu, cpu->segregs[regcs], cpu->ip));
			StepIP(cpu, 1);
			if (cpu->cf) {
				cpu->ip = cpu->ip + cpu->temp16;
			}
			break;

		case 0x73:	/* 73 JNB Jb */
			cpu->temp16 = signext(getmem8(cpu, cpu->segregs[regcs], cpu->ip));
			StepIP(cpu, 1);
			if (!cpu->cf) {
				cpu->ip = cpu->ip + cpu->temp16;
			}
			break;

		case 0x74:	/* 74 JZ Jb */
			cpu->temp16 = signext(getmem8(cpu, cpu->segregs[regcs], cpu->ip));
			StepIP(cpu, 1);
			if (cpu->zf) {
				cpu->ip = cpu->ip + cpu->temp16;
			}
			break;

		case 0x75:	/* 75 JNZ Jb */
			cpu->temp16 = signext(getmem8(cpu, cpu->segregs[regcs], cpu->ip));
			StepIP(cpu, 1);
			if (!cpu->zf) {
				cpu->ip = cpu->ip + cpu->temp16;
			}
			break;

		case 0x76:	/* 76 JBE Jb */
			cpu->temp16 = signext(getmem8(cpu, cpu->segregs[regcs], cpu->ip));
			StepIP(cpu, 1);
			if (cpu->cf || cpu->zf) {
				cpu->ip = cpu->ip + cpu->temp16;
			}
			break;

		case 0x77:	/* 77 JA Jb */
			cpu->temp16 = signext(getmem8(cpu, cpu->segregs[regcs], cpu->ip));
			StepIP(cpu, 1);
			if (!cpu->cf && !cpu->zf) {
				cpu->ip = cpu->ip + cpu->temp16;
			}
			break;

		case 0x78:	/* 78 JS Jb */
			cpu->temp16 = signext(getmem8(cpu, cpu->segregs[regcs], cpu->ip));
			StepIP(cpu, 1);
			if (cpu->sf) {
				cpu->ip = cpu->ip + cpu->temp16;
			}
			break;

		case 0x79:	/* 79 JNS Jb */
			cpu->temp16 = signext(getmem8(cpu, cpu->segregs[regcs], cpu->ip));
			StepIP(cpu, 1);
			if (!cpu->sf) {
				cpu->ip = cpu->ip + cpu->temp16;
			}
			break;

		case 0x7A:	/* 7A JPE Jb */
			cpu->temp16 = signext(getmem8(cpu, cpu->segregs[regcs], cpu->ip));
			StepIP(cpu, 1);
			if (cpu->pf) {
				cpu->ip = cpu->ip + cpu->temp16;
			}
			break;

		case 0x7B:	/* 7B JPO Jb */
			cpu->temp16 = signext(getmem8(cpu, cpu->segregs[regcs], cpu->ip));
			StepIP(cpu, 1);
			if (!cpu->pf) {
				cpu->ip = cpu->ip + cpu->temp16;
			}
			break;

		case 0x7C:	/* 7C JL Jb */
			cpu->temp16 = signext(getmem8(cpu, cpu->segregs[regcs], cpu->ip));
			StepIP(cpu, 1);
			if (cpu->sf != cpu->of) {
				cpu->ip = cpu->ip + cpu->temp16;
			}
			break;

		case 0x7D:	/* 7D JGE Jb */
			cpu->temp16 = signext(getmem8(cpu, cpu->segregs[regcs], cpu->ip));
			StepIP(cpu, 1);
			if (cpu->sf == cpu->of) {
				cpu->ip = cpu->ip + cpu->temp16;
			}
			break;

		case 0x7E:	/* 7E JLE Jb */
			cpu->temp16 = signext(getmem8(cpu, cpu->segregs[regcs], cpu->ip));
			StepIP(cpu, 1);
			if ((cpu->sf != cpu->of) || cpu->zf) {
				cpu->ip = cpu->ip + cpu->temp16;
			}
			break;

		case 0x7F:	/* 7F JG Jb */
			cpu->temp16 = signext(getmem8(cpu, cpu->segregs[regcs], cpu->ip));
			StepIP(cpu, 1);
			if (!cpu->zf && (cpu->sf == cpu->of)) {
				cpu->ip = cpu->ip + cpu->temp16;
			}
			break;

		case 0x80:
		case 0x82:	/* 80/82 GRP1 Eb Ib */
			modregrm(cpu);
			cpu->oper1b = readrm8(cpu, cpu->rm);
			cpu->oper2b = getmem8(cpu, cpu->segregs[regcs], cpu->ip);
			StepIP(cpu, 1);
			switch (cpu->reg) {
			case 0:
				op_add8(cpu);
				break;
			case 1:
				op_or8(cpu);
				break;
			case 2:
				op_adc8(cpu);
				break;
			case 3:
				op_sbb8(cpu);
				break;
			case 4:
				op_and8(cpu);
				break;
			case 5:
				op_sub8(cpu);
				break;
			case 6:
				op_xor8(cpu);
				break;
			case 7:
				flag_sub8(cpu, cpu->oper1b, cpu->oper2b);
				break;
			default:
				break;	/* to avoid compiler warnings */
			}

			if (cpu->reg < 7) {
				writerm8(cpu, cpu->rm, cpu->res8);
			}
			break;

		case 0x81:	/* 81 GRP1 Ev Iv */
		case 0x83:	/* 83 GRP1 Ev Ib */
			modregrm(cpu);
			cpu->oper1 = readrm16(cpu, cpu->rm);
			if (cpu->opcode == 0x81) {
				cpu->oper2 = getmem16(cpu, cpu->segregs[regcs], cpu->ip);
				StepIP(cpu, 2);
			}
			else {
				cpu->oper2 = signext(getmem8(cpu, cpu->segregs[regcs], cpu->ip));
				StepIP(cpu, 1);
			}

			switch (cpu->reg) {
			case 0:
				op_add16(cpu);
				break;
			case 1:
				op_or16(cpu);
				break;
			case 2:
				op_adc16(cpu);
				break;
			case 3:
				op_sbb16(cpu);
				break;
			case 4:
				op_and16(cpu);
				break;
			case 5:
				op_sub16(cpu);
				break;
			case 6:
				op_xor16(cpu);
				break;
			case 7:
				flag_sub16(cpu, cpu->oper1, cpu->oper2);
				break;
			default:
				break;	/* to avoid compiler warnings */
			}

			if (cpu->reg < 7) {
				writerm16(cpu, cpu->rm, cpu->res16);
			}
			break;

		case 0x84:	/* 84 TEST Gb Eb */
			modregrm(cpu);
			cpu->oper1b = getreg8(cpu, cpu->reg);
			cpu->oper2b = readrm8(cpu, cpu->rm);
			flag_log8(cpu, cpu->oper1b & cpu->oper2b);
			break;

		case 0x85:	/* 85 TEST Gv Ev */
			modregrm(cpu);
			cpu->oper1 = getreg16(cpu, cpu->reg);
			cpu->oper2 = readrm16(cpu, cpu->rm);
			flag_log16(cpu, cpu->oper1 & cpu->oper2);
			break;

		case 0x86:	/* 86 XCHG Gb Eb */
			modregrm(cpu);
			cpu->oper1b = getreg8(cpu, cpu->reg);
			putreg8(cpu, cpu->reg, readrm8(cpu, cpu->rm));
			writerm8(cpu, cpu->rm, cpu->oper1b);
			break;

		case 0x87:	/* 87 XCHG Gv Ev */
			modregrm(cpu);
			cpu->oper1 = getreg16(cpu, cpu->reg);
			putreg16(cpu, cpu->reg, readrm16(cpu, cpu->rm));
			writerm16(cpu, cpu->rm, cpu->oper1);
			break;

		case 0x88:	/* 88 MOV Eb Gb */
			modregrm(cpu);
			writerm8(cpu, cpu->rm, getreg8(cpu, cpu->reg));
			break;

		case 0x89:	/* 89 MOV Ev Gv */
			modregrm(cpu);
			writerm16(cpu, cpu->rm, getreg16(cpu, cpu->reg));
			break;

		case 0x8A:	/* 8A MOV Gb Eb */
			modregrm(cpu);
			putreg8(cpu, cpu->reg, readrm8(cpu, cpu->rm));
			break;

		case 0x8B:	/* 8B MOV Gv Ev */
			modregrm(cpu);
			putreg16(cpu, cpu->reg, readrm16(cpu, cpu->rm));
			break;

		case 0x8C:	/* 8C MOV Ew Sw */
			modregrm(cpu);
			writerm16(cpu, cpu->rm, getsegreg(cpu, cpu->reg));
			break;

		case 0x8D:	/* 8D LEA Gv M */
			modregrm(cpu);
			getea(cpu, cpu->rm);
			putreg16(cpu, cpu->reg, cpu->ea - segbase(cpu->useseg));
			break;

		case 0x8E:	/* 8E MOV Sw Ew */
			modregrm(cpu);
			cpu->oper1 = readrm16(cpu, cpu->rm);
			if (cpu->protected_mode) {
				load_descriptor(cpu, cpu->reg, cpu->oper1);
			}
			putsegreg(cpu, cpu->reg, cpu->oper1);
			break;

		case 0x8F:	/* 8F POP Ev */
			modregrm(cpu);
			writerm16(cpu, cpu->rm, pop(cpu));
			break;

		case 0x90:	/* 90 NOP */
			break;

		case 0x91:	/* 91 XCHG eCX eAX */
			cpu->oper1 = cpu->regs.wordregs[regcx];
			cpu->regs.wordregs[regcx] = cpu->regs.wordregs[regax];
			cpu->regs.wordregs[regax] = cpu->oper1;
			break;

		case 0x92:	/* 92 XCHG eDX eAX */
			cpu->oper1 = cpu->regs.wordregs[regdx];
			cpu->regs.wordregs[regdx] = cpu->regs.wordregs[regax];
			cpu->regs.wordregs[regax] = cpu->oper1;
			break;

		case 0x93:	/* 93 XCHG eBX eAX */
			cpu->oper1 = cpu->regs.wordregs[regbx];
			cpu->regs.wordregs[regbx] = cpu->regs.wordregs[regax];
			cpu->regs.wordregs[regax] = cpu->oper1;
			break;

		case 0x94:	/* 94 XCHG eSP eAX */
			cpu->oper1 = cpu->regs.wordregs[regsp];
			cpu->regs.wordregs[regsp] = cpu->regs.wordregs[regax];
			cpu->regs.wordregs[regax] = cpu->oper1;
			break;

		case 0x95:	/* 95 XCHG eBP eAX */
			cpu->oper1 = cpu->regs.wordregs[regbp];
			cpu->regs.wordregs[regbp] = cpu->regs.wordregs[regax];
			cpu->regs.wordregs[regax] = cpu->oper1;
			break;

		case 0x96:	/* 96 XCHG eSI eAX */
			cpu->oper1 = cpu->regs.wordregs[regsi];
			cpu->regs.wordregs[regsi] = cpu->regs.wordregs[regax];
			cpu->regs.wordregs[regax] = cpu->oper1;
			break;

		case 0x97:	/* 97 XCHG eDI eAX */
			cpu->oper1 = cpu->regs.wordregs[regdi];
			cpu->regs.wordregs[regdi] = cpu->regs.wordregs[regax];
			cpu->regs.wordregs[regax] = cpu->oper1;
			break;

		case 0x98:	/* 98 CBW */
			if ((cpu->regs.byteregs[regal] & 0x80) == 0x80) {
				cpu->regs.byteregs[regah] = 0xFF;
			}
			else {
				cpu->regs.byteregs[regah] = 0;
			}
			break;

		case 0x99:	/* 99 CWD */
			if ((cpu->regs.byteregs[regah] & 0x80) == 0x80) {
				cpu->regs.wordregs[regdx] = 0xFFFF;
			}
			else {
				cpu->regs.wordregs[regdx] = 0;
			}
			break;

		case 0x9A:	/* 9A CALL Ap */
			cpu->oper1 = getmem16(cpu, cpu->segregs[regcs], cpu->ip);
			StepIP(cpu, 2);
			cpu->oper2 = getmem16(cpu, cpu->segregs[regcs], cpu->ip);
			StepIP(cpu, 2);
			push(cpu, cpu->segregs[regcs]);
			push(cpu, cpu->ip);
			cpu->ip = cpu->oper1;
			cpu->segregs[regcs] = cpu->oper2;
			if (cpu->protected_mode) {
				load_descriptor(cpu, regcs, cpu->segregs[regcs]);
			}
			break;

		case 0x9B:	/* 9B WAIT */
			break;

		case 0x9C:	/* 9C PUSHF */
			if (cpu->protected_mode) {
				push(cpu, makeflagsword(cpu));
			}
			else {
				push(cpu, makeflagsword(cpu) & 0x0FFF);
			}
			break;

		case 0x9D:    /* 9D POPF */
		{
			uint16_t new_flags = pop(cpu);
			uint16_t old_flags = makeflagsword(cpu);
			uint8_t cpl = cpu->segregs[regcs] & 3;
			uint8_t iopl = (old_flags >> 12) & 3;

			if (cpu->protected_mode) {
				if (cpl > iopl) {
					if (new_flags & 0x0200) {
						old_flags |= 0x0200;
					}
					else {
						old_flags &= ~0x0200;
					}
					new_flags = (new_flags & ~0x0200) | (old_flags & 0x0200);
				}

				if (cpl != 0) {
					new_flags = (new_flags & ~0x3000) | (old_flags & 0x3000);
				}

				new_flags &= 0x72FF;
				new_flags |= 0x0002;
			}
			else {
				new_flags &= 0x72FF;
				new_flags |= 0xF002;
			}

			decodeflagsword(cpu, new_flags);
			break;
		}
		case 0x9E:	/* 9E SAHF */
			decodeflagsword(cpu, (makeflagsword(cpu) & 0xFF00) | cpu->regs.byteregs[regah]);
			break;

		case 0x9F:	/* 9F LAHF */
			cpu->regs.byteregs[regah] = makeflagsword(cpu) & 0xFF;
			break;

		case 0xA0:	/* A0 MOV cpu->regs.byteregs[regal] Ob */
			cpu->regs.byteregs[regal] = getmem8(cpu, cpu->useseg, getmem16(cpu, cpu->segregs[regcs], cpu->ip));
			StepIP(cpu, 2);
			break;

		case 0xA1:	/* A1 MOV eAX Ov */
			cpu->oper1 = getmem16(cpu, cpu->useseg, getmem16(cpu, cpu->segregs[regcs], cpu->ip));
			StepIP(cpu, 2);
			cpu->regs.wordregs[regax] = cpu->oper1;
			break;

		case 0xA2:	/* A2 MOV Ob cpu->regs.byteregs[regal] */
			putmem8(cpu, cpu->useseg, getmem16(cpu, cpu->segregs[regcs], cpu->ip), cpu->regs.byteregs[regal]);
			StepIP(cpu, 2);
			break;

		case 0xA3:	/* A3 MOV Ov eAX */
			putmem16(cpu, cpu->useseg, getmem16(cpu, cpu->segregs[regcs], cpu->ip), cpu->regs.wordregs[regax]);
			StepIP(cpu, 2);
			break;

		case 0xA4:	/* A4 MOVSB */
			if (cpu->reptype && (cpu->regs.wordregs[regcx] == 0)) {
				break;
			}

			putmem8(cpu, cpu->segregs[reges], cpu->regs.wordregs[regdi], getmem8(cpu, cpu->useseg, cpu->regs.wordregs[regsi]));
			if (cpu->df) {
				cpu->regs.wordregs[regsi] = cpu->regs.wordregs[regsi] - 1;
				cpu->regs.wordregs[regdi] = cpu->regs.wordregs[regdi] - 1;
			}
			else {
				cpu->regs.wordregs[regsi] = cpu->regs.wordregs[regsi] + 1;
				cpu->regs.wordregs[regdi] = cpu->regs.wordregs[regdi] + 1;
			}

			if (cpu->reptype) {
				cpu->regs.wordregs[regcx] = cpu->regs.wordregs[regcx] - 1;
			}

			loopcount++;
			if (!cpu->reptype) {
				break;
			}

			cpu->ip = firstip;
			break;

		case 0xA5:	/* A5 MOVSW */
			if (cpu->reptype && (cpu->regs.wordregs[regcx] == 0)) {
				break;
			}

			putmem16(cpu, cpu->segregs[reges], cpu->regs.wordregs[regdi], getmem16(cpu, cpu->useseg, cpu->regs.wordregs[regsi]));
			if (cpu->df) {
				cpu->regs.wordregs[regsi] = cpu->regs.wordregs[regsi] - 2;
				cpu->regs.wordregs[regdi] = cpu->regs.wordregs[regdi] - 2;
			}
			else {
				cpu->regs.wordregs[regsi] = cpu->regs.wordregs[regsi] + 2;
				cpu->regs.wordregs[regdi] = cpu->regs.wordregs[regdi] + 2;
			}

			if (cpu->reptype) {
				cpu->regs.wordregs[regcx] = cpu->regs.wordregs[regcx] - 1;
			}

			loopcount++;
			if (!cpu->reptype) {
				break;
			}

			cpu->ip = firstip;
			break;

		case 0xA6:	/* A6 CMPSB */
			if (cpu->reptype && (cpu->regs.wordregs[regcx] == 0)) {
				break;
			}

			cpu->oper1b = getmem8(cpu, cpu->useseg, cpu->regs.wordregs[regsi]);
			cpu->oper2b = getmem8(cpu, cpu->segregs[reges], cpu->regs.wordregs[regdi]);
			if (cpu->df) {
				cpu->regs.wordregs[regsi] = cpu->regs.wordregs[regsi] - 1;
				cpu->regs.wordregs[regdi] = cpu->regs.wordregs[regdi] - 1;
			}
			else {
				cpu->regs.wordregs[regsi] = cpu->regs.wordregs[regsi] + 1;
				cpu->regs.wordregs[regdi] = cpu->regs.wordregs[regdi] + 1;
			}

			flag_sub8(cpu, cpu->oper1b, cpu->oper2b);
			if (cpu->reptype) {
				cpu->regs.wordregs[regcx] = cpu->regs.wordregs[regcx] - 1;
			}

			if ((cpu->reptype == 1) && !cpu->zf) {
				break;
			}
			else if ((cpu->reptype == 2) && (cpu->zf == 1)) {
				break;
			}

			loopcount++;
			if (!cpu->reptype) {
				break;
			}

			cpu->ip = firstip;
			break;

		case 0xA7:	/* A7 CMPSW */
			if (cpu->reptype && (cpu->regs.wordregs[regcx] == 0)) {
				break;
			}

			cpu->oper1 = getmem16(cpu, cpu->useseg, cpu->regs.wordregs[regsi]);
			cpu->oper2 = getmem16(cpu, cpu->segregs[reges], cpu->regs.wordregs[regdi]);
			if (cpu->df) {
				cpu->regs.wordregs[regsi] = cpu->regs.wordregs[regsi] - 2;
				cpu->regs.wordregs[regdi] = cpu->regs.wordregs[regdi] - 2;
			}
			else {
				cpu->regs.wordregs[regsi] = cpu->regs.wordregs[regsi] + 2;
				cpu->regs.wordregs[regdi] = cpu->regs.wordregs[regdi] + 2;
			}

			flag_sub16(cpu, cpu->oper1, cpu->oper2);
			if (cpu->reptype) {
				cpu->regs.wordregs[regcx] = cpu->regs.wordregs[regcx] - 1;
			}

			if ((cpu->reptype == 1) && !cpu->zf) {
				break;
			}

			if ((cpu->reptype == 2) && (cpu->zf == 1)) {
				break;
			}

			loopcount++;
			if (!cpu->reptype) {
				break;
			}

			cpu->ip = firstip;
			break;

		case 0xA8:	/* A8 TEST cpu->regs.byteregs[regal] Ib */
			cpu->oper1b = cpu->regs.byteregs[regal];
			cpu->oper2b = getmem8(cpu, cpu->segregs[regcs], cpu->ip);
			StepIP(cpu, 1);
			flag_log8(cpu, cpu->oper1b & cpu->oper2b);
			break;

		case 0xA9:	/* A9 TEST eAX Iv */
			cpu->oper1 = cpu->regs.wordregs[regax];
			cpu->oper2 = getmem16(cpu, cpu->segregs[regcs], cpu->ip);
			StepIP(cpu, 2);
			flag_log16(cpu, cpu->oper1 & cpu->oper2);
			break;

		case 0xAA:	/* AA STOSB */
			if (cpu->reptype && (cpu->regs.wordregs[regcx] == 0)) {
				break;
			}

			putmem8(cpu, cpu->segregs[reges], cpu->regs.wordregs[regdi], cpu->regs.byteregs[regal]);
			if (cpu->df) {
				cpu->regs.wordregs[regdi] = cpu->regs.wordregs[regdi] - 1;
			}
			else {
				cpu->regs.wordregs[regdi] = cpu->regs.wordregs[regdi] + 1;
			}

			if (cpu->reptype) {
				cpu->regs.wordregs[regcx] = cpu->regs.wordregs[regcx] - 1;
			}

			loopcount++;
			if (!cpu->reptype) {
				break;
			}

			cpu->ip = firstip;
			break;

		case 0xAB:	/* AB STOSW */
			if (cpu->reptype && (cpu->regs.wordregs[regcx] == 0)) {
				break;
			}

			putmem16(cpu, cpu->segregs[reges], cpu->regs.wordregs[regdi], cpu->regs.wordregs[regax]);
			if (cpu->df) {
				cpu->regs.wordregs[regdi] = cpu->regs.wordregs[regdi] - 2;
			}
			else {
				cpu->regs.wordregs[regdi] = cpu->regs.wordregs[regdi] + 2;
			}

			if (cpu->reptype) {
				cpu->regs.wordregs[regcx] = cpu->regs.wordregs[regcx] - 1;
			}

			loopcount++;
			if (!cpu->reptype) {
				break;
			}

			cpu->ip = firstip;
			break;

		case 0xAC:	/* AC LODSB */
			if (cpu->reptype && (cpu->regs.wordregs[regcx] == 0)) {
				break;
			}

			cpu->regs.byteregs[regal] = getmem8(cpu, cpu->useseg, cpu->regs.wordregs[regsi]);
			if (cpu->df) {
				cpu->regs.wordregs[regsi] = cpu->regs.wordregs[regsi] - 1;
			}
			else {
				cpu->regs.wordregs[regsi] = cpu->regs.wordregs[regsi] + 1;
			}

			if (cpu->reptype) {
				cpu->regs.wordregs[regcx] = cpu->regs.wordregs[regcx] - 1;
			}

			loopcount++;
			if (!cpu->reptype) {
				break;
			}

			cpu->ip = firstip;
			break;

		case 0xAD:	/* AD LODSW */
			if (cpu->reptype && (cpu->regs.wordregs[regcx] == 0)) {
				break;
			}

			cpu->oper1 = getmem16(cpu, cpu->useseg, cpu->regs.wordregs[regsi]);
			cpu->regs.wordregs[regax] = cpu->oper1;
			if (cpu->df) {
				cpu->regs.wordregs[regsi] = cpu->regs.wordregs[regsi] - 2;
			}
			else {
				cpu->regs.wordregs[regsi] = cpu->regs.wordregs[regsi] + 2;
			}

			if (cpu->reptype) {
				cpu->regs.wordregs[regcx] = cpu->regs.wordregs[regcx] - 1;
			}

			loopcount++;
			if (!cpu->reptype) {
				break;
			}

			cpu->ip = firstip;
			break;

		case 0xAE:	/* AE SCASB */
			if (cpu->reptype && (cpu->regs.wordregs[regcx] == 0)) {
				break;
			}

			cpu->oper1b = cpu->regs.byteregs[regal];
			cpu->oper2b = getmem8(cpu, cpu->segregs[reges], cpu->regs.wordregs[regdi]);
			flag_sub8(cpu, cpu->oper1b, cpu->oper2b);
			if (cpu->df) {
				cpu->regs.wordregs[regdi] = cpu->regs.wordregs[regdi] - 1;
			}
			else {
				cpu->regs.wordregs[regdi] = cpu->regs.wordregs[regdi] + 1;
			}

			if (cpu->reptype) {
				cpu->regs.wordregs[regcx] = cpu->regs.wordregs[regcx] - 1;
			}

			if ((cpu->reptype == 1) && !cpu->zf) {
				break;
			}
			else if ((cpu->reptype == 2) && (cpu->zf == 1)) {
				break;
			}

			loopcount++;
			if (!cpu->reptype) {
				break;
			}

			cpu->ip = firstip;
			break;

		case 0xAF:	/* AF SCASW */
			if (cpu->reptype && (cpu->regs.wordregs[regcx] == 0)) {
				break;
			}

			cpu->oper1 = cpu->regs.wordregs[regax];
			cpu->oper2 = getmem16(cpu, cpu->segregs[reges], cpu->regs.wordregs[regdi]);
			flag_sub16(cpu, cpu->oper1, cpu->oper2);
			if (cpu->df) {
				cpu->regs.wordregs[regdi] = cpu->regs.wordregs[regdi] - 2;
			}
			else {
				cpu->regs.wordregs[regdi] = cpu->regs.wordregs[regdi] + 2;
			}

			if (cpu->reptype) {
				cpu->regs.wordregs[regcx] = cpu->regs.wordregs[regcx] - 1;
			}

			if ((cpu->reptype == 1) && !cpu->zf) {
				break;
			}
			else if ((cpu->reptype == 2) && (cpu->zf == 1)) { //did i fix a typo bug? this used to be & instead of &&
				break;
			}

			loopcount++;
			if (!cpu->reptype) {
				break;
			}

			cpu->ip = firstip;
			break;

		case 0xB0:	/* B0 MOV cpu->regs.byteregs[regal] Ib */
			cpu->regs.byteregs[regal] = getmem8(cpu, cpu->segregs[regcs], cpu->ip);
			StepIP(cpu, 1);
			break;

		case 0xB1:	/* B1 MOV cpu->regs.byteregs[regcl] Ib */
			cpu->regs.byteregs[regcl] = getmem8(cpu, cpu->segregs[regcs], cpu->ip);
			StepIP(cpu, 1);
			break;

		case 0xB2:	/* B2 MOV cpu->regs.byteregs[regdl] Ib */
			cpu->regs.byteregs[regdl] = getmem8(cpu, cpu->segregs[regcs], cpu->ip);
			StepIP(cpu, 1);
			break;

		case 0xB3:	/* B3 MOV cpu->regs.byteregs[regbl] Ib */
			cpu->regs.byteregs[regbl] = getmem8(cpu, cpu->segregs[regcs], cpu->ip);
			StepIP(cpu, 1);
			break;

		case 0xB4:	/* B4 MOV cpu->regs.byteregs[regah] Ib */
			cpu->regs.byteregs[regah] = getmem8(cpu, cpu->segregs[regcs], cpu->ip);
			StepIP(cpu, 1);
			break;

		case 0xB5:	/* B5 MOV cpu->regs.byteregs[regch] Ib */
			cpu->regs.byteregs[regch] = getmem8(cpu, cpu->segregs[regcs], cpu->ip);
			StepIP(cpu, 1);
			break;

		case 0xB6:	/* B6 MOV cpu->regs.byteregs[regdh] Ib */
			cpu->regs.byteregs[regdh] = getmem8(cpu, cpu->segregs[regcs], cpu->ip);
			StepIP(cpu, 1);
			break;

		case 0xB7:	/* B7 MOV cpu->regs.byteregs[regbh] Ib */
			cpu->regs.byteregs[regbh] = getmem8(cpu, cpu->segregs[regcs], cpu->ip);
			StepIP(cpu, 1);
			break;

		case 0xB8:	/* B8 MOV eAX Iv */
			cpu->oper1 = getmem16(cpu, cpu->segregs[regcs], cpu->ip);
			StepIP(cpu, 2);
			cpu->regs.wordregs[regax] = cpu->oper1;
			break;

		case 0xB9:	/* B9 MOV eCX Iv */
			cpu->oper1 = getmem16(cpu, cpu->segregs[regcs], cpu->ip);
			StepIP(cpu, 2);
			cpu->regs.wordregs[regcx] = cpu->oper1;
			break;

		case 0xBA:	/* BA MOV eDX Iv */
			cpu->oper1 = getmem16(cpu, cpu->segregs[regcs], cpu->ip);
			StepIP(cpu, 2);
			cpu->regs.wordregs[regdx] = cpu->oper1;
			break;

		case 0xBB:	/* BB MOV eBX Iv */
			cpu->oper1 = getmem16(cpu, cpu->segregs[regcs], cpu->ip);
			StepIP(cpu, 2);
			cpu->regs.wordregs[regbx] = cpu->oper1;
			break;

		case 0xBC:	/* BC MOV eSP Iv */
			cpu->regs.wordregs[regsp] = getmem16(cpu, cpu->segregs[regcs], cpu->ip);
			StepIP(cpu, 2);
			break;

		case 0xBD:	/* BD MOV eBP Iv */
			cpu->regs.wordregs[regbp] = getmem16(cpu, cpu->segregs[regcs], cpu->ip);
			StepIP(cpu, 2);
			break;

		case 0xBE:	/* BE MOV eSI Iv */
			cpu->regs.wordregs[regsi] = getmem16(cpu, cpu->segregs[regcs], cpu->ip);
			StepIP(cpu, 2);
			break;

		case 0xBF:	/* BF MOV eDI Iv */
			cpu->regs.wordregs[regdi] = getmem16(cpu, cpu->segregs[regcs], cpu->ip);
			StepIP(cpu, 2);
			break;

		case 0xC0:	/* C0 GRP2 byte imm8 (80186+) */
			modregrm(cpu);
			cpu->oper1b = readrm8(cpu, cpu->rm);
			cpu->oper2b = getmem8(cpu, cpu->segregs[regcs], cpu->ip);
			StepIP(cpu, 1);
			writerm8(cpu, cpu->rm, op_grp2_8(cpu, cpu->oper2b));
			break;

		case 0xC1:	/* C1 GRP2 word imm8 (80186+) */
			modregrm(cpu);
			cpu->oper1 = readrm16(cpu, cpu->rm);
			cpu->oper2 = getmem8(cpu, cpu->segregs[regcs], cpu->ip);
			StepIP(cpu, 1);
			writerm16(cpu, cpu->rm, op_grp2_16(cpu, (uint8_t)cpu->oper2));
			break;

		case 0xC2:	/* C2 RET Iw */
			cpu->oper1 = getmem16(cpu, cpu->segregs[regcs], cpu->ip);
			cpu->ip = pop(cpu);
			cpu->regs.wordregs[regsp] = cpu->regs.wordregs[regsp] + cpu->oper1;
			break;

		case 0xC3:	/* C3 RET */
			cpu->ip = pop(cpu);
			break;

		case 0xC4:	/* C4 LES Gv Mp */
			modregrm(cpu);
			getea(cpu, cpu->rm);
			putreg16(cpu, cpu->reg, cpu_read(cpu, cpu->ea) + cpu_read(cpu, cpu->ea + 1) * 256);
			cpu->segregs[reges] = cpu_read(cpu, cpu->ea + 2) + cpu_read(cpu, cpu->ea + 3) * 256;
			break;

		case 0xC5:	/* C5 LDS Gv Mp */
			modregrm(cpu);
			getea(cpu, cpu->rm);
			putreg16(cpu, cpu->reg, cpu_read(cpu, cpu->ea) + cpu_read(cpu, cpu->ea + 1) * 256);
			cpu->segregs[regds] = cpu_read(cpu, cpu->ea + 2) + cpu_read(cpu, cpu->ea + 3) * 256;
			break;

		case 0xC6:	/* C6 MOV Eb Ib */
			modregrm(cpu);
			writerm8(cpu, cpu->rm, getmem8(cpu, cpu->segregs[regcs], cpu->ip));
			StepIP(cpu, 1);
			break;

		case 0xC7:	/* C7 MOV Ev Iv */
			modregrm(cpu);
			writerm16(cpu, cpu->rm, getmem16(cpu, cpu->segregs[regcs], cpu->ip));
			StepIP(cpu, 2);
			break;

		case 0xC8:	/* C8 ENTER (80186+) */
			cpu->stacksize = getmem16(cpu, cpu->segregs[regcs], cpu->ip);
			StepIP(cpu, 2);
			cpu->nestlev = getmem8(cpu, cpu->segregs[regcs], cpu->ip);
			StepIP(cpu, 1);
			push(cpu, cpu->regs.wordregs[regbp]);
			cpu->frametemp = cpu->regs.wordregs[regsp];
			if (cpu->nestlev) {
				for (cpu->temp16 = 1; cpu->temp16 < cpu->nestlev; ++cpu->temp16) {
					cpu->regs.wordregs[regbp] = cpu->regs.wordregs[regbp] - 2;
					push(cpu, cpu->regs.wordregs[regbp]);
				}

				push(cpu, cpu->frametemp); //cpu->regs.wordregs[regsp]);
			}

			cpu->regs.wordregs[regbp] = cpu->frametemp;
			cpu->regs.wordregs[regsp] = cpu->regs.wordregs[regbp] - cpu->stacksize;

			break;

		case 0xC9:	/* C9 LEAVE (80186+) */
			cpu->regs.wordregs[regsp] = cpu->regs.wordregs[regbp];
			cpu->regs.wordregs[regbp] = pop(cpu);
			break;

		case 0xCA:	/* CA RETF Iw */
			cpu->oper1 = getmem16(cpu, cpu->segregs[regcs], cpu->ip);
			cpu->ip = pop(cpu);
			cpu->segregs[regcs] = pop(cpu);
			cpu->regs.wordregs[regsp] = cpu->regs.wordregs[regsp] + cpu->oper1;
			break;

		case 0xCB:	/* CB RETF */
			cpu->ip = pop(cpu);
			cpu->segregs[regcs] = pop(cpu);
			break;

		case 0xCC:	/* CC INT 3 */
			cpu_intcall(cpu, 3);
			break;

		case 0xCD:	/* CD INT Ib */
			cpu->oper1b = getmem8(cpu, cpu->segregs[regcs], cpu->ip);
			StepIP(cpu, 1);
			cpu_intcall(cpu, cpu->oper1b);
			break;

		case 0xCE:	/* CE INTO */
			if (cpu->of) {
				cpu_intcall(cpu, 4);
			}
			break;

		case 0xCF:	/* CF IRET */
		{
			if (cpu->protected_mode) {
				uint16_t temp_ip = pop(cpu);
				uint16_t temp_cs = pop(cpu);
				uint16_t temp_flags = pop(cpu);
				uint8_t cpl = cpu->segregs[regcs] & 3;
				uint8_t rpl = temp_cs & 3;

				if (rpl > cpl) {
					uint16_t temp_sp = pop(cpu);
					uint16_t temp_ss = pop(cpu);

					load_descriptor(cpu, regss, temp_ss);
					cpu->segregs[regss] = temp_ss;
					cpu->regs.wordregs[regsp] = temp_sp;
				}

				load_descriptor(cpu, regcs, temp_cs);
				cpu->segregs[regcs] = temp_cs;
				cpu->ip = temp_ip;
				decodeflagsword(cpu, temp_flags);

			}
			else {
				cpu->ip = pop(cpu);
				cpu->segregs[regcs] = pop(cpu);
				decodeflagsword(cpu, pop(cpu));
			}
			break;
		}

		case 0xD0:	/* D0 GRP2 Eb 1 */
			modregrm(cpu);
			cpu->oper1b = readrm8(cpu, cpu->rm);
			writerm8(cpu, cpu->rm, op_grp2_8(cpu, 1));
			break;

		case 0xD1:	/* D1 GRP2 Ev 1 */
			modregrm(cpu);
			cpu->oper1 = readrm16(cpu, cpu->rm);
			writerm16(cpu, cpu->rm, op_grp2_16(cpu, 1));
			break;

		case 0xD2:	/* D2 GRP2 Eb cpu->regs.byteregs[regcl] */
			modregrm(cpu);
			cpu->oper1b = readrm8(cpu, cpu->rm);
			writerm8(cpu, cpu->rm, op_grp2_8(cpu, cpu->regs.byteregs[regcl]));
			break;

		case 0xD3:	/* D3 GRP2 Ev cpu->regs.byteregs[regcl] */
			modregrm(cpu);
			cpu->oper1 = readrm16(cpu, cpu->rm);
			writerm16(cpu, cpu->rm, op_grp2_16(cpu, cpu->regs.byteregs[regcl]));
			break;

		case 0xD4:	/* D4 AAM I0 */
			cpu->oper1 = getmem8(cpu, cpu->segregs[regcs], cpu->ip);
			StepIP(cpu, 1);
			if (!cpu->oper1) {
				cpu_intcall(cpu, 0);
				break;
			}	/* division by zero */

			cpu->regs.byteregs[regah] = (cpu->regs.byteregs[regal] / cpu->oper1) & 255;
			cpu->regs.byteregs[regal] = (cpu->regs.byteregs[regal] % cpu->oper1) & 255;
			flag_szp16(cpu, cpu->regs.wordregs[regax]);
			break;

		case 0xD5:	/* D5 AAD I0 */
			cpu->oper1 = getmem8(cpu, cpu->segregs[regcs], cpu->ip);
			StepIP(cpu, 1);
			cpu->regs.byteregs[regal] = (cpu->regs.byteregs[regah] * cpu->oper1 + cpu->regs.byteregs[regal]) & 255;
			cpu->regs.byteregs[regah] = 0;
			flag_szp16(cpu, cpu->regs.byteregs[regah] * cpu->oper1 + cpu->regs.byteregs[regal]);
			cpu->sf = 0;
			break;

		case 0xD6:	/* D6 SALC on 8086/8088 */
			cpu->regs.byteregs[regal] = cpu->cf ? 0xFF : 0x00;
			break;

		case 0xD7:	/* D7 XLAT */
			cpu->regs.byteregs[regal] = cpu_read(cpu, cpu->useseg * 16 + (cpu->regs.wordregs[regbx]) + cpu->regs.byteregs[regal]);
			break;

		case 0xD8:
		case 0xD9:
		case 0xDA:
		case 0xDB:
		case 0xDC:
		case 0xDE:
		case 0xDD:
		case 0xDF:	/* escape to x87 FPU */
			if (cpu->msw & 0x0008) {
				debug_log(DEBUG_INFO, "[CPU] FPU instruction with TS flag set. Triggering INT 7.\n");
				cpu_intcall(cpu, 7);
				cpu->ip = cpu->saveip;
				break;
			}
			modregrm(cpu);
			OpFpu(cpu);
			break;

		case 0xE0:	/* E0 LOOPNZ Jb */
			cpu->temp16 = signext(getmem8(cpu, cpu->segregs[regcs], cpu->ip));
			StepIP(cpu, 1);
			cpu->regs.wordregs[regcx] = cpu->regs.wordregs[regcx] - 1;
			if ((cpu->regs.wordregs[regcx]) && !cpu->zf) {
				cpu->ip = cpu->ip + cpu->temp16;
			}
			break;

		case 0xE1:	/* E1 LOOPZ Jb */
			cpu->temp16 = signext(getmem8(cpu, cpu->segregs[regcs], cpu->ip));
			StepIP(cpu, 1);
			cpu->regs.wordregs[regcx] = cpu->regs.wordregs[regcx] - 1;
			if (cpu->regs.wordregs[regcx] && (cpu->zf == 1)) {
				cpu->ip = cpu->ip + cpu->temp16;
			}
			break;

		case 0xE2:	/* E2 LOOP Jb */
			cpu->temp16 = signext(getmem8(cpu, cpu->segregs[regcs], cpu->ip));
			StepIP(cpu, 1);
			cpu->regs.wordregs[regcx] = cpu->regs.wordregs[regcx] - 1;
			if (cpu->regs.wordregs[regcx]) {
				cpu->ip = cpu->ip + cpu->temp16;
			}
			break;

		case 0xE3:	/* E3 JCXZ Jb */
			cpu->temp16 = signext(getmem8(cpu, cpu->segregs[regcs], cpu->ip));
			StepIP(cpu, 1);
			if (!cpu->regs.wordregs[regcx]) {
				cpu->ip = cpu->ip + cpu->temp16;
			}
			break;

		case 0xE4:	/* E4 IN cpu->regs.byteregs[regal] Ib */
			cpu->oper1b = getmem8(cpu, cpu->segregs[regcs], cpu->ip);
			StepIP(cpu, 1);
			cpu->regs.byteregs[regal] = (uint8_t)port_read(cpu, cpu->oper1b);
			break;

		case 0xE5:	/* E5 IN eAX Ib */
			cpu->oper1b = getmem8(cpu, cpu->segregs[regcs], cpu->ip);
			StepIP(cpu, 1);
			cpu->regs.wordregs[regax] = port_readw(cpu, cpu->oper1b);
			break;

		case 0xE6:	/* E6 OUT Ib cpu->regs.byteregs[regal] */
			cpu->oper1b = getmem8(cpu, cpu->segregs[regcs], cpu->ip);
			StepIP(cpu, 1);
			port_write(cpu, cpu->oper1b, cpu->regs.byteregs[regal]);
			break;

		case 0xE7:	/* E7 OUT Ib eAX */
			cpu->oper1b = getmem8(cpu, cpu->segregs[regcs], cpu->ip);
			StepIP(cpu, 1);
			port_writew(cpu, cpu->oper1b, cpu->regs.wordregs[regax]);
			break;

		case 0xE8:	/* E8 CALL Jv */
			cpu->oper1 = getmem16(cpu, cpu->segregs[regcs], cpu->ip);
			StepIP(cpu, 2);
			push(cpu, cpu->ip);
			cpu->ip = cpu->ip + cpu->oper1;
			break;

		case 0xE9:	/* E9 JMP Jv */
			cpu->oper1 = getmem16(cpu, cpu->segregs[regcs], cpu->ip);
			StepIP(cpu, 2);
			cpu->ip = cpu->ip + cpu->oper1;
			break;

		case 0xEA:	/* EA JMP Ap */
			cpu->oper1 = getmem16(cpu, cpu->segregs[regcs], cpu->ip);
			StepIP(cpu, 2);
			cpu->oper2 = getmem16(cpu, cpu->segregs[regcs], cpu->ip);
			cpu->ip = cpu->oper1;
			cpu->segregs[regcs] = cpu->oper2;
			if (cpu->protected_mode) {
				load_descriptor(cpu, regcs, cpu->segregs[regcs]);
			}
			break;

		case 0xEB:	/* EB JMP Jb */
			cpu->oper1 = signext(getmem8(cpu, cpu->segregs[regcs], cpu->ip));
			StepIP(cpu, 1);
			cpu->ip = cpu->ip + cpu->oper1;
			break;

		case 0xEC:	/* EC IN cpu->regs.byteregs[regal] regdx */
			cpu->oper1 = cpu->regs.wordregs[regdx];
			cpu->regs.byteregs[regal] = (uint8_t)port_read(cpu, cpu->oper1);
			break;

		case 0xED:	/* ED IN eAX regdx */
			cpu->oper1 = cpu->regs.wordregs[regdx];
			cpu->regs.wordregs[regax] = port_readw(cpu, cpu->oper1);
			break;

		case 0xEE:	/* EE OUT regdx cpu->regs.byteregs[regal] */
			cpu->oper1 = cpu->regs.wordregs[regdx];
			port_write(cpu, cpu->oper1, cpu->regs.byteregs[regal]);
			break;

		case 0xEF:	/* EF OUT regdx eAX */
			cpu->oper1 = cpu->regs.wordregs[regdx];
			port_writew(cpu, cpu->oper1, cpu->regs.wordregs[regax]);
			break;

		case 0xF4:	/* F4 HLT */
			cpu->hltstate = 1;
			break;

		case 0xF5:	/* F5 CMC */
			if (!cpu->cf) {
				cpu->cf = 1;
			}
			else {
				cpu->cf = 0;
			}
			break;

		case 0xF6:	/* F6 GRP3a Eb */
			modregrm(cpu);
			cpu->oper1b = readrm8(cpu, cpu->rm);
			op_grp3_8(cpu);
			if ((cpu->reg > 1) && (cpu->reg < 4)) {
				writerm8(cpu, cpu->rm, cpu->res8);
			}
			break;

		case 0xF7:	/* F7 GRP3b Ev */
			modregrm(cpu);
			cpu->oper1 = readrm16(cpu, cpu->rm);
			op_grp3_16(cpu);
			if ((cpu->reg > 1) && (cpu->reg < 4)) {
				writerm16(cpu, cpu->rm, cpu->res16);
			}
			break;

		case 0xF8:	/* F8 CLC */
			cpu->cf = 0;
			break;

		case 0xF9:	/* F9 STC */
			cpu->cf = 1;
			break;

		case 0xFA:	/* FA CLI */
			cpu->ifl = 0;
			break;

		case 0xFB:	/* FB STI */
			cpu->ifl = 1;
			break;

		case 0xFC:	/* FC CLD */
			cpu->df = 0;
			break;

		case 0xFD:	/* FD STD */
			cpu->df = 1;
			break;

		case 0xFE:	/* FE GRP4 Eb */
			modregrm(cpu);
			cpu->oper1b = readrm8(cpu, cpu->rm);
			cpu->oper2b = 1;
			if (!cpu->reg) {
				cpu->tempcf = cpu->cf;
				cpu->res8 = cpu->oper1b + cpu->oper2b;
				flag_add8(cpu, cpu->oper1b, cpu->oper2b);
				cpu->cf = cpu->tempcf;
				writerm8(cpu, cpu->rm, cpu->res8);
			}
			else {
				cpu->tempcf = cpu->cf;
				cpu->res8 = cpu->oper1b - cpu->oper2b;
				flag_sub8(cpu, cpu->oper1b, cpu->oper2b);
				cpu->cf = cpu->tempcf;
				writerm8(cpu, cpu->rm, cpu->res8);
			}
			break;

		case 0xFF:	/* FF GRP5 Ev */
			modregrm(cpu);
			cpu->oper1 = readrm16(cpu, cpu->rm);
			op_grp5(cpu);
			break;

		default:
			cpu_intcall(cpu, 6); /* trip invalid opcode exception. this occurs on the 80186+, 8086/8088 CPUs treat them as NOPs. */
			/* technically they aren't exactly like NOPs in most cases, but for our pursoses, that's accurate enough. */
			debug_log(DEBUG_INFO, "[CPU] Invalid opcode exception at %04X:%04X\r\n", cpu->segregs[regcs], firstip);
			break;
		}

	skipexecution:
		;
	}
}

void cpu_registerIntCallback(CPU_t* cpu, uint8_t interrupt, void (*cb)(CPU_t*, uint8_t)) {
	cpu->int_callback[interrupt] = cb;
}
