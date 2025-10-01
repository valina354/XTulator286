#ifndef _I8042_H_
#define _I8042_H_

#include <stdint.h>
#include "../cpu/cpu.h"
#include "../chipset/i8259.h"
#include "../modules/input/input.h"

#define KBC_BUFFER_SIZE 16 

extern volatile uint8_t a20_enabled;

typedef struct {
    uint8_t status;
    uint8_t command;
    uint8_t output_port;
    uint8_t output_buffer;
    uint8_t command_byte;
    uint8_t input_port;
    uint8_t buffer[KBC_BUFFER_SIZE];
    uint8_t head;
    uint8_t tail;
    KEYSTATE_t* keystate;
    CPU_t* cpu;
    I8259_t* i8259;
} I8042_t;

void i8042_init(I8042_t* kbc, CPU_t* cpu, I8259_t* i8259, KEYSTATE_t* keystate);
void i8042_send_scancode(I8042_t* kbc, uint8_t scancode);
uint8_t i8042_read(void* udata, uint32_t port);
void i8042_write(void* udata, uint32_t port, uint8_t value);

#endif