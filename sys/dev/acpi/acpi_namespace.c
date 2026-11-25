/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-25
 * SPDX-License-Identifier: MIT
 */

/*
 * The ACPI namespace, through uACPI: loaded and initialized at boot
 * (acpi_namespace_init, from configure() before the PCI drivers attach),
 * with the I/O APIC interrupt model selected (\_PIC 1) and the general
 * purpose events turned on (the SCI).
 *
 * Then PCI interrupt routing: each PCI root bridge's _PRT (PNP0A03, and
 * PNP0A08 for PCI Express) is read once and kept, with the root's bus
 * number (_BBN, 0 if none). acpi_pci_route() answers for a device and
 * pin on a root bus: a GSI, either given directly or as the interrupt of
 * a link device (its current resources, _CRS), with its trigger and
 * polarity. Devices behind bridges are routed by pci.c, which swizzles
 * the pin up to the root bus.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/intr.h>
#include <sys/systm.h>

#include <uacpi/event.h>
#include <uacpi/resources.h>
#include <uacpi/sleep.h>
#include <uacpi/uacpi.h>
#include <uacpi/utilities.h>

#include "dev/acpi/acpivar.h"

#define ACPI_MAXROOTS   8

struct acpi_root {
        uint32_t        ar_bus;
        uacpi_pci_routing_table *ar_prt;
};

static struct acpi_root acpi_roots[ACPI_MAXROOTS];
static unsigned int acpi_nroots;
static int acpi_ready;

static uacpi_iteration_decision
acpi_root_found(void *arg, uacpi_namespace_node *node, uacpi_u32 depth)
{
        struct acpi_root *ar;
        uacpi_u64 bbn = 0;

        (void)arg;
        (void)depth;
        if (acpi_nroots == ACPI_MAXROOTS)
                return UACPI_ITERATION_DECISION_BREAK;
        ar = &acpi_roots[acpi_nroots];
        (void)uacpi_eval_simple_integer(node, "_BBN", &bbn);
        if (uacpi_get_pci_routing_table(node, &ar->ar_prt) != UACPI_STATUS_OK)
                ar->ar_prt = NULL;
        ar->ar_bus = (uint32_t)bbn;
        printf("acpi: PCI root bus %u, %zu routing entries\n", ar->ar_bus,
            ar->ar_prt != NULL ? (size_t)ar->ar_prt->num_entries : 0);
        acpi_nroots++;
        return UACPI_ITERATION_DECISION_CONTINUE;
}

void
acpi_namespace_init(void)
{
        static const uacpi_char *const roots[] = {
                "PNP0A03", "PNP0A08", UACPI_NULL
        };
        uacpi_status st;

        if ((st = uacpi_initialize(0)) != UACPI_STATUS_OK) {
                printf("acpi: cannot start uACPI: %s\n",
                    uacpi_status_to_string(st));
                return;
        }
        if ((st = uacpi_namespace_load()) != UACPI_STATUS_OK) {
                printf("acpi: cannot load the namespace: %s\n",
                    uacpi_status_to_string(st));
                return;
        }
        if ((st = uacpi_set_interrupt_model(UACPI_INTERRUPT_MODEL_IOAPIC)) !=
            UACPI_STATUS_OK)
                printf("acpi: cannot select the I/O APIC model: %s\n",
                    uacpi_status_to_string(st));
        if ((st = uacpi_namespace_initialize()) != UACPI_STATUS_OK) {
                printf("acpi: cannot initialize the namespace: %s\n",
                    uacpi_status_to_string(st));
                return;
        }
        if ((st = uacpi_finalize_gpe_initialization()) != UACPI_STATUS_OK)
                printf("acpi: cannot enable the events: %s\n",
                    uacpi_status_to_string(st));
        acpi_ready = 1;
        (void)uacpi_find_devices_at(uacpi_namespace_root(), roots,
            acpi_root_found, NULL);
}

int
acpi_namespace_ready(void)
{
        return acpi_ready;
}

/* What a link device's _CRS says of its interrupt number index. */
struct acpi_linkirq {
        uint32_t        al_index;
        uint32_t        al_seen;
        uint32_t        al_gsi;
        int             al_flags;
        int             al_found;
};

