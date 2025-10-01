#include <math.h>
#include <string.h>
#include <stdint.h>
#include "fpu.h"
#include "cpu.h"
#include "../debuglog.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

union FloatPun {
    float f;
    uint32_t i;
};

union DoublePun {
    double f;
    uint64_t i;
};

#define FPUREG 0
#define MEMORY 1
#define DISP(x, y, z) (((x) & 7) << 4 | (y) << 3 | (uint32_t)(z))

static uint32_t fpu_get_memory_address(CPU_t* cpu) {
    if (cpu->mode == 3) {
        return 0;
    }
    getea(cpu, cpu->rm);
    return cpu->ea;
}

static int16_t FpuGetMemoryShort(CPU_t* cpu) {
    return cpu_readw(cpu, fpu_get_memory_address(cpu));
}

static void FpuSetMemoryShort(CPU_t* cpu, int16_t i) {
    cpu_writew(cpu, fpu_get_memory_address(cpu), i);
}

static int32_t FpuGetMemoryInt(CPU_t* cpu) {
    uint32_t addr = fpu_get_memory_address(cpu);
    return cpu_readw(cpu, addr) | (cpu_readw(cpu, addr + 2) << 16);
}

static void FpuSetMemoryInt(CPU_t* cpu, int32_t i) {
    uint32_t addr = fpu_get_memory_address(cpu);
    cpu_writew(cpu, addr, i & 0xFFFF);
    cpu_writew(cpu, addr + 2, i >> 16);
}

static int64_t FpuGetMemoryLong(CPU_t* cpu) {
    uint32_t addr = fpu_get_memory_address(cpu);
    return (uint64_t)cpu_readw(cpu, addr) | ((uint64_t)cpu_readw(cpu, addr + 2) << 16) |
        ((uint64_t)cpu_readw(cpu, addr + 4) << 32) | ((uint64_t)cpu_readw(cpu, addr + 6) << 48);
}

static void FpuSetMemoryLong(CPU_t* cpu, int64_t i) {
    uint32_t addr = fpu_get_memory_address(cpu);
    cpu_writew(cpu, addr, i & 0xFFFF);
    cpu_writew(cpu, addr + 2, (i >> 16) & 0xFFFF);
    cpu_writew(cpu, addr + 4, (i >> 32) & 0xFFFF);
    cpu_writew(cpu, addr + 6, (i >> 48) & 0xFFFF);
}

static float FpuGetMemoryFloat(CPU_t* cpu) {
    union FloatPun u;
    u.i = FpuGetMemoryInt(cpu);
    return u.f;
}

static void FpuSetMemoryFloat(CPU_t* cpu, float f) {
    union FloatPun u = { f };
    FpuSetMemoryInt(cpu, u.i);
}

static double FpuGetMemoryDouble(CPU_t* cpu) {
    union DoublePun u;
    u.i = FpuGetMemoryLong(cpu);
    return u.f;
}

static void FpuSetMemoryDouble(CPU_t* cpu, double f) {
    union DoublePun u = { f };
    FpuSetMemoryLong(cpu, u.i);
}

static double FpuGetMemoryLdbl(CPU_t* cpu) {
    return FpuGetMemoryDouble(cpu);
}

static void FpuSetMemoryLdbl(CPU_t* cpu, double f) {
    FpuSetMemoryDouble(cpu, f);
}

static void OnFpuStackOverflow(CPU_t* cpu) {
    cpu->fpu.sw |= kFpuSwIe | kFpuSwC1 | kFpuSwSf;
}

static double OnFpuStackUnderflow(CPU_t* cpu) {
    cpu->fpu.sw |= kFpuSwIe | kFpuSwSf;
    cpu->fpu.sw &= ~kFpuSwC1;
    return -NAN;
}

int FpuGetTag(CPU_t* cpu, unsigned i) {
    unsigned t = cpu->fpu.tw;
    i += (cpu->fpu.sw & kFpuSwSp) >> 11;
    i &= 7;
    i *= 2;
    t &= 3 << i;
    t >>= i;
    return t;
}

