/*
 * Copyright 2017, Data61
 * Commonwealth Scientific and Industrial Research Organisation (CSIRO)
 * ABN 41 687 119 230.
 *
 * This software may be distributed and modified according to the terms of
 * the BSD 2-Clause license. Note that NO WARRANTY is provided.
 * See "LICENSE_BSD2.txt" for details.
 *
 * @TAG(DATA61_BSD)
 */

#include <stdio.h>
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <utils/util.h>

#include <platsupport/timer.h>
#include <platsupport/plat/timer.h>

#define CLKCTRL_EXT_NEDGE           BIT(6)
#define CLKCTRL_EXT_SRC_EN          BIT(5)
#define CLKCTRL_PRESCALE_VAL(N)     (((N) & 0xf) << 1) /* rate = clk_src/[2^(N+1)] */
#define CLKCTRL_GET_PRESCALE_VAL(v) (((v) >> 1) & 0xf)
#define CLKCTRL_PRESCALE_EN         BIT(0)
#define CLKCTRL_PRESCALE_MASK       (CLKCTRL_PRESCALE_VAL(0xf) | CLKCTRL_PRESCALE_EN)

#define CNTCTRL_WAVE_POL BIT(6)
#define CNTCTRL_WAVE_EN  BIT(5)
#define CNTCTRL_RST      BIT(4)
#define CNTCTRL_MATCH    BIT(3)
#define CNTCTRL_DECR     BIT(2)
#define CNTCTRL_INT      BIT(1)
#define CNTCTRL_STOP     BIT(0)

#define INT_EVENT_OVR    BIT(5)
#define INT_CNT_OVR      BIT(4)
#define INT_MATCH2       BIT(3)
#define INT_MATCH1       BIT(2)
#define INT_MATCH0       BIT(1)
#define INT_INTERVAL     BIT(0)

#define EVCTRL_OVR       BIT(2)
#define EVCTRL_LO        BIT(1)
#define EVCTRL_EN        BIT(0)

#define PRESCALE_MAX       0xf
#define PCLK_FREQ          111110000U

#define CNT_WIDTH 16
#define CNT_MAX (BIT(CNT_WIDTH) - 1)

/* Byte offsets into a field of ttc_tmr_regs_t for each ttc */
#define TTCX_TIMER1_OFFSET 0x0
#define TTCX_TIMER2_OFFSET 0x4
#define TTCX_TIMER3_OFFSET 0x8

struct ttc_tmr_regs {
    uint32_t clk_ctrl[3];   /* +0x00 */
    uint32_t cnt_ctrl[3];   /* +0x0C */
    uint32_t cnt_val[3];    /* +0x18 */
    uint32_t interval[3];   /* +0x24 */
    uint32_t match[3][3];   /* +0x30 */
    uint32_t int_sts[3];    /* +0x54 */
    uint32_t int_en[3];     /* +0x60 */
    uint32_t event_ctrl[3]; /* +0x6C */
    uint32_t event[3];      /* +0x78 */
};
typedef volatile struct ttc_tmr_regs ttc_tmr_regs_t;

static freq_t _ttc_clk_get_freq(clk_t* clk);
static freq_t _ttc_clk_set_freq(clk_t* clk, freq_t hz);
static void _ttc_clk_recal(clk_t* clk);
static clk_t* _ttc_clk_init(clk_t* clk);

static inline ttc_tmr_regs_t*
ttc_get_regs(ttc_t *ttc)
{
    return ttc->regs;
}

/****************** Clocks ******************/

static ttc_t*
ttc_clk_get_priv(clk_t* clk)
{
    return (ttc_t*)clk->priv;
}

/* FPGA PL Clocks */
static freq_t
_ttc_clk_get_freq(clk_t* clk)
{
    ttc_t *ttc = ttc_clk_get_priv(clk);
    ttc_tmr_regs_t* regs = ttc_get_regs(ttc);
    uint32_t clk_ctrl;
    freq_t fin, fout;
    /* Get the parent frequency */
    if (clk->parent) {
        fin = clk_get_freq(clk->parent);
    } else {
        fin = PCLK_FREQ;
    }
    /* Calculate fout */
    clk_ctrl = *regs->clk_ctrl;
    if (clk_ctrl & CLKCTRL_PRESCALE_EN) {
        fout = fin >> (CLKCTRL_GET_PRESCALE_VAL(clk_ctrl) + 1);
    } else {
        fout = fin;
    }
    /* Return */
    return fout;
}

