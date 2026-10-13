// neopixel LED driven by STM32 timer + DMA hardware
//
// Copyright (C) 2026 Julian Schill <j.schill@web.de>
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include <string.h> // memcpy
#include "autoconf.h" // CONFIG_CLOCK_FREQ
#include "board/irq.h" // irq_poll
#include "board/misc.h" // timer_read_time, timer_is_before
#include "basecmd.h" // oid_alloc, oid_next, oid_lookup
#include "command.h" // shutdown, sendf, DECL_COMMAND
#include "sched.h" // sched_shutdown
#include "compiler.h" // ARRAY_SIZE
#include "gpio.h" // gpio_pwm_setup
#include "internal.h" // GPIO, enable_pclock, is_enabled_pclock

// A timer PWM channel draws one ws2812 pulse per counting period. hard_pwm.c
// sets its channels up so a value written into the address of a channel's
// compare register is applied by the hardware at the next period boundary: a
// DMA task paced by the timer's own update request writes one duty value from
// memory per bit, and its transfer count ends the frame. Bit periods come from
// the timer, never measured or waited for by the CPU; update-event requests are
// used instead of compare-match requests because they fire once per period even
// at zero or full duty. One hardware LED may use one timer's update feed: the
// table below lists distinct timer request sources and the stream that carries
// that source, so different timer sources can drive several LEDs at once while
// a second component asking for the same source is refused.
#define NEOPIXEL_BIT_NS         1500

// A bit of NEOPIXEL_BIT_NS in system clock ticks, a compile-time constant so
// no run-time divide code is needed to scale frame lengths
#define NEOPIXEL_TICKS_PER_BIT \
        ((uint32_t)((uint64_t)NEOPIXEL_BIT_NS * CONFIG_CLOCK_FREQ \
         / 1000000000ull))

// Long and short pulse duties as fractions of the bit period, which matches
// the ws2812 protocol's 2/3 and 1/3 timing within its tolerances
#define NEOPIXEL_PULSE_LONG_NUM 2
#define NEOPIXEL_PULSE_SHORT_NUM 1
#define NEOPIXEL_PULSE_DEN      3

