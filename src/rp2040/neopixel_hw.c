// Support for driving "neopixel" type LEDs with the RPxxxx PIO hardware
//
// Copyright (C) 2026 Julian Schill <j.schill@web.de>
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include <string.h> // memcpy
#include "autoconf.h" // CONFIG_CLOCK_FREQ
#include "board/irq.h" // irq_poll
#include "board/misc.h" // timer_read_time, timer_is_before
#include "basecmd.h" // oid_alloc, oid_next, oid_lookup
#include "command.h" // DECL_COMMAND
#include "sched.h" // sched_shutdown
#include "hardware/regs/dma.h" // DMA_CH0_CTRL_TRIG_*
#include "hardware/regs/dreq.h" // DREQ_PIO0_TX0
#include "hardware/regs/pio.h" // PIO_SM0_* field definitions
#include "hardware/structs/dma.h" // dma_hw
#include "hardware/structs/pio.h" // pio0_hw, pio_hw_t
#include "hardware/structs/resets.h" // RESETS_RESET_PIO0_BITS
#include "internal.h" // enable_pclock, is_enabled_pclock, gpio_peripheral

// A PIO state machine generates the pulse pattern for each bit and DMA
// feeds it the framebuffer bytes, which uses no CPU time and cannot miss
// pulses. The program below is the pulse-generation example shipped with
// the RP2040 datasheet and pico-examples, with its documented broad-
// compatibility timing constants (T1=3, T2=3, T3=4), encoded as state-
// machine instructions. Its first instruction waits for data with the
// pin low, which is what leaves a long low gap between frames.
static const uint16_t neopixel_program[4] = {
    0x6321,     // out x, 1  side 0 [3]
    0x1223,     // jmp !x, 3 side 1 [2]
    0x1200,     // jmp 0     side 1 [2]
    0xA242,     // mov y, y  side 0 [2]
};

// 10 machine cycles per bit scaled to 1500ns/bit, within the neopixel
// pulse protocol (>=1250ns average bit time) and unambiguous pulses.
#define NEOPIXEL_CYCLES_PER_BIT 10
#define NEOPIXEL_BIT_NS         1500

// A bit of NEOPIXEL_BIT_NS in timer ticks, a compile-time constant so
// no run-time divide code is needed to scale frame lengths
#define NEOPIXEL_TICKS_PER_BIT \
        ((uint32_t)((uint64_t)NEOPIXEL_BIT_NS * CONFIG_CLOCK_FREQ \
         / 1000000000ull))

struct neopixel_hw_s {
    uint32_t pin;
    uint8_t sm;               // (pio_index << 2) | sm slot, also DMA channel
    uint32_t frame_end, reset_min_ticks;
    uint16_t data_size;
    uint8_t data[0];          // framebuffer, then a copy of it for DMA
};

void command_config_neopixel_hw(uint32_t *args);

// can.c hands all four state machines of the PIO0 block to the can2040
// library, so start with them claimed when CAN is enabled. The remaining
// machines and DMA channels are allocated from configured hardware LEDs
// rather than remembered statically, so config reprocessing cannot leave
// old claims behind.
static uint8_t
claim_sm(struct neopixel_hw_s *n)
{
    uint32_t used = (CONFIG_CANSERIAL || CONFIG_USBCANBUS) ? 0xfu : 0;
    uint8_t oid;
    struct neopixel_hw_s *o;
    foreach_oid(oid, o, command_config_neopixel_hw) {
        if (o != n)
            used |= 1u << o->sm;
    }
    for (uint8_t bit = 0; bit < 8; bit++) {
        if (used & (1u << bit))
            continue;
        return bit;
    }
    shutdown("No free PIO state machine");
    return 0;
}

static void
hw_setup(struct neopixel_hw_s *n)
{
    uint8_t pio_index = n->sm >> 2;
    uint8_t sm = n->sm & 3;
    pio_hw_t *pio = pio_index ? pio1_hw : pio0_hw;
    pio_sm_hw_t *smh = &pio->sm[sm];
    uint32_t pin = n->pin & 31;

    // Take the DMA and PIO units out of their power-up reset state; while
    // a unit is resetting its registers read zero and ignore writes
    if (!is_enabled_pclock(RESETS_RESET_DMA_BITS))
        enable_pclock(RESETS_RESET_DMA_BITS);
    if (!is_enabled_pclock(RESETS_RESET_PIO0_BITS))
        enable_pclock(RESETS_RESET_PIO0_BITS);
    if (!is_enabled_pclock(RESETS_RESET_PIO1_BITS))
        enable_pclock(RESETS_RESET_PIO1_BITS);

    // Load the bit-generation program into the state machine
    for (uint8_t i = 0; i < 4; i++)
        pio->instr_mem[i] = neopixel_program[i];

    // Drive the LED pin from the machine's side-set, and mark the pin
    // as written by PIO (set pindirs, 1)
    gpio_peripheral(n->pin, 6 + pio_index, 0);
    smh->pinctrl = (
            (1u << PIO_SM0_PINCTRL_SIDESET_COUNT_LSB)
            | (1u << PIO_SM0_PINCTRL_SET_COUNT_LSB)
            | (pin << PIO_SM0_PINCTRL_SIDESET_BASE_LSB)
            | (pin << PIO_SM0_PINCTRL_SET_BASE_LSB));
    smh->instr = 0xE081;     // set pindirs, 1

    // Pull one FIFO word at a time (a byte replicated by DMA over the
    // word), refilling every 8 bits, MSB first, joined with the RX FIFO
    smh->shiftctrl = (
            (1u << PIO_SM0_SHIFTCTRL_FJOIN_TX_LSB)
            | (1u << PIO_SM0_SHIFTCTRL_AUTOPULL_LSB)
            | (8u << PIO_SM0_SHIFTCTRL_PULL_THRESH_LSB));
    // Repeat the program forever
    smh->execctrl = 3u << PIO_SM0_EXECCTRL_WRAP_TOP_LSB;
    // Scale a machine cycle to 1/10 of NEOPIXEL_BIT_NS at the system clock,
    // in the 16.8 fixed-point clock divider format
    uint32_t sysclk = get_pclock_frequency(RESETS_RESET_PIO0_BITS);
    uint32_t div256 = (uint32_t)(
        (uint64_t)NEOPIXEL_BIT_NS * sysclk * 256
        / (1000000000ull * NEOPIXEL_CYCLES_PER_BIT));
    smh->clkdiv = ((div256 >> 8) << PIO_SM0_CLKDIV_INT_LSB)
                  | ((div256 & 0xff) << PIO_SM0_CLKDIV_FRAC_LSB);

    // Run the machine from now on; it stays put with the pin low until
    // DMA hands it the next frame's bytes
    pio->ctrl |= 1u << (PIO_CTRL_SM_ENABLE_LSB + sm);
}