void FpuSetTag(CPU_t* cpu, unsigned i, unsigned t) {
    i += (cpu->fpu.sw & kFpuSwSp) >> 11;
    t &= 3;
    i &= 7;
    i *= 2;
    cpu->fpu.tw &= ~(3 << i);
    cpu->fpu.tw |= t << i;
}

static double St(CPU_t* cpu, int i) {
    if (FpuGetTag(cpu, i) == kFpuTagEmpty) {
        return OnFpuStackUnderflow(cpu);
    }
    return *FpuSt(cpu, i);
}

static double St0(CPU_t* cpu) { return St(cpu, 0); }
static double St1(CPU_t* cpu) { return St(cpu, 1); }
static double StRm(CPU_t* cpu) { return St(cpu, cpu->rm); }

void FpuPush(CPU_t* cpu, double x) {
    if (FpuGetTag(cpu, -1) != kFpuTagEmpty) {
        OnFpuStackOverflow(cpu);
    }
    cpu->fpu.sw = (cpu->fpu.sw & ~kFpuSwSp) | ((cpu->fpu.sw - (1 << 11)) & kFpuSwSp);
    *FpuSt(cpu, 0) = x;
    FpuSetTag(cpu, 0, kFpuTagValid);
}

double FpuPop(CPU_t* cpu) {
    double x;
    if (FpuGetTag(cpu, 0) != kFpuTagEmpty) {
        x = *FpuSt(cpu, 0);
        FpuSetTag(cpu, 0, kFpuTagEmpty);
    }
    else {
        x = OnFpuStackUnderflow(cpu);
    }
    cpu->fpu.sw = (cpu->fpu.sw & ~kFpuSwSp) | ((cpu->fpu.sw + (1 << 11)) & kFpuSwSp);
    return x;
}

static void FpuSetSt0(CPU_t* cpu, double x) { *FpuSt(cpu, 0) = x; }
static void FpuSetStRm(CPU_t* cpu, double x) { *FpuSt(cpu, cpu->rm) = x; }
static void FpuSetStPop(CPU_t* cpu, int i, double x) { *FpuSt(cpu, i) = x; FpuPop(cpu); }
static void FpuSetStRmPop(CPU_t* cpu, double x) { FpuSetStPop(cpu, cpu->rm, x); }

static double fyl2x(double x, double y) { return y * log2(x); }
static double fptan(double x, uint16_t* sw) { *sw &= ~kFpuSwC2; return tan(x); }

static void FpuCompare(CPU_t* cpu, double y) {
    double x = St0(cpu);
    cpu->fpu.sw &= ~(kFpuSwC0 | kFpuSwC1 | kFpuSwC2 | kFpuSwC3);
    if (!isunordered(x, y)) {
        if (x < y) cpu->fpu.sw |= kFpuSwC0;
        if (x == y) cpu->fpu.sw |= kFpuSwC3;
    }
    else {
        cpu->fpu.sw |= kFpuSwC0 | kFpuSwC2 | kFpuSwC3 | kFpuSwIe;
    }
}