static int
acpi_irqflags(uacpi_u8 triggering, uacpi_u8 polarity)
{
        return (triggering == UACPI_TRIGGERING_LEVEL ? INTR_LEVEL : 0) |
            (polarity == UACPI_POLARITY_ACTIVE_LOW ? INTR_ACTIVE_LOW : 0);
}

static uacpi_iteration_decision
acpi_link_resource(void *arg, uacpi_resource *res)
{
        struct acpi_linkirq *al = arg;

        if (res->type == UACPI_RESOURCE_TYPE_IRQ) {
                uacpi_resource_irq *irq = &res->irq;

                for (uacpi_u8 i = 0; i < irq->num_irqs; i++, al->al_seen++)
                        if (al->al_seen == al->al_index) {
                                al->al_gsi = irq->irqs[i];
                                al->al_flags = acpi_irqflags(irq->triggering,
                                    irq->polarity);
                                al->al_found = 1;
                                return UACPI_ITERATION_DECISION_BREAK;
                        }
        } else if (res->type == UACPI_RESOURCE_TYPE_EXTENDED_IRQ) {
                uacpi_resource_extended_irq *irq = &res->extended_irq;

                for (uacpi_u8 i = 0; i < irq->num_irqs; i++, al->al_seen++)
                        if (al->al_seen == al->al_index) {
                                al->al_gsi = irq->irqs[i];
                                al->al_flags = acpi_irqflags(irq->triggering,
                                    irq->polarity);
                                al->al_found = 1;
                                return UACPI_ITERATION_DECISION_BREAK;
                        }
        }
        return UACPI_ITERATION_DECISION_CONTINUE;
}

int
acpi_pci_route(unsigned int bus, unsigned int dev, unsigned int pin,
    uint32_t *gsi, int *flags)
{
        for (unsigned int r = 0; r < acpi_nroots; r++) {
                uacpi_pci_routing_table *prt = acpi_roots[r].ar_prt;

                if (acpi_roots[r].ar_bus != bus || prt == NULL)
                        continue;
                for (uacpi_size i = 0; i < prt->num_entries; i++) {
                        uacpi_pci_routing_table_entry *e = &prt->entries[i];
                        struct acpi_linkirq al = { 0, 0, 0, 0, 0 };

                        if ((e->address >> 16) != dev || e->pin + 1u != pin)
                                continue;
                        if (e->source == NULL) {
                                /* A GSI itself: level, active low. */
                                *gsi = e->index;
                                *flags = INTR_LEVEL | INTR_ACTIVE_LOW;
                                return 0;
                        }
                        al.al_index = e->index;
                        if (uacpi_for_each_device_resource(e->source, "_CRS",
                            acpi_link_resource, &al) != UACPI_STATUS_OK ||
                            !al.al_found)
                                return -1;
                        *gsi = al.al_gsi;
                        *flags = al.al_flags;
                        return 0;
                }
        }
        return -1;
}

/*
 * Power off (ACPI S5), in two steps: the firmware is told first (_PTS
 * and the like, which run AML), from a thread with interrupts on;
 * acpi_poweroff() then turns the power off with them off. 0, or -1.
 */
int
acpi_poweroff_prepare(void)
{
        uacpi_status st;

        if (!acpi_ready)
                return -1;
        if ((st = uacpi_prepare_for_sleep_state(UACPI_SLEEP_STATE_S5)) !=
            UACPI_STATUS_OK) {
                printf("acpi: cannot prepare to power off: %s\n",
                    uacpi_status_to_string(st));
                return -1;
        }
        return 0;
}

void
acpi_poweroff(void)
{
        uacpi_status st = uacpi_enter_sleep_state(UACPI_SLEEP_STATE_S5);

        printf("acpi: cannot power off: %s\n", uacpi_status_to_string(st));
}