static void
hw_send(struct neopixel_hw_s *n)
{
    uint8_t pio_index = n->sm >> 2;
    uint8_t sm = n->sm & 3;
    pio_hw_t *pio = pio_index ? pio1_hw : pio0_hw;

    // Wait until the frame before this one has left the machine and its
    // low gap was long enough for pixels to latch; frames apart in time
    // need no wait here
    uint32_t end = n->frame_end + n->reset_min_ticks;
    while (timer_is_before(timer_read_time(), end))
        irq_poll();

    // DMA reads a copy of the framebuffer, so an update arriving during
    // a transmission cannot change the picture half way through
    uint8_t *frame = n->data + n->data_size + 1;
    memcpy(frame, n->data, n->data_size + 1);

    // DMA writes the frame's bytes into the machine's TX FIFO, paced by
    // FIFO space; the trailing zero byte ends the frame low so pixels
    // latch
    dma_channel_hw_t *chan = &dma_hw->ch[n->sm];
    uint32_t treq = (pio_index ? DREQ_PIO1_TX0 : DREQ_PIO0_TX0) + sm;
    chan->read_addr = (uint32_t)(void*)frame;
    chan->write_addr = (uint32_t)(void*)&pio->txf[sm];
    chan->transfer_count = n->data_size + 1;
    chan->ctrl_trig = (
            (1u << DMA_CH0_CTRL_TRIG_EN_LSB)
            | (1u << DMA_CH0_CTRL_TRIG_INCR_READ_LSB)
            | (treq << DMA_CH0_CTRL_TRIG_TREQ_SEL_LSB));

    // Bytes are fed in faster than the machine consumes them, so allow a
    // bit of slack before the last one runs out
    n->frame_end = timer_read_time() + (uint32_t)(
        (n->data_size + 1) * 8 * NEOPIXEL_TICKS_PER_BIT
        + NEOPIXEL_TICKS_PER_BIT);
}

void
command_config_neopixel_hw(uint32_t *args)
{
    uint16_t data_size = args[2];
    if (data_size & 0x8000)
        shutdown("Invalid neopixel data_size");
    if (args[1] >= 32)
        // The state machines can only drive the GPIO 0..31 PAD registers
        shutdown("NEOPIXEL hardware cannot drive pins above gpio31");
    struct neopixel_hw_s *n = oid_alloc(args[0], command_config_neopixel_hw
                                        , sizeof(*n) + 2 * (data_size + 1));
    n->pin = args[1];
    n->data_size = data_size;
    n->reset_min_ticks = args[3];
    n->sm = claim_sm(n);
    n->data[data_size] = 0;
    hw_setup(n);
}
DECL_COMMAND(command_config_neopixel_hw,
             "config_neopixel_hw oid=%c pin=%u data_size=%hu"
             " reset_min_ticks=%u");

void
command_neopixel_hw_update(uint32_t *args)
{
    uint8_t oid = args[0];
    struct neopixel_hw_s *n = oid_lookup(oid, command_config_neopixel_hw);
    uint_fast16_t pos = args[1];
    uint_fast8_t data_len = args[2];
    uint8_t *data = command_decode_ptr(args[3]);
    if (pos & 0x8000 || pos + data_len > n->data_size)
        shutdown("Invalid neopixel update command");
    memcpy(&n->data[pos], data, data_len);
}
DECL_COMMAND(command_neopixel_hw_update,
             "neopixel_hw_update oid=%c pos=%hu data=%*s");

void
command_neopixel_hw_send(uint32_t *args)
{
    uint8_t oid = args[0];
    struct neopixel_hw_s *n = oid_lookup(oid, command_config_neopixel_hw);

    hw_send(n);
    sendf("neopixel_hw_result oid=%c success=%c", oid, 1);
}
DECL_COMMAND(command_neopixel_hw_send, "neopixel_hw_send oid=%c");
