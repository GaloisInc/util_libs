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
/* Implementation of a logical timer for pc99 platforms
 *
 * We try to use the HPET, but if that doesn't work we use the PIT.
 */
/* this should eventually go away, but we need IRQ_OFFSET from it for now */
#include <sel4/arch/constants.h>
#include <platsupport/plat/timer.h>
#include <platsupport/arch/tsc.h>
#include <platsupport/pmem.h>
#include <utils/util.h>
#include <platsupport/plat/acpi/acpi.h>
#include <platsupport/plat/hpet.h>

typedef enum {
    HPET,
    PIT
} pc99_timer_t;

typedef struct {
    pc99_timer_t type;
    /* we are either using the HPET or the PIT */
    union {
        struct {
             hpet_t device;
             pmem_region_t region;
             uint64_t period;
             hpet_config_t config;
        } hpet;
        struct {
            pit_t device;
            uint32_t freq;
        } pit;
    };
    ps_irq_t irq;
    ps_io_ops_t ops;
} pc99_ltimer_t;

static size_t get_num_irqs(void *data)
{
    assert(data != NULL);

    /* both PIT and HPET only have one irq */
    return 1;
}

static int get_nth_irq(void *data, size_t n, ps_irq_t *irq)
{

    assert(data != NULL);
    assert(irq != NULL);
    assert(n == 0);

    pc99_ltimer_t *pc99_ltimer = data;
    *irq = pc99_ltimer->irq;
    return 0;
}

static size_t get_num_pmems(void *data)
{
    assert(data != NULL);
    pc99_ltimer_t *pc99_ltimer = data;

    return pc99_ltimer->type == HPET ? 1 : 0;
}

static int hpet_ltimer_get_nth_pmem(void *data, size_t n, pmem_region_t *pmem)
{
    assert(data != NULL);
    assert(pmem != NULL);
    assert(n == 0);

    pc99_ltimer_t *pc99_ltimer = data;
    *pmem = pc99_ltimer->hpet.region;
    return 0;
}

static int pit_ltimer_handle_irq(void *data, ps_irq_t *irq)
{
    /* on pc99 the kernel handles everything we need for irqs */
    return 0;
}

static int hpet_ltimer_handle_irq(void *data, ps_irq_t *irq)
{
    /* our hpet driver doesn't do periodic timeouts, so emulate them here */
    pc99_ltimer_t *pc99_ltimer = data;
    if (pc99_ltimer->hpet.period > 0) {
        return hpet_set_timeout(&pc99_ltimer->hpet.device,
                hpet_get_time(&pc99_ltimer->hpet.device) + pc99_ltimer->hpet.period);
    }
    return 0;
}

static int hpet_ltimer_get_time(void *data, uint64_t *time)
{
    assert(data != NULL);
    assert(time != NULL);

    pc99_ltimer_t *pc99_ltimer = data;
    *time = hpet_get_time(&pc99_ltimer->hpet.device);
    return 0;
}

static int pit_ltimer_get_time(void *data, uint64_t *time)
{
    pc99_ltimer_t *pc99_ltimer = data;
    *time = tsc_get_time(pc99_ltimer->pit.freq);
    return 0;
}

static int get_resolution(void *data, uint64_t *resolution)
{
    return ENOSYS;
}

static int hpet_ltimer_set_timeout(void *data, uint64_t ns, timeout_type_t type)
{
    assert(data != NULL);
    pc99_ltimer_t *pc99_ltimer = data;

    if (type == TIMEOUT_PERIODIC) {
        pc99_ltimer->hpet.period = ns;
    } else {
        pc99_ltimer->hpet.period = 0;
    }

    if (type != TIMEOUT_ABSOLUTE) {
        ns += hpet_get_time(&pc99_ltimer->hpet.device);
    }

    return hpet_set_timeout(&pc99_ltimer->hpet.device, ns);
}

static int pit_ltimer_set_timeout(void *data, uint64_t ns, timeout_type_t type)
{
    assert(data != NULL);
    pc99_ltimer_t *pc99_ltimer = data;
    if (type == TIMEOUT_ABSOLUTE) {
        ns -= tsc_get_time(pc99_ltimer->pit.freq);
    }

    return pit_set_timeout(&pc99_ltimer->pit.device, ns, type == TIMEOUT_PERIODIC);
}