static void OpFaddStEst(CPU_t* cpu) { FpuSetSt0(cpu, St0(cpu) + StRm(cpu)); }
static void OpFmulStEst(CPU_t* cpu) { FpuSetSt0(cpu, St0(cpu) * StRm(cpu)); }
static void OpFsubStEst(CPU_t* cpu) { FpuSetSt0(cpu, St0(cpu) - StRm(cpu)); }
static void OpFsubrStEst(CPU_t* cpu) { FpuSetSt0(cpu, StRm(cpu) - St0(cpu)); }
static void OpFdivStEst(CPU_t* cpu) { FpuSetSt0(cpu, St0(cpu) / StRm(cpu)); }
static void OpFdivrStEst(CPU_t* cpu) { FpuSetSt0(cpu, StRm(cpu) / St0(cpu)); }
static void OpFaddEstSt(CPU_t* cpu) { FpuSetStRm(cpu, StRm(cpu) + St0(cpu)); }
static void OpFmulEstSt(CPU_t* cpu) { FpuSetStRm(cpu, StRm(cpu) * St0(cpu)); }
static void OpFsubEstSt(CPU_t* cpu) { FpuSetStRm(cpu, St0(cpu) - StRm(cpu)); }
static void OpFsubrEstSt(CPU_t* cpu) { FpuSetStRm(cpu, StRm(cpu) - St0(cpu)); }
static void OpFdivEstSt(CPU_t* cpu) { FpuSetStRm(cpu, StRm(cpu) / St0(cpu)); }
static void OpFdivrEstSt(CPU_t* cpu) { FpuSetStRm(cpu, St0(cpu) / StRm(cpu)); }
static void OpFaddp(CPU_t* cpu) { FpuSetStRmPop(cpu, StRm(cpu) + St0(cpu)); }
static void OpFmulp(CPU_t* cpu) { FpuSetStRmPop(cpu, StRm(cpu) * St0(cpu)); }
static void OpFsubp(CPU_t* cpu) { FpuSetStRmPop(cpu, StRm(cpu) - St0(cpu)); }
static void OpFsubrp(CPU_t* cpu) { FpuSetStPop(cpu, 1, St0(cpu) - St1(cpu)); }
static void OpFdivp(CPU_t* cpu) { FpuSetStRmPop(cpu, StRm(cpu) / St0(cpu)); }
static void OpFdivrp(CPU_t* cpu) { FpuSetStRmPop(cpu, St0(cpu) / StRm(cpu)); }
static void OpFadds(CPU_t* cpu) { FpuSetSt0(cpu, St0(cpu) + FpuGetMemoryFloat(cpu)); }
static void OpFmuls(CPU_t* cpu) { FpuSetSt0(cpu, St0(cpu) * FpuGetMemoryFloat(cpu)); }
static void OpFsubs(CPU_t* cpu) { FpuSetSt0(cpu, St0(cpu) - FpuGetMemoryFloat(cpu)); }
static void OpFsubrs(CPU_t* cpu) { FpuSetSt0(cpu, FpuGetMemoryFloat(cpu) - St0(cpu)); }
static void OpFdivs(CPU_t* cpu) { FpuSetSt0(cpu, St0(cpu) / FpuGetMemoryFloat(cpu)); }
static void OpFdivrs(CPU_t* cpu) { FpuSetSt0(cpu, FpuGetMemoryFloat(cpu) / St0(cpu)); }
static void OpFaddl(CPU_t* cpu) { FpuSetSt0(cpu, St0(cpu) + FpuGetMemoryDouble(cpu)); }
static void OpFmull(CPU_t* cpu) { FpuSetSt0(cpu, St0(cpu) * FpuGetMemoryDouble(cpu)); }
static void OpFsubl(CPU_t* cpu) { FpuSetSt0(cpu, St0(cpu) - FpuGetMemoryDouble(cpu)); }
static void OpFsubrl(CPU_t* cpu) { FpuSetSt0(cpu, FpuGetMemoryDouble(cpu) - St0(cpu)); }
static void OpFdivl(CPU_t* cpu) { FpuSetSt0(cpu, St0(cpu) / FpuGetMemoryDouble(cpu)); }
static void OpFdivrl(CPU_t* cpu) { FpuSetSt0(cpu, FpuGetMemoryDouble(cpu) / St0(cpu)); }
static void OpFiadds(CPU_t* cpu) { FpuSetSt0(cpu, St0(cpu) + FpuGetMemoryShort(cpu)); }
static void OpFimuls(CPU_t* cpu) { FpuSetSt0(cpu, St0(cpu) * FpuGetMemoryShort(cpu)); }
static void OpFisubs(CPU_t* cpu) { FpuSetSt0(cpu, St0(cpu) - FpuGetMemoryShort(cpu)); }
static void OpFisubrs(CPU_t* cpu) { FpuSetSt0(cpu, FpuGetMemoryShort(cpu) - St0(cpu)); }
static void OpFidivs(CPU_t* cpu) { FpuSetSt0(cpu, St0(cpu) / FpuGetMemoryShort(cpu)); }
static void OpFidivrs(CPU_t* cpu) { FpuSetSt0(cpu, FpuGetMemoryShort(cpu) / St0(cpu)); }
static void OpFiaddl(CPU_t* cpu) { FpuSetSt0(cpu, St0(cpu) + FpuGetMemoryInt(cpu)); }
static void OpFimull(CPU_t* cpu) { FpuSetSt0(cpu, St0(cpu) * FpuGetMemoryInt(cpu)); }
static void OpFisubl(CPU_t* cpu) { FpuSetSt0(cpu, St0(cpu) - FpuGetMemoryInt(cpu)); }
static void OpFisubrl(CPU_t* cpu) { FpuSetSt0(cpu, FpuGetMemoryInt(cpu) - St0(cpu)); }
static void OpFidivl(CPU_t* cpu) { FpuSetSt0(cpu, St0(cpu) / FpuGetMemoryInt(cpu)); }
static void OpFidivrl(CPU_t* cpu) { FpuSetSt0(cpu, FpuGetMemoryInt(cpu) / St0(cpu)); }
static void OpFilds(CPU_t* cpu) { FpuPush(cpu, FpuGetMemoryShort(cpu)); }
static void OpFildl(CPU_t* cpu) { FpuPush(cpu, FpuGetMemoryInt(cpu)); }
static void OpFildll(CPU_t* cpu) { FpuPush(cpu, FpuGetMemoryLong(cpu)); }
static void OpFists(CPU_t* cpu) { FpuSetMemoryShort(cpu, round(St0(cpu))); }
static void OpFistl(CPU_t* cpu) { FpuSetMemoryInt(cpu, round(St0(cpu))); }
static void OpFistpl(CPU_t* cpu) { OpFistl(cpu); FpuPop(cpu); }
static void OpFistps(CPU_t* cpu) { FpuSetMemoryShort(cpu, round(FpuPop(cpu))); }
static void OpFistpll(CPU_t* cpu) { FpuSetMemoryLong(cpu, round(FpuPop(cpu))); }
static void OpFld(CPU_t* cpu) { FpuPush(cpu, StRm(cpu)); }
static void OpFlds(CPU_t* cpu) { FpuPush(cpu, FpuGetMemoryFloat(cpu)); }
static void OpFldl(CPU_t* cpu) { FpuPush(cpu, FpuGetMemoryDouble(cpu)); }
static void OpFldt(CPU_t* cpu) { FpuPush(cpu, FpuGetMemoryLdbl(cpu)); }
static void OpFsts(CPU_t* cpu) { FpuSetMemoryFloat(cpu, St0(cpu)); }
static void OpFstps(CPU_t* cpu) { OpFsts(cpu); FpuPop(cpu); }
static void OpFstl(CPU_t* cpu) { FpuSetMemoryDouble(cpu, St0(cpu)); }
static void OpFstpl(CPU_t* cpu) { OpFstl(cpu); FpuPop(cpu); }
static void OpFrstor(CPU_t* cpu) {
    uint32_t addr = fpu_get_memory_address(cpu);

    cpu->fpu.cw = cpu_readw(cpu, addr + 0);
    cpu->fpu.sw = cpu_readw(cpu, addr + 2);
    cpu->fpu.tw = cpu_readw(cpu, addr + 4);

    cpu->fpu.ip = cpu_readw(cpu, addr + 6);
    cpu->fpu.cs = cpu_readw(cpu, addr + 8);

    for (int i = 0; i < 8; i++) {
        union DoublePun u;
        u.i = (uint64_t)cpu_readw(cpu, addr + 14 + (i * 10) + 0) |
            ((uint64_t)cpu_readw(cpu, addr + 14 + (i * 10) + 2) << 16) |
            ((uint64_t)cpu_readw(cpu, addr + 14 + (i * 10) + 4) << 32) |
            ((uint64_t)cpu_readw(cpu, addr + 14 + (i * 10) + 6) << 48);
        cpu->fpu.st[i] = u.f;
    }
}
static void OpFstpt(CPU_t* cpu) { FpuSetMemoryLdbl(cpu, FpuPop(cpu)); }
static void OpFst(CPU_t* cpu) { FpuSetStRm(cpu, St0(cpu)); }
static void OpFstp(CPU_t* cpu) { FpuSetStRmPop(cpu, St0(cpu)); }
static void OpFxch(CPU_t* cpu) { double t = StRm(cpu); FpuSetStRm(cpu, St0(cpu)); FpuSetSt0(cpu, t); }
static void OpFchs(CPU_t* cpu) { FpuSetSt0(cpu, -St0(cpu)); }
static void OpFabs(CPU_t* cpu) { FpuSetSt0(cpu, fabs(St0(cpu))); }
static void OpFcom(CPU_t* cpu) { FpuCompare(cpu, StRm(cpu)); }
static void OpFcomp(CPU_t* cpu) { OpFcom(cpu); FpuPop(cpu); }
static void OpFcompp(CPU_t* cpu) { FpuCompare(cpu, St1(cpu)); FpuPop(cpu); FpuPop(cpu); }
static void OpFicoml(CPU_t* cpu) { FpuCompare(cpu, FpuGetMemoryInt(cpu)); }
static void OpFicompl(CPU_t* cpu) { OpFicoml(cpu); FpuPop(cpu); }
static void OpFicoms(CPU_t* cpu) { FpuCompare(cpu, FpuGetMemoryShort(cpu)); }
static void OpFicomps(CPU_t* cpu) { OpFicoms(cpu); FpuPop(cpu); }
static void OpFldcw(CPU_t* cpu) { cpu->fpu.cw = FpuGetMemoryShort(cpu); }
static void OpFstcw(CPU_t* cpu) { FpuSetMemoryShort(cpu, cpu->fpu.cw); }
static void OpFldConstant(CPU_t* cpu) {
    double x;
    switch (cpu->rm) {
    case 0: x = 1.0; break;
    case 1: x = log10(2.0); break;
    case 2: x = log2(exp(1.0)); break;
    case 3: x = M_PI; break;
    case 4: x = log2(10.0); break;
    case 5: x = log(2.0); break;
    case 6: x = 0.0; break;
    default: x = NAN; break;
    }
    FpuPush(cpu, x);
}
static void OpFstswMw(CPU_t* cpu) { FpuSetMemoryShort(cpu, cpu->fpu.sw); }
static void OpFstswAx(CPU_t* cpu) { cpu->regs.wordregs[regax] = cpu->fpu.sw; }
void OpFsetpm(CPU_t* cpu) {
    (void)cpu;
}
static void OpF2xm1(CPU_t* cpu) {
    double x = St0(cpu);
    if (x < 0.0 || x > 0.5) {
        cpu->fpu.sw |= kFpuSwIe;
        return;
    }
    FpuSetSt0(cpu, exp2(x) - 1.0);
}
static void OpFyl2x(CPU_t* cpu) { FpuSetStPop(cpu, 1, fyl2x(St0(cpu), St1(cpu))); }
static void OpFyl2xp1(CPU_t* cpu) {
    double x = St0(cpu);
    if (fabs(x) >= (1.0 - sqrt(0.5))) {
        cpu->fpu.sw |= kFpuSwIe;
        return;
    }
    FpuSetStPop(cpu, 1, St1(cpu) * log2(x + 1.0));
}
static void OpFptan(CPU_t* cpu) {
    double x = St0(cpu);
    if (fabs(x) >= (M_PI / 4.0)) {
        cpu->fpu.sw |= kFpuSwIe;
        return;
    }
    FpuSetSt0(cpu, tan(x));
    FpuPush(cpu, 1.0);
}
static void OpFpatan(CPU_t* cpu) {
    double y = St1(cpu);
    double x = St0(cpu);
    if (fabs(y) > fabs(x)) {
        cpu->fpu.sw |= kFpuSwIe;
        return;
    }
    FpuSetStPop(cpu, 1, atan2(y, x));
}
static void OpFsin(CPU_t* cpu) {
    double x = St0(cpu);
    if (isfinite(x)) {
        cpu->fpu.sw &= ~kFpuSwC2;
        FpuSetSt0(cpu, sin(x));
    }
    else {
        cpu->fpu.sw |= kFpuSwC2;
    }
}
static void OpFsqrt(CPU_t* cpu) { FpuSetSt0(cpu, sqrt(St0(cpu))); }
static void OpFdecstp(CPU_t* cpu) { cpu->fpu.sw = (cpu->fpu.sw & ~kFpuSwSp) | ((cpu->fpu.sw - (1 << 11)) & kFpuSwSp); }
static void OpFincstp(CPU_t* cpu) { cpu->fpu.sw = (cpu->fpu.sw & ~kFpuSwSp) | ((cpu->fpu.sw + (1 << 11)) & kFpuSwSp); }
static void OpFtst(CPU_t* cpu) { FpuCompare(cpu, 0.0); }
static void OpFnclex(CPU_t* cpu) { cpu->fpu.sw &= ~(kFpuSwIe | kFpuSwDe | kFpuSwZe | kFpuSwOe | kFpuSwUe | kFpuSwPe | kFpuSwEs | kFpuSwSf | kFpuSwBf); }
static void OpFnop(CPU_t* cpu) { /* do nothing */ }
void OpFinit(CPU_t* cpu) { cpu->fpu.cw = 0x037F; cpu->fpu.sw = 0; cpu->fpu.tw = 0xFFFF; }
static void OpFxam(CPU_t* cpu) {
    double x = St0(cpu);
    cpu->fpu.sw &= ~(kFpuSwC0 | kFpuSwC1 | kFpuSwC2 | kFpuSwC3);
    if (signbit(x)) cpu->fpu.sw |= kFpuSwC1;
    if (FpuGetTag(cpu, 0) == kFpuTagEmpty) {
        cpu->fpu.sw |= kFpuSwC0 | kFpuSwC3;
    }
    else {
        switch (fpclassify(x)) {
        case FP_NAN: cpu->fpu.sw |= kFpuSwC0; break;
        case FP_INFINITE: cpu->fpu.sw |= kFpuSwC0 | kFpuSwC2; break;
        case FP_ZERO: cpu->fpu.sw |= kFpuSwC3; break;
        case FP_SUBNORMAL: cpu->fpu.sw &= ~(kFpuSwC0 | kFpuSwC2 | kFpuSwC3); break;
        case FP_NORMAL: cpu->fpu.sw |= kFpuSwC2; break;
        }
    }
}
static void OpFfree(CPU_t* cpu) { FpuSetTag(cpu, cpu->rm, kFpuTagEmpty); }