static freq_t
_ttc_clk_set_freq(clk_t* clk, freq_t hz)
{
    ttc_t *ttc = ttc_clk_get_priv(clk);
    ttc_tmr_regs_t* regs = ttc_get_regs(ttc);
    uint32_t v;
    freq_t fin;
    int ps;
    /* Determine input clock frequency */
    if (clk->parent) {
        fin = clk_get_freq(clk->parent);
    } else {
        fin = PCLK_FREQ;
    }
    /* Find a prescale value */
    for (ps = 0; fin > hz; ps++, fin >>= 1);
    if (ps > PRESCALE_MAX) {
        return 0;
    }
    /* Configure the timer */
    v = regs->clk_ctrl[0] & ~CLKCTRL_PRESCALE_MASK;
    if (ps > 0) {
        v |= CLKCTRL_PRESCALE_EN | CLKCTRL_PRESCALE_VAL(ps - 1);
    } else {
        v &= ~CLKCTRL_PRESCALE_EN;
    }
    *regs->clk_ctrl = v;
    return clk_get_freq(clk);
}

static void
_ttc_clk_recal(clk_t* clk UNUSED)
{
    assert(0);
}

static clk_t*
_ttc_clk_init(clk_t* clk)
{
    return clk;
}

static inline freq_t
_ttc_get_freq(ttc_t *ttc)
{
    return ttc->freq;
}

static inline freq_t
_ttc_set_freq(ttc_t *ttc, freq_t hz)
{
    ttc->freq = clk_set_freq(&ttc->clk, hz);
    return ttc->freq;
}

/********************************************/

/* Computes the optimal clock frequency for interrupting after
 * a given period of time. This will be the highest frequency
 * such that starting from 0, the timer counter will reach its
 * maximum value in AT MOST the specified time.
 *
 * If no such frequency is supported by the clock (ie. the
 * requested time is too high) this returns ETIME. Returns 0
 * on success.
 *
 * If a frequency is found, the clock is reprogrammed to run
 * at that frequency. The number of ticks it will take for the
 * requested time to pass (ie. the interval) is computed and
 * returned via an argument (interval). */
static inline int
_ttc_set_freq_for_ns(ttc_t *ttc, uint64_t ns, uint64_t *interval)
{
    freq_t fin, f;
    uint64_t interval_value;

    /* Set the clock source frequency
     * 1 / (fin / max_cnt) > interval
     * fin < max_cnt / interval */
    f = freq_cycles_and_ns_to_hz(CNT_MAX, ns);
    fin = _ttc_set_freq(ttc, f);
    if (fin > f) {
        /* This happens when the requested time is so long that the clock can't
         * run slow enough. In this case, the clock driver reported the minimum
         * rate it can run at, and we can use that to calculate a maximum time.
         */
        ZF_LOGE("Timeout too big for timer, max %llu, got %llu\n",
                            freq_cycles_and_hz_to_ns(CNT_MAX, fin), ns);

        return ETIME;
    }

    interval_value = freq_ns_and_hz_to_cycles(ns, fin);

    assert(interval_value <= CNT_MAX);

    if (interval) {
        *interval = interval_value;
    }

    return 0;
}

int ttc_start(ttc_t *ttc)
{
    ttc_tmr_regs_t* regs = ttc_get_regs(ttc);
    *regs->cnt_ctrl &= ~CNTCTRL_STOP;
    return 0;
}

int ttc_stop(ttc_t *ttc)
{
    ttc_tmr_regs_t* regs = ttc_get_regs(ttc);
    *regs->cnt_ctrl |= CNTCTRL_STOP;
    ttc_handle_irq(ttc);
    return 0;
}

/* Set up the ttc to fire an interrupt every ns nanoseconds.
 * The first such interrupt may arrive before ns nanoseconds
 * have passed since calling. */
static int
_ttc_periodic(ttc_tmr_regs_t *regs, uint64_t interval)
{
    *regs->interval = interval;

    /* Interval mode: Continuously count from 0 to value in interval register,
     * triggering an interval interrupt and resetting the counter to 0
     * whenever the counter passes through 0. */
    *regs->cnt_ctrl |= CNTCTRL_INT;

    /* The INTERVAL interrupt is used in periodic mode. The only source of
     * interrupts will be when the counter passes through 0 after reaching
     * the value in the interval register. */
    *regs->int_en = INT_INTERVAL;

    return 0;
}