static int pit_ltimer_reset(void *data)
{
    assert(data != NULL);
    pc99_ltimer_t *pc99_ltimer = data;
    pit_cancel_timeout(&pc99_ltimer->pit.device);
    return 0;
}

static int hpet_ltimer_reset(void *data)
{
    assert(data != NULL);
    pc99_ltimer_t *pc99_ltimer = data;

    hpet_stop(&pc99_ltimer->hpet.device);
    hpet_start(&pc99_ltimer->hpet.device);
    pc99_ltimer->hpet.period = 0;
    return 0;
}

static void destroy(void *data)
{
    assert(data);

    pc99_ltimer_t *pc99_ltimer = data;

    if (pc99_ltimer->type == HPET && pc99_ltimer->hpet.config.vaddr) {
        hpet_stop(&pc99_ltimer->hpet.device);
        ps_pmem_unmap(&pc99_ltimer->ops, pc99_ltimer->hpet.region, pc99_ltimer->hpet.config.vaddr);
    } else {
        assert(pc99_ltimer->type == PIT);
        pit_cancel_timeout(&pc99_ltimer->pit.device);
    }

    ps_free(&pc99_ltimer->ops.malloc_ops, sizeof(pc99_ltimer), pc99_ltimer);
}

static inline void
ltimer_init_common(ltimer_t *ltimer, ps_io_ops_t ops)
{
    pc99_ltimer_t *pc99_ltimer = ltimer->data;
    pc99_ltimer->ops = ops;
    ltimer->destroy = destroy;
}

static int ltimer_hpet_init_internal(ltimer_t *ltimer, ps_io_ops_t ops)
{
    ltimer_init_common(ltimer, ops);

    /* map in the paddr */
    pc99_ltimer_t *pc99_ltimer = ltimer->data;
    pc99_ltimer->hpet.config.vaddr = ps_pmem_map(&ops, pc99_ltimer->hpet.region, false, PS_MEM_NORMAL);
    if (pc99_ltimer->hpet.config.vaddr == NULL) {
        return -1;
    }

    ltimer->handle_irq = hpet_ltimer_handle_irq;
    ltimer->get_time = hpet_ltimer_get_time;
    ltimer->get_resolution = get_resolution;
    ltimer->set_timeout = hpet_ltimer_set_timeout;
    ltimer->reset = hpet_ltimer_reset;

    int error = hpet_init(&pc99_ltimer->hpet.device, pc99_ltimer->hpet.config);
    if (!error) {
        error = hpet_start(&pc99_ltimer->hpet.device);
    }

    return error;
}

int ltimer_default_init(ltimer_t *ltimer, ps_io_ops_t ops)
{
    int error = ltimer_default_describe(ltimer, ops);
    if (error) {
        return error;
    }

    pc99_ltimer_t *pc99_ltimer = ltimer->data;
    if (pc99_ltimer->type == PIT) {
        return ltimer_pit_init(ltimer, ops);
    } else {
        assert(pc99_ltimer->type == HPET);
        return ltimer_hpet_init_internal(ltimer, ops);
    }
}

int ltimer_hpet_init(ltimer_t *ltimer, ps_io_ops_t ops, ps_irq_t irq, pmem_region_t region)
{
    int error = ltimer_hpet_describe(ltimer, ops, irq, region);
    if (error) {
        return error;
    }

    return ltimer_hpet_init_internal(ltimer, ops);
}

int ltimer_pit_init_freq(ltimer_t *ltimer, ps_io_ops_t ops, uint64_t freq)
{
    int error = ltimer_pit_describe(ltimer, ops);
    if (error) {
        return error;
    }

    ltimer_init_common(ltimer, ops);
    pc99_ltimer_t *pc99_ltimer = ltimer->data;

    ltimer->handle_irq = pit_ltimer_handle_irq;
    ltimer->get_time = pit_ltimer_get_time;
    ltimer->get_resolution = get_resolution;
    ltimer->set_timeout = pit_ltimer_set_timeout;
    ltimer->reset = pit_ltimer_reset;
    pc99_ltimer->pit.freq = freq;
    return pit_init(&pc99_ltimer->pit.device, ops.io_port_ops);
}