// Timer channels able to feed a compare register from memory, paired with
// their pins; these rows mirror timer-mapped entries of hard_pwm.c and add
// the fixed feed slot for that timer's update request. The table intentionally
// uses one stream per update-event source: components on distinct timer
// request sources may run at the same time, while a second component asking
// for the same source is refused before it changes the shared timer. The
// stream/channel numbers come from the "DMA request mapping" tables of the
// reference manuals for the covered product lines; they agree across each
// listed F4xx and F7xx model, while models whose requests connect through a
// request multiplexer (G0x/G4x/H7x) or whose DMA controller predates this
// stream-and-channel programming model (F0x/F1x) have no fixed feed slot to
// name and stay out of this table.
static const struct neopixel_hw_map {
    TIM_TypeDef *tim;
    uint32_t pin;
    uint8_t channel;          // hard_pwm channel served by this row
    uint8_t task_index;       // index into led_dma_tasks
    uint32_t chsel;           // DMA_SxCR.CHSEL value selecting that event
} led_hw_regs[] = {
#if CONFIG_MACH_STM32F4
  #if CONFIG_MACH_STM32F401
    // TIM3_UP pins only mapped in the F401 block of hard_pwm.c
    {TIM3, GPIO('A',  6), 1, 2, 5},
    {TIM3, GPIO('C',  6), 1, 2, 5},
    {TIM3, GPIO('A',  7), 2, 2, 5},
    {TIM3, GPIO('C',  7), 2, 2, 5},
    {TIM3, GPIO('C',  8), 3, 2, 5},
    {TIM3, GPIO('C',  9), 4, 2, 5},
  #endif
  #if CONFIG_MACH_STM32F4x5 || CONFIG_MACH_STM32F446
    // TIM8_UP: one feed slot for any TIM8 channel, DMA2.Stream1 CH7
    {TIM8, GPIO('C',  6), 1, 9, 7},
    {TIM8, GPIO('C',  7), 2, 9, 7},
    {TIM8, GPIO('C',  8), 3, 9, 7},
    {TIM8, GPIO('C',  9), 4, 9, 7},
  #endif
    // The rows below map the pins hard_pwm.c defines for every klipper STM32F4
    // target; their feed numbers match the request table shared by that whole
    // F4xx line, as already verified on F446
    // TIM1_UP: one feed slot for any TIM1 channel, DMA2.Stream5 CH6
    {TIM1, GPIO('A',  8), 1, 13, 6},
    {TIM1, GPIO('A',  9), 2, 13, 6},
    {TIM1, GPIO('A', 10), 3, 13, 6},
    {TIM1, GPIO('A', 11), 4, 13, 6},
    {TIM1, GPIO('E',  9), 1, 13, 6},
    {TIM1, GPIO('E', 11), 2, 13, 6},
    {TIM1, GPIO('E', 13), 3, 13, 6},
    {TIM1, GPIO('E', 14), 4, 13, 6},
    // TIM2_UP: one feed slot for any TIM2 channel, DMA1.Stream1 CH3
    {TIM2, GPIO('A',  5), 1, 1, 3},
    {TIM2, GPIO('A', 15), 1, 1, 3},
  #if CONFIG_MACH_STM32F446
    // TIM2 B2 is mapped by hard_pwm.c only on F446
    {TIM2, GPIO('B',  2), 4, 1, 3},
  #endif
    {TIM2, GPIO('B',  3), 2, 1, 3},
    {TIM2, GPIO('B', 10), 3, 1, 3},
    {TIM2, GPIO('B', 11), 4, 1, 3},
    // TIM3_UP: one feed slot for any TIM3 channel, DMA1.Stream2 CH5
    {TIM3, GPIO('B',  0), 3, 2, 5},
    {TIM3, GPIO('B',  1), 4, 2, 5},
    {TIM3, GPIO('B',  4), 1, 2, 5},
    {TIM3, GPIO('B',  5), 2, 2, 5},
    // TIM4_UP: one feed slot for any TIM4 channel, DMA1.Stream6 CH2
    {TIM4, GPIO('B',  6), 1, 6, 2},
    {TIM4, GPIO('D', 12), 1, 6, 2},
    {TIM4, GPIO('B',  7), 2, 6, 2},
    {TIM4, GPIO('D', 13), 2, 6, 2},
    {TIM4, GPIO('D', 14), 3, 6, 2},
    {TIM4, GPIO('D', 15), 4, 6, 2},
    // TIM5_UP: one feed slot for any TIM5 channel, DMA1.Stream0 CH6
    {TIM5, GPIO('A',  0), 1, 0, 6},
    {TIM5, GPIO('A',  1), 2, 0, 6},
    {TIM5, GPIO('A',  2), 3, 0, 6},
    {TIM5, GPIO('A',  3), 4, 0, 6},
#elif CONFIG_MACH_STM32F7
    // F7xx rows mirror the hard_pwm.c F7 pin table; these request feeds use
    // the same stream/channel numbers as the F4xx table above (RM0410 lists
    // TIM2_UP at DMA1.Stream1 CH3, TIM3_UP at DMA1.Stream2 CH5 and TIM5_UP
    // at DMA1.Stream0 CH6 for the F76x/F77x line)
    {TIM2, GPIO('A', 15), 1, 1, 3},
    {TIM2, GPIO('B',  3), 2, 1, 3},
    {TIM2, GPIO('B', 10), 3, 1, 3},
    {TIM2, GPIO('B', 11), 4, 1, 3},
    {TIM3, GPIO('B',  4), 1, 2, 5},
    {TIM3, GPIO('B',  5), 1, 2, 5},
    {TIM3, GPIO('C',  8), 2, 2, 5},
    {TIM3, GPIO('C',  9), 2, 2, 5},
    {TIM5, GPIO('A', 10), 3, 0, 6},
    {TIM5, GPIO('E', 13), 3, 0, 6},
    {TIM5, GPIO('A', 11), 4, 0, 6},
    {TIM5, GPIO('E', 14), 4, 0, 6}
#endif
};

// DMA tasks which feed the frames; nothing else in klipper uses these units
static DMA_Stream_TypeDef *led_dma_tasks[16] = {
    DMA1_Stream0, DMA1_Stream1, DMA1_Stream2, DMA1_Stream3,
    DMA1_Stream4, DMA1_Stream5, DMA1_Stream6, DMA1_Stream7,
    DMA2_Stream0, DMA2_Stream1, DMA2_Stream2, DMA2_Stream3,
    DMA2_Stream4, DMA2_Stream5, DMA2_Stream6, DMA2_Stream7
};

void command_config_neopixel_hw(uint32_t *args);

struct neopixel_hw_s {
    TIM_TypeDef *tim;
    DMA_TypeDef *dm;                // controller owning the feed stream
    uint32_t pin, frame_end, reset_min_ticks;
    uint16_t dcr_word;          // compare register word offset, claim key
    uint16_t ccr_long, ccr_short;
    uint32_t ccr_addr;          // channel's compare register, for the feed
    uint8_t task_index, channel;
    uint32_t chsel;             // fixed request source for this timer
    uint16_t data_size;
    uint8_t data[0];  // framebuffer, then a per-bit compare area
};

