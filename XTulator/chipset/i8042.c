#include <stdio.h>
#include <string.h>
#include "../config.h"
#include "../debuglog.h"
#include "../ports.h"
#include "i8042.h"

volatile uint8_t a20_enabled = 0;

void i8042_send_scancode(I8042_t* kbc, uint8_t scancode) {
    uint8_t next_head = (kbc->head + 1) % KBC_BUFFER_SIZE;
    if (next_head == kbc->tail) return;
    kbc->buffer[kbc->head] = scancode;
    kbc->head = next_head;
    if (!(kbc->status & 1)) {
        kbc->output_buffer = scancode;
        kbc->status |= 1;
        if (kbc->command_byte & 1) i8259_doirq(kbc->i8259, 1);
    }
}

void i8042_write(void* udata, uint32_t port, uint8_t value) {
    I8042_t* kbc = (I8042_t*)udata;
    if (port == 0x64) {
        kbc->status |= 2;
        kbc->command = value;
        switch (value) {
        case 0x20: i8042_send_scancode(kbc, kbc->command_byte); break;
        case 0xAA: i8042_send_scancode(kbc, 0x55); break;
        case 0xAD: kbc->command_byte |= 0x10; break;
        case 0xAE: kbc->command_byte &= ~0x10; break;
        case 0xA7: kbc->command_byte |= 0x20; break;
        case 0xA8: kbc->command_byte &= ~0x20; break;
        case 0xC0: i8042_send_scancode(kbc, 0x00); break;
        case 0xD0: i8042_send_scancode(kbc, kbc->output_port); break;
        case 0xE0: i8042_send_scancode(kbc, 0x00); break;
        case 0xFE: cpu_reset(kbc->cpu); break;
        }
        if (value != 0x60 && value != 0xD1 && value != 0xD3 && value != 0xD4) {
            kbc->status &= ~2;
        }
    }
    else if (port == 0x60) {
        if (kbc->command) {
            switch (kbc->command) {
            case 0x60: kbc->command_byte = value; break;
            case 0xD1: kbc->output_port = value; a20_enabled = (value >> 1) & 1; break;
            case 0xD3: break;
            case 0xD4:
                i8042_send_scancode(kbc, 0xFA);
                if (value == 0xFF) {
                    i8042_send_scancode(kbc, 0xAA);
                    i8042_send_scancode(kbc, 0x00);
                }
                break;
            }
            kbc->command = 0;
            kbc->status &= ~2;
        }
        else {
            i8042_send_scancode(kbc, 0xFA);
        }
    }
}

uint8_t i8042_read(void* udata, uint32_t port) {
    I8042_t* kbc = (I8042_t*)udata;
    if (port == 0x64) {
        return kbc->status;
    }
    else if (port == 0x60) {
        uint8_t data = kbc->output_buffer;
        if (kbc->head != kbc->tail) {
            kbc->output_buffer = kbc->buffer[kbc->tail];
            kbc->tail = (kbc->tail + 1) % KBC_BUFFER_SIZE;
        }
        else {
            kbc->status &= ~1;
        }
        if (kbc->head != kbc->tail && (kbc->command_byte & 1)) {
            i8259_doirq(kbc->i8259, 1);
        }
        kbc->status &= ~1;
        return data;
    }
    return 0xFF;
}

static uint8_t port92_data = 0;

void port92_write(void* udata, uint32_t port, uint8_t value) {
    port92_data = value;
    a20_enabled = (port92_data >> 1) & 1;
}

uint8_t port92_read(void* udata, uint32_t port) {
    return port92_data;
}

void i8042_init(I8042_t* kbc, CPU_t* cpu, I8259_t* i8259, KEYSTATE_t* keystate) {
    memset(kbc, 0, sizeof(I8042_t));
    kbc->keystate = keystate;
    kbc->cpu = cpu;
    kbc->i8259 = i8259;
    a20_enabled = 0;
    kbc->status = 0x14;
    kbc->command_byte = 0x45;
    kbc->output_port = 0xDD;
    kbc->input_port = 0x01;
    ports_cbRegister(0x60, 1, i8042_read, NULL, i8042_write, NULL, kbc);
    ports_cbRegister(0x64, 1, i8042_read, NULL, i8042_write, NULL, kbc);
}