int ltimer_pit_init(ltimer_t *ltimer, ps_io_ops_t ops)
{
    int error = ltimer_pit_init_freq(ltimer, ops, 0);
    if (error) {
        return error;
    }

    /* now calculate the tsc freq */
    pc99_ltimer_t *pc99_ltimer = ltimer->data;
    pc99_ltimer->pit.freq = tsc_calculate_frequency_pit(&pc99_ltimer->pit.device);
    if (pc99_ltimer->pit.freq == 0) {
        ltimer_destroy(ltimer);
        return ENOSYS;
    }
    return 0;
}

int ltimer_default_describe(ltimer_t *ltimer, ps_io_ops_t ops)
{
    pmem_region_t hpet_region;
    acpi_t *acpi = acpi_init(ops.io_mapper);
    int error = hpet_parse_acpi(acpi, &hpet_region);

    if (!error) {
        ps_irq_t irq;
        error = ltimer_hpet_describe_with_region(ltimer, ops, hpet_region, &irq);
    }

    if (error) {
        /* HPET failed - use the pit */
        error = ltimer_pit_describe(ltimer, ops);
    }

    return error;
}

int ltimer_hpet_describe_with_region(ltimer_t *ltimer, ps_io_ops_t ops, pmem_region_t region, ps_irq_t *irq)
{
    /* try to map the HPET to query its properties */
    void *vaddr = ps_pmem_map(&ops, region, false, PS_MEM_NORMAL);
    if (vaddr == NULL) {
        return ENOSYS;
    }

    /* first try to use MSIs */
    if (hpet_supports_fsb_delivery(vaddr)) {
        irq->type = PS_MSI;
        irq->msi.pci_bus = 0;
        irq->msi.pci_dev = 0;
        irq->msi.pci_func = 0;
        irq->msi.handle = 0;
        irq->msi.vector = DEFAULT_HPET_MSI_VECTOR;
    } else {
     /* try a IOAPIC */
        irq->type = PS_IOAPIC;
        irq->ioapic.pin = FFS(hpet_ioapic_irq_delivery_mask(vaddr)) - 1;
        irq->ioapic.level = hpet_level(vaddr);
        irq->ioapic.polarity = irq->ioapic.level;
        irq->ioapic.ioapic = 0; /* TODO how to work these out properly */
        irq->ioapic.vector = 0; /* TODO how to work this out properly */
    }

    ps_pmem_unmap(&ops, region, vaddr);
    return ltimer_hpet_describe(ltimer, ops, *irq, region);
}

int ltimer_pit_describe(ltimer_t *ltimer, ps_io_ops_t ops)
{
    int error = ps_calloc(&ops.malloc_ops, 1, sizeof(pc99_ltimer_t), &ltimer->data);
    if (error) {
        return error;
    }

    pc99_ltimer_t *pc99_ltimer = ltimer->data;
    pc99_ltimer->type = PIT;
    pc99_ltimer->irq.irq.number = PIT_INTERRUPT;
    pc99_ltimer->irq.type = PS_INTERRUPT;
    ltimer->get_num_irqs = get_num_irqs;
    ltimer->get_num_pmems = get_num_pmems;
    ltimer->get_nth_irq = get_nth_irq;
    return 0;
}

int ltimer_hpet_describe(ltimer_t *ltimer, ps_io_ops_t ops, ps_irq_t irq, pmem_region_t region)
{
    int error = ps_calloc(&ops.malloc_ops, 1, sizeof(pc99_ltimer_t), &ltimer->data);
    if (error) {
        return error;
    }

    pc99_ltimer_t *pc99_ltimer = ltimer->data;
    pc99_ltimer->type = HPET;
    ltimer->get_num_irqs = get_num_irqs;
    ltimer->get_nth_irq = get_nth_irq;
    ltimer->get_num_pmems = get_num_pmems;
    ltimer->get_nth_pmem = hpet_ltimer_get_nth_pmem;

    pc99_ltimer->hpet.region = region;
    pc99_ltimer->hpet.config.irq = irq.type == PS_MSI ? irq.msi.vector + IRQ_OFFSET : irq.ioapic.pin;
    pc99_ltimer->hpet.config.ioapic_delivery = (irq.type == PS_IOAPIC);
    pc99_ltimer->irq = irq;

    return 0;
}