static void
dcr_word_for(TIM_TypeDef *tim, uint8_t channel, uint32_t *dcr_word)
{
    uint32_t reg;
    switch (channel) {
        case 1: reg = (uint32_t)(void*)&tim->CCR1; break;
        case 2: reg = (uint32_t)(void*)&tim->CCR2; break;
        case 3: reg = (uint32_t)(void*)&tim->CCR3; break;
        default: reg = (uint32_t)(void*)&tim->CCR4; break;
    }
    *dcr_word = (reg - (uint32_t)(void*)tim) >> 2;
}

// Refuse another hardware LED taking this timer channel or feed stream.
static void
check_free(struct neopixel_hw_s *n, TIM_TypeDef *tim, uint8_t channel,
           uint8_t task_index)
{
    uint32_t dcr_word;
    dcr_word_for(tim, channel, &dcr_word);
    uint8_t oid;
    struct neopixel_hw_s *o;
    foreach_oid(oid, o, command_config_neopixel_hw) {
        if (o == n)
            continue;
        uint32_t other_dcr;
        dcr_word_for(o->tim, o->channel, &other_dcr);
        if (o->tim == tim && other_dcr == dcr_word)
            shutdown("Timer channel in use");
        if (o->task_index == task_index)
            shutdown("Hardware LED feed in use");
    }
}

static void
hw_setup(struct neopixel_hw_s *n)
{
    const struct neopixel_hw_map *p = led_hw_regs;
    for (;; p++) {
        if (p >= &led_hw_regs[ARRAY_SIZE(led_hw_regs)])
            shutdown("Hardware LED needs a timer-mapped pin");
        if (p->pin == n->pin)
            break;
    }
    TIM_TypeDef *tim = p->tim;
    if (!is_enabled_pclock((uint32_t)tim))
        enable_pclock((uint32_t)tim);
    check_free(n, tim, p->channel, p->task_index);

    // Set up the PWM output channel with one period per LED bit, initially
    // off so the pin waits low until the first frame is fed
    struct gpio_pwm chan = gpio_pwm_setup(n->pin, NEOPIXEL_TICKS_PER_BIT, 0);
    uint32_t hwpwm = chan.hwpwm_ticks;
    if (hwpwm < 3 * NEOPIXEL_PULSE_DEN)
        shutdown("LED bit period too short");

    // This timer generation has no transfer window for feeding a channel:
    // DMA writes the channel's compare register itself, so the register
    // offset identifies both the channel and where the fed values land;
    // use it as the claim key. Confirm that the table row agrees with the
    // address gpio_pwm_setup selected.
    uint32_t dcr_word;
    dcr_word_for(tim, p->channel, &dcr_word);
    uint32_t ccr_addr = (uint32_t)(void*)chan.reg;
    if (ccr_addr != ((dcr_word << 2) + (uint32_t)(void*)tim))
        shutdown("Hardware LED table mismatch");
    n->tim = tim;
    n->channel = p->channel;
    n->dcr_word = dcr_word;
    n->ccr_addr = ccr_addr;
    n->ccr_long = hwpwm * NEOPIXEL_PULSE_LONG_NUM / NEOPIXEL_PULSE_DEN;
    n->ccr_short = hwpwm * NEOPIXEL_PULSE_SHORT_NUM / NEOPIXEL_PULSE_DEN;

    // Pace the feed with this timer's update requests: they occur once per
    // counting period whatever the duty is, while compare-match requests
    // can be skipped at trivial duties like zero
    tim->DIER |= TIM_DIER_UDE;

    // The DMA controllers are clock-gated devices like everything else in
    // this address space: their registers stay unwritable until their line
    // is enabled. Their window addresses do not map through the enable
    // table used for the other devices, so set the controller's enable bit
    // by its documented name here; as upstream does when enabling any other
    // clock line, read it back once before touching the stream and confirm
    // that the requested bit really took.
    uint32_t want = RCC_AHB1ENR_DMA1EN;
    if (p->task_index & 8u)
        want = RCC_AHB1ENR_DMA2EN;
    RCC->AHB1ENR |= want;
    uint32_t enabled = 0;
    uint32_t tries = 16;
    while (tries--) {
        if ((RCC->AHB1ENR & want) == want) {
            enabled = 1;
            break;
        }
    }
    if (!enabled)
        shutdown("Hardware LED DMA clock unavailable");

    DMA_TypeDef *dm = (p->task_index & 8u) ? DMA2 : DMA1;
    n->dm = dm;
    n->task_index = p->task_index;
    n->chsel = p->chsel;
}