void OpFpu(CPU_t* cpu) {
    uint8_t modrm_byte = getmem8(cpu, cpu->segregs[regcs], cpu->ip - 1);
    uint8_t ismemory = cpu->mode != 3;
    uint32_t disp = ((cpu->opcode & 7) << 4) | (ismemory << 3) | cpu->reg;

    if (ismemory) {
        cpu->fpu.dp = fpu_get_memory_address(cpu);
    }

    switch (disp) {
    case DISP(0xD8, FPUREG, 0): OpFaddStEst(cpu); break;
    case DISP(0xD8, FPUREG, 1): OpFmulStEst(cpu); break;
    case DISP(0xD8, FPUREG, 2): OpFcom(cpu); break;
    case DISP(0xD8, FPUREG, 3): OpFcomp(cpu); break;
    case DISP(0xD8, FPUREG, 4): OpFsubStEst(cpu); break;
    case DISP(0xD8, FPUREG, 5): OpFsubrStEst(cpu); break;
    case DISP(0xD8, FPUREG, 6): OpFdivStEst(cpu); break;
    case DISP(0xD8, FPUREG, 7): OpFdivrStEst(cpu); break;
    case DISP(0xD8, MEMORY, 0): OpFadds(cpu); break;
    case DISP(0xD8, MEMORY, 1): OpFmuls(cpu); break;
    case DISP(0xD8, MEMORY, 2): FpuCompare(cpu, FpuGetMemoryFloat(cpu)); break;
    case DISP(0xD8, MEMORY, 3): FpuCompare(cpu, FpuGetMemoryFloat(cpu)); FpuPop(cpu); break;
    case DISP(0xD8, MEMORY, 4): OpFsubs(cpu); break;
    case DISP(0xD8, MEMORY, 5): OpFsubrs(cpu); break;
    case DISP(0xD8, MEMORY, 6): OpFdivs(cpu); break;
    case DISP(0xD8, MEMORY, 7): OpFdivrs(cpu); break;
    case DISP(0xD9, FPUREG, 0): OpFld(cpu); break;
    case DISP(0xD9, FPUREG, 1): OpFxch(cpu); break;
    case DISP(0xD9, FPUREG, 2): OpFnop(cpu); break;
    case DISP(0xD9, FPUREG, 3): OpFstp(cpu); break;
    case DISP(0xD9, FPUREG, 4): switch (cpu->rm) { case 0: OpFchs(cpu); break; case 1: OpFabs(cpu); break; case 4: OpFtst(cpu); break; case 5: OpFxam(cpu); break; default: goto invalid_opcode; } break;
    case DISP(0xD9, FPUREG, 5): OpFldConstant(cpu); break;
    case DISP(0xD9, FPUREG, 6): switch (cpu->rm) { case 0: OpF2xm1(cpu); break; case 1: OpFyl2x(cpu); break; case 2: OpFptan(cpu); break; case 3: OpFpatan(cpu); break; case 6: OpFdecstp(cpu); break; case 7: OpFincstp(cpu); break; default: goto invalid_opcode; } break;
    case DISP(0xD9, FPUREG, 7): switch (cpu->rm) { case 1: OpFyl2xp1(cpu); break; case 2: OpFsqrt(cpu); break; case 6: OpFsin(cpu); break; default: goto invalid_opcode; } break;
    case DISP(0xD9, MEMORY, 0): OpFlds(cpu); break;
    case DISP(0xD9, MEMORY, 2): OpFsts(cpu); break;
    case DISP(0xD9, MEMORY, 3): OpFstps(cpu); break;
    case DISP(0xD9, MEMORY, 5): OpFldcw(cpu); break;
    case DISP(0xD9, MEMORY, 7): OpFstcw(cpu); break;
    case DISP(0xDA, MEMORY, 0): OpFiaddl(cpu); break;
    case DISP(0xDA, MEMORY, 1): OpFimull(cpu); break;
    case DISP(0xDA, MEMORY, 2): OpFicoml(cpu); break;
    case DISP(0xDA, MEMORY, 3): OpFicompl(cpu); break;
    case DISP(0xDA, MEMORY, 4): OpFisubl(cpu); break;
    case DISP(0xDA, MEMORY, 5): OpFisubrl(cpu); break;
    case DISP(0xDA, MEMORY, 6): OpFidivl(cpu); break;
    case DISP(0xDA, MEMORY, 7): OpFidivrl(cpu); break;
    case DISP(0xDA, FPUREG, 5): FpuPop(cpu); FpuPop(cpu); break;
    case DISP(0xDB, MEMORY, 0): OpFildl(cpu); break;
    case DISP(0xDB, MEMORY, 2): OpFistl(cpu); break;
    case DISP(0xDB, MEMORY, 3): OpFistpl(cpu); break;
    case DISP(0xDF, MEMORY, 3): OpFistps(cpu); break;
    case DISP(0xDB, MEMORY, 5): OpFldt(cpu); break;
    case DISP(0xDB, MEMORY, 7): OpFstpt(cpu); break;
    case DISP(0xDB, FPUREG, 4):
        switch (cpu->rm) {
        case 2: OpFnclex(cpu); break;
        case 3: OpFinit(cpu);  break;
        case 4: OpFsetpm(cpu); break;
        default: goto invalid_opcode;
        }
        break;
    case DISP(0xDC, MEMORY, 0): OpFaddl(cpu); break;
    case DISP(0xDC, MEMORY, 1): OpFmull(cpu); break;
    case DISP(0xDC, MEMORY, 2): FpuCompare(cpu, FpuGetMemoryDouble(cpu)); break;
    case DISP(0xDC, MEMORY, 3): FpuCompare(cpu, FpuGetMemoryDouble(cpu)); FpuPop(cpu); break;
    case DISP(0xDC, MEMORY, 4): OpFsubl(cpu); break;
    case DISP(0xDC, MEMORY, 5): OpFsubrl(cpu); break;
    case DISP(0xDC, MEMORY, 6): OpFdivl(cpu); break;
    case DISP(0xDC, MEMORY, 7): OpFdivrl(cpu); break;
    case DISP(0xDC, FPUREG, 0): OpFaddEstSt(cpu); break;
    case DISP(0xDC, FPUREG, 1): OpFmulEstSt(cpu); break;
    case DISP(0xDC, FPUREG, 4): OpFsubrEstSt(cpu); break;
    case DISP(0xDC, FPUREG, 5): OpFsubEstSt(cpu); break;
    case DISP(0xDC, FPUREG, 6): OpFdivrEstSt(cpu); break;
    case DISP(0xDC, FPUREG, 7): OpFdivEstSt(cpu); break;
    case DISP(0xDD, MEMORY, 0): OpFldl(cpu); break;
    case DISP(0xDD, MEMORY, 2): OpFstl(cpu); break;
    case DISP(0xDD, MEMORY, 3): OpFstpl(cpu); break;
    case DISP(0xDD, MEMORY, 4): OpFrstor(cpu); break;
    case DISP(0xDD, MEMORY, 7): OpFstswMw(cpu); break;
    case DISP(0xDD, FPUREG, 0): OpFfree(cpu); break;
    case DISP(0xDD, FPUREG, 2): OpFst(cpu); break;
    case DISP(0xDD, FPUREG, 3): OpFstp(cpu); break;
    case DISP(0xDE, MEMORY, 0): OpFiadds(cpu); break;
    case DISP(0xDE, MEMORY, 1): OpFimuls(cpu); break;
    case DISP(0xDE, MEMORY, 2): OpFicoms(cpu); break;
    case DISP(0xDE, MEMORY, 3): OpFicomps(cpu); break;
    case DISP(0xDE, MEMORY, 4): OpFisubs(cpu); break;
    case DISP(0xDE, MEMORY, 5): OpFisubrs(cpu); break;
    case DISP(0xDE, MEMORY, 6): OpFidivs(cpu); break;
    case DISP(0xDE, MEMORY, 7): OpFidivrs(cpu); break;
    case DISP(0xDE, FPUREG, 0): OpFaddp(cpu); break;
    case DISP(0xDE, FPUREG, 1): OpFmulp(cpu); break;
    case DISP(0xDE, FPUREG, 3): OpFcompp(cpu); break;
    case DISP(0xDE, FPUREG, 4): OpFsubrp(cpu); break;
    case DISP(0xDE, FPUREG, 5): OpFsubp(cpu); break;
    case DISP(0xDE, FPUREG, 6): OpFdivrp(cpu); break;
    case DISP(0xDE, FPUREG, 7): OpFdivp(cpu); break;
    case DISP(0xDF, MEMORY, 5): OpFildll(cpu); break;
    case DISP(0xDF, MEMORY, 7): OpFistpll(cpu); break;
    case DISP(0xDF, FPUREG, 4): OpFstswAx(cpu); break;
    default:
    invalid_opcode:
        debug_log(DEBUG_ERROR, "Invalid FPU Opcode at %04X:%04X: Opcode=0x%02X, ModRM=0x%02X (reg=%d, rm=%d, mod=%d)\n",
            cpu->savecs, cpu->saveip, cpu->opcode, modrm_byte, cpu->reg, cpu->rm, cpu->mode);
        break;
    }
}