void ttc_handle_irq(ttc_t *ttc)
{
    ttc_tmr_regs_t* regs = ttc_get_regs(ttc);

    /* The MATCH0 interrupt is used in oneshot mode. It is enabled when a
     * oneshot function is called, and disabled here so only one interrupt
     * is triggered per call. */
    *regs->int_en &= ~INT_MATCH0;

    FORCE_READ(regs->int_sts); /* Clear on read */
}

uint64_t ttc_get_time(ttc_t *ttc)
{
    ttc_tmr_regs_t* regs = ttc_get_regs(ttc);
    uint32_t cnt = *regs->cnt_val;
    uint32_t fin = _ttc_get_freq(ttc);
    return freq_cycles_and_hz_to_ns(cnt, fin);
}

/* Set up the ttc to fire an interrupt ns nanoseconds after this
 * function is called. */
static int
_ttc_oneshot_relative(ttc_tmr_regs_t *regs, uint64_t interval)
{

    /* In overflow mode the ttc will continuously count up to 0xffff and reset to 0.
     * The ttc will be programmed to interrupt when the counter reaches
     * current_time + interval, allowing the addition to wrap around (16 bits).
     */

    *regs->match[0] = (interval + *regs->cnt_val) % BIT(CNT_WIDTH);

    /* Overflow mode: Continuously count from 0 to 0xffff (this is a 16 bit ttc).
     * In this mode no interrval interrupts. A match interrupt (MATCH0) will be used
     * in this mode. */
    *regs->cnt_ctrl &= ~CNTCTRL_INT;

    /* The MATCH0 interrupt is used in oneshot mode. The only source of interrupts
     * will be when the counter passes through the value in the match[0] register.
     * This interrupt is disabled in the irq handler so it is only triggered once.
     */
    *regs->int_en = INT_MATCH0;

    return 0;
}

int ttc_set_timeout(ttc_t *ttc, uint64_t ns, bool periodic)
{
    if (ttc == NULL) {
        return EINVAL;
    }

    /* Program the clock and compute the interval value */
    uint64_t interval;
    int error = _ttc_set_freq_for_ns(ttc, ns, &interval);
    if (error) {
        return error;
    }

    ttc_tmr_regs_t* regs = ttc_get_regs(ttc);
    if (periodic) {
        return _ttc_periodic(regs, interval);
    } else {
        return _ttc_oneshot_relative(regs, interval);
    }
}

int ttc_init(ttc_t *ttc, ttc_config_t config)
{
    /* This sets the base of the ttc_tmr_regs_t pointer to
     * an offset into the ttc's mmio region such that
     * ((ttc_tmr_regs_t*)vaddr)->clk_ctrl
     * (and all other registers) refers to the address of the
     * register relevant for the specified ttc device. */
    switch (config.id) {
    case TTC0_TIMER1:
    case TTC1_TIMER1:
        config.vaddr += TTCX_TIMER1_OFFSET;
        break;
    case TTC0_TIMER2:
    case TTC1_TIMER2:
        config.vaddr += TTCX_TIMER2_OFFSET;
        break;
    case TTC0_TIMER3:
    case TTC1_TIMER3:
        config.vaddr += TTCX_TIMER3_OFFSET;
        break;
    default:
        return EINVAL;
    }

    ttc->regs = config.vaddr;
    ttc->id = config.id;

    /* Configure clock source */
    memset(&ttc->clk, 0, sizeof(ttc->clk));
    ttc->clk.name = STRINGIFY(config.id);
    ttc->clk.get_freq = _ttc_clk_get_freq;
    ttc->clk.set_freq = _ttc_clk_set_freq;
    ttc->clk.recal = _ttc_clk_recal;
    ttc->clk.init = _ttc_clk_init;
    ttc->clk.priv = ttc;

    if (config.clk_src) {
        clk_register_child(config.clk_src, &ttc->clk);
    }
    ttc->freq = clk_get_freq(&ttc->clk);

    ttc_tmr_regs_t *regs = ttc_get_regs(ttc);
    *regs->int_en = 0;
    FORCE_READ(regs->int_sts); /* Clear on read */
    *regs->cnt_ctrl = CNTCTRL_RST | CNTCTRL_STOP | CNTCTRL_INT | CNTCTRL_MATCH;
    *regs->clk_ctrl = 0;
    *regs->int_en = INT_INTERVAL;
    *regs->interval = CNT_MAX;

    return 0;
}