static void
hw_send(struct neopixel_hw_s *n)
{
    // Wait until the frame before this one has left the timer and its low
    // gap was long enough for pixels to latch; frames apart in time need no
    // wait here
    uint32_t end = n->frame_end + n->reset_min_ticks;
    while (timer_is_before(timer_read_time(), end))
        irq_poll();

    // Decode the bits into compare values, MSB first like the ws2812
    // protocol; rebuilding the area from the framebuffer each time means an
    // update arriving now cannot change the picture half way through. The
    // compare area holds one half-word per bit and starts at the first
    // aligned address after the framebuffer
    uint32_t at = (uint32_t)(void*)(n->data + n->data_size);
    uint16_t *feed = (uint16_t*)(void*)((at + 1u) & ~1u);
    uint_fast16_t entries = 0;
    for (uint16_t i = 0; i < n->data_size; i++) {
        uint8_t v = n->data[i];
        for (uint_fast8_t bit = 0; bit < 8; bit++) {
            uint16_t duty = ((v >> (7 - bit)) & 1) ? n->ccr_long : n->ccr_short;
            feed[entries++] = duty;
        }
    }
    // The final value holds the output through the frame end, so nothing
    // else in the timer is mistaken for a pulse between frames
    feed[entries++] = 0;

    // DMA hands the compare values to the timer paced by the update events,
    // writing each one into the channel's own register. The documented
    // sequence is followed: clear only this stream's enable bit, wait until
    // that bit really reads back clear, clear old completion/error flags for
    // this stream, program PAR/M0AR/NDTR while idle, then make one final CR
    // write with the configuration plus EN. No completion flag of a previous
    // frame can be mistaken for this one. The FIFO is not used, so its error
    // flag does not need clearing; this mask covers transfer-complete, DMA
    // error and the related non-FIFO flags for the one stream.
    DMA_Stream_TypeDef *task = led_dma_tasks[n->task_index];
    uint32_t en = 1u << DMA_SxCR_EN_Pos;
    task->CR &= ~en;
    uint32_t idle = 0;
    uint32_t tries = 16;
    while (tries--) {
        if ((task->CR & en) == 0) {
            idle = 1;
            break;
        }
    }
    if (!idle)
        shutdown("Hardware LED feed still running");
    static const uint32_t led_flag_base[] = {0u, 6u, 16u, 22u};
    uint32_t sidx = n->task_index & 7u;
    uint32_t fbits = 0xFu << (led_flag_base[sidx & 3u] + 2u);
    if (sidx >= 4u)
        n->dm->HIFCR = fbits;
    else
        n->dm->LIFCR = fbits;
    task->M0AR = (uint32_t)(void*)feed;
    task->NDTR = entries;
    task->PAR = n->ccr_addr;
    // The stream listens only to this timer's requests and moves half-words
    // through the compare area into the channel register: the channel's
    // compare window is 16 bits wide, so each request hands exactly one
    // absolute value to it
    uint32_t cr = (n->chsel << DMA_SxCR_CHSEL_Pos)
                | (1u << DMA_SxCR_DIR_Pos)      // memory to device
                | (1u << DMA_SxCR_MINC_Pos)     // advance through the area
                | (1u << DMA_SxCR_MSIZE_Pos)    // half-word sized
                | (1u << DMA_SxCR_PSIZE_Pos);   // matching the window width
    // The address and count registers were written above with the stream
    // taken down; one write now sets the configuration and starts counting
    task->CR = cr | (1u << DMA_SxCR_EN_Pos);

    n->frame_end = timer_read_time() + entries * NEOPIXEL_TICKS_PER_BIT
                   + NEOPIXEL_TICKS_PER_BIT;
}

void
command_config_neopixel_hw(uint32_t *args)
{
    uint16_t data_size = args[2];
    if (data_size & 0x8000)
        shutdown("Invalid neopixel data_size");
    uint32_t feed_bytes = 2u * (data_size * 8u + 1u);
    if (feed_bytes > 16384)
        // One compare half-word per bit is 16 bytes per pixel byte; keep
        // the frame buffer within a sane share of the MCU memory
        shutdown("neopixel hardware chain too long");
    struct neopixel_hw_s *n = oid_alloc(args[0], command_config_neopixel_hw
                                , sizeof(*n) + data_size + 4 + feed_bytes);
    n->pin = args[1];
    n->data_size = data_size;
    n->reset_min_ticks = args[3];
    n->frame_end = 0;
    n->dcr_word = 0;
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
