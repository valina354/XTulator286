#ifndef _FPU_H_
#define _FPU_H_

#include <stdint.h>



// FPU tag word values
#define kFpuTagValid   0
#define kFpuTagZero    1
#define kFpuTagSpecial 2
#define kFpuTagEmpty   3

// FPU Control Word bits
#define kFpuCwIm 0x0001 /* invalid operation mask */
#define kFpuCwDm 0x0002 /* denormal operand mask */
#define kFpuCwZm 0x0004 /* zero divide mask */
#define kFpuCwOm 0x0008 /* overflow mask */
#define kFpuCwUm 0x0010 /* underflow mask */
#define kFpuCwPm 0x0020 /* precision mask */
#define kFpuCwPc 0x0300 /* precision: 32,∅,64,80 */
#define kFpuCwRc 0x0c00 /* rounding: even,→-∞,→+∞,→0 */

// FPU Status Word bits
#define kFpuSwIe 0x0001 /* invalid operation */
#define kFpuSwDe 0x0002 /* denormalized operand */
#define kFpuSwZe 0x0004 /* zero divide */
#define kFpuSwOe 0x0008 /* overflow */
#define kFpuSwUe 0x0010 /* underflow */
#define kFpuSwPe 0x0020 /* precision */
#define kFpuSwSf 0x0040 /* stack fault */
#define kFpuSwEs 0x0080 /* exception summary status */
#define kFpuSwC0 0x0100 /* condition 0 */
#define kFpuSwC1 0x0200 /* condition 1 */
#define kFpuSwC2 0x0400 /* condition 2 */
#define kFpuSwSp 0x3800 /* top of stack pointer */
#define kFpuSwC3 0x4000 /* condition 3 */
#define kFpuSwBf 0x8000 /* busy flag */

// Main FPU struct
typedef struct {
    double st[8]; // Stack registers
    uint16_t cw;  // Control Word
    uint16_t sw;  // Status Word
    uint16_t tw;  // Tag Word
    uint32_t ip;  // Instruction Pointer
    uint16_t cs;  // Code Segment
    uint32_t dp;  // Data Pointer
    uint16_t ds;  // Data Segment
    uint16_t op;  // Last Opcode
} FPU_t;

#define FpuSt(cpu, i) (&((cpu)->fpu.st[(((i) + (((cpu)->fpu.sw & kFpuSwSp) >> 11))) & 7]))

#endif /* _FPU_H_ */