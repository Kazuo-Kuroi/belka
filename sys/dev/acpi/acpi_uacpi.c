/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-25
 * SPDX-License-Identifier: MIT
 */

/*
 * The kernel side of uACPI (sys/external/uacpi), the AML interpreter:
 * the services <uacpi/kernel_api.h> asks of a host, in Belka's terms.
 *
 *   memory          kva_map_phys, kmem (uACPI gives the size to free)
 *   PCI             configuration space by bus, device and function
 *   SystemIO        in and out, the handle being the base port
 *   time            gethrtime; sleeps by delay, stalls by spinning
 *   semaphores      struct sema; a timed wait polls with cpsema
 *   spinlocks       struct spinlock (which raises to splhi)
 *   the SCI         intr_establish, on its ISA IRQ as the MADT routes it
 *   deferred work   one kernel thread, acpi_work, runs it in order
 *
 * Locks: acpi_work_lock (a spin lock) covers the queue of work and the
 * count of work running; acpi_intr_inflight counts SCI handlers running.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/intr.h>
#include <sys/kmem.h>
#include <sys/kthread.h>
#include <sys/param.h>
#include <sys/spinlock.h>
#include <sys/synch.h>
#include <sys/systm.h>
#include <sys/time.h>

#include <machine/atomic.h>
#include <machine/bootinfo.h>
#include <machine/cpufunc.h>
#include <machine/intr.h>

#include <vm/hat.h>
#include <vm/kva.h>

#include <uacpi/kernel_api.h>

#include "dev/pci/pcivar.h"

/* --- Memory, logging. --- */

uacpi_status
uacpi_kernel_get_rsdp(uacpi_phys_addr *out)
{
        if (bootinfo.bi_rsdp == 0)
                return UACPI_STATUS_NOT_FOUND;
        *out = bootinfo.bi_rsdp;
        return UACPI_STATUS_OK;
}

void *
uacpi_kernel_map(uacpi_phys_addr addr, uacpi_size len)
{
        /*
         * Write-back, as the firmware's memory types (MTRRs) make device
         * memory uncached whatever the page tables say.
         */
        return kva_map_phys((paddr_t)addr, len == 0 ? 1 : len, HAT_WRITE);
}

void
uacpi_kernel_unmap(void *addr, uacpi_size len)
{
        kva_unmap_phys(addr, len == 0 ? 1 : len);
}

void
uacpi_kernel_log(uacpi_log_level level, const uacpi_char *fmt, ...)
{
        static const char *const names[] = {
                "", "error: ", "warning: ", "", "trace: ", "debug: "
        };
        va_list ap;

        printf("acpi: %s", (unsigned int)level < 6 ? names[level] : "");
        va_start(ap, fmt);
        vprintf(fmt, ap);
        va_end(ap);
}

void *
uacpi_kernel_alloc(uacpi_size size)
{
        return kmem_alloc(size, KM_SLEEP);
}

void
uacpi_kernel_free(void *mem, uacpi_size size)
{
        if (mem != NULL)
                kmem_free(mem, size);
}

/* --- PCI configuration space. --- */

struct acpi_pcidev {
        uint8_t         ap_bus;
        uint8_t         ap_dev;
        uint8_t         ap_fn;
};

uacpi_status
uacpi_kernel_pci_device_open(uacpi_pci_address addr, uacpi_handle *out)
{
        struct acpi_pcidev *ap;

        if (addr.segment != 0)
                return UACPI_STATUS_NOT_FOUND;
        ap = kmem_alloc(sizeof(*ap), KM_SLEEP);
        ap->ap_bus = addr.bus;
        ap->ap_dev = addr.device;
        ap->ap_fn = addr.function;
        *out = ap;
        return UACPI_STATUS_OK;
}

void
uacpi_kernel_pci_device_close(uacpi_handle h)
{
        kmem_free(h, sizeof(struct acpi_pcidev));
}

static uint32_t
acpi_pci_read(uacpi_handle h, uacpi_size off, int width)
{
        struct acpi_pcidev *ap = h;

        return pci_conf_read_bdf(ap->ap_bus, ap->ap_dev, ap->ap_fn, (int)off,
            width);
}

static void
acpi_pci_write(uacpi_handle h, uacpi_size off, int width, uint32_t v)
{
        struct acpi_pcidev *ap = h;

        pci_conf_write_bdf(ap->ap_bus, ap->ap_dev, ap->ap_fn, (int)off,
            width, v);
}

uacpi_status
uacpi_kernel_pci_read8(uacpi_handle h, uacpi_size off, uacpi_u8 *v)
{
        *v = (uacpi_u8)acpi_pci_read(h, off, 1);
        return UACPI_STATUS_OK;
}

uacpi_status
uacpi_kernel_pci_read16(uacpi_handle h, uacpi_size off, uacpi_u16 *v)
{
        *v = (uacpi_u16)acpi_pci_read(h, off, 2);
        return UACPI_STATUS_OK;
}

uacpi_status
uacpi_kernel_pci_read32(uacpi_handle h, uacpi_size off, uacpi_u32 *v)
{
        *v = acpi_pci_read(h, off, 4);
        return UACPI_STATUS_OK;
}

uacpi_status
uacpi_kernel_pci_write8(uacpi_handle h, uacpi_size off, uacpi_u8 v)
{
        acpi_pci_write(h, off, 1, v);
        return UACPI_STATUS_OK;
}

uacpi_status
uacpi_kernel_pci_write16(uacpi_handle h, uacpi_size off, uacpi_u16 v)
{
        acpi_pci_write(h, off, 2, v);
        return UACPI_STATUS_OK;
}

uacpi_status
uacpi_kernel_pci_write32(uacpi_handle h, uacpi_size off, uacpi_u32 v)
{
        acpi_pci_write(h, off, 4, v);
        return UACPI_STATUS_OK;
}

/* --- SystemIO: the handle is the base port. --- */

uacpi_status
uacpi_kernel_io_map(uacpi_io_addr base, uacpi_size len, uacpi_handle *out)
{
        if (base > 0xffff || base + len > 0x10000)
                return UACPI_STATUS_INVALID_ARGUMENT;
        *out = (uacpi_handle)(uintptr_t)(base + 1);     /* never NULL */
        return UACPI_STATUS_OK;
}

void
uacpi_kernel_io_unmap(uacpi_handle h)
{
        (void)h;
}

static uint16_t
acpi_port(uacpi_handle h, uacpi_size off)
{
        return (uint16_t)((uintptr_t)h - 1 + off);
}

uacpi_status
uacpi_kernel_io_read8(uacpi_handle h, uacpi_size off, uacpi_u8 *v)
{
        *v = inb(acpi_port(h, off));
        return UACPI_STATUS_OK;
}

uacpi_status
uacpi_kernel_io_read16(uacpi_handle h, uacpi_size off, uacpi_u16 *v)
{
        *v = inw(acpi_port(h, off));
        return UACPI_STATUS_OK;
}

uacpi_status
uacpi_kernel_io_read32(uacpi_handle h, uacpi_size off, uacpi_u32 *v)
{
        *v = inl(acpi_port(h, off));
        return UACPI_STATUS_OK;
}

uacpi_status
uacpi_kernel_io_write8(uacpi_handle h, uacpi_size off, uacpi_u8 v)
{
        outb(acpi_port(h, off), v);
        return UACPI_STATUS_OK;
}

uacpi_status
uacpi_kernel_io_write16(uacpi_handle h, uacpi_size off, uacpi_u16 v)
{
        outw(acpi_port(h, off), v);
        return UACPI_STATUS_OK;
}

uacpi_status
uacpi_kernel_io_write32(uacpi_handle h, uacpi_size off, uacpi_u32 v)
{
        outl(acpi_port(h, off), v);
        return UACPI_STATUS_OK;
}

/* --- Memory-mapped registers. --- */

uacpi_u8
uacpi_kernel_mmio_read8(void *p)
{
        return *(volatile uint8_t *)p;
}

uacpi_u16
uacpi_kernel_mmio_read16(void *p)
{
        return *(volatile uint16_t *)p;
}

uacpi_u32
uacpi_kernel_mmio_read32(void *p)
{
        return *(volatile uint32_t *)p;
}

uacpi_u64
uacpi_kernel_mmio_read64(void *p)
{
        return *(volatile uint64_t *)p;
}

void
uacpi_kernel_mmio_write8(void *p, uacpi_u8 v)
{
        *(volatile uint8_t *)p = v;
}

void
uacpi_kernel_mmio_write16(void *p, uacpi_u16 v)
{
        *(volatile uint16_t *)p = v;
}

void
uacpi_kernel_mmio_write32(void *p, uacpi_u32 v)
{
        *(volatile uint32_t *)p = v;
}

void
uacpi_kernel_mmio_write64(void *p, uacpi_u64 v)
{
        *(volatile uint64_t *)p = v;
}

/* --- Time. --- */

uacpi_u64
uacpi_kernel_get_nanoseconds_since_boot(void)
{
        return gethrtime();
}

void
uacpi_kernel_stall(uacpi_u8 usec)
{
        uint64_t end = gethrtime() + (uint64_t)usec * 1000;

        while (gethrtime() < end)
                cpu_spinwait();
}

void
uacpi_kernel_sleep(uacpi_u64 msec)
{
        long ticks = (long)((msec * HZ + 999) / 1000);

        if (ticks > 0)
                delay(ticks);
}

/* --- Semaphores, threads, spinlocks. --- */

uacpi_handle
uacpi_kernel_create_semaphore(uacpi_u32 units)
{
        struct sema *s = kmem_alloc(sizeof(*s), KM_SLEEP);

        sema_init(s, (int)units, "acpi");
        return s;
}

void
uacpi_kernel_free_semaphore(uacpi_handle h)
{
        kmem_free(h, sizeof(struct sema));
}

uacpi_status
uacpi_kernel_wait_for_semaphore(uacpi_handle h, uacpi_u16 timeout)
{
        struct sema *s = h;
        uint64_t end;

        if (timeout == 0xffff) {
                psema(s);
                return UACPI_STATUS_OK;
        }
        end = gethrtime() + (uint64_t)timeout * 1000000;
        for (;;) {
                if (cpsema(s))
                        return UACPI_STATUS_OK;
                if (timeout == 0 || gethrtime() >= end)
                        return UACPI_STATUS_TIMEOUT;
                delay(1);
        }
}

void
uacpi_kernel_signal_semaphore(uacpi_handle h)
{
        vsema(h);
}

uacpi_thread_id
uacpi_kernel_get_thread_id(void)
{
        return curthread;
}

uacpi_interrupt_state
uacpi_kernel_disable_interrupts(void)
{
        return (uacpi_interrupt_state)splhi();
}

void
uacpi_kernel_restore_interrupts(uacpi_interrupt_state state)
{
        splx((int)state);
}

uacpi_handle
uacpi_kernel_create_spinlock(void)
{
        struct spinlock *l = kmem_alloc(sizeof(*l), KM_SLEEP);

        spinlock_init(l, "acpi");
        return l;
}

void
uacpi_kernel_free_spinlock(uacpi_handle h)
{
        kmem_free(h, sizeof(struct spinlock));
}

uacpi_cpu_flags
uacpi_kernel_lock_spinlock(uacpi_handle h)
{
        return (uacpi_cpu_flags)spin_lock(h);
}

void
uacpi_kernel_unlock_spinlock(uacpi_handle h, uacpi_cpu_flags flags)
{
        spin_unlock(h, (int)flags);
}

uacpi_status
uacpi_kernel_handle_firmware_request(uacpi_firmware_request *req)
{
        if (req->type == UACPI_FIRMWARE_REQUEST_TYPE_FATAL)
                printf("acpi: the firmware reports a fatal error: type %u, "
                    "code %#x, argument %#jx\n", req->fatal.type,
                    req->fatal.code, (uintmax_t)req->fatal.arg);
        return UACPI_STATUS_OK;
}

/* --- The SCI. --- */

struct acpi_irq {
        uacpi_interrupt_handler ai_fn;
        uacpi_handle    ai_ctx;
        void            *ai_cookie;
};

static volatile uint32_t acpi_intr_inflight;

static int
acpi_intr(void *arg)
{
        struct acpi_irq *ai = arg;
        int handled;

        atomic_inc_32_nv(&acpi_intr_inflight);
        handled = ai->ai_fn(ai->ai_ctx) == UACPI_INTERRUPT_HANDLED;
        atomic_dec_32_nv(&acpi_intr_inflight);
        return handled;
}

uacpi_status
uacpi_kernel_install_interrupt_handler(uacpi_u32 irq,
    uacpi_interrupt_handler fn, uacpi_handle ctx, uacpi_handle *out)
{
        struct acpi_irq *ai = kmem_alloc(sizeof(*ai), KM_SLEEP);

        ai->ai_fn = fn;
        ai->ai_ctx = ctx;
        /* An ISA IRQ, as the MADT's overrides route it (the SCI: 9). */
        if (irq < 16)
                ai->ai_cookie = intr_establish((int)irq, INTR_ISA, acpi_intr,
                    ai, "acpi");
        else
                ai->ai_cookie = intr_establish((int)irq,
                    INTR_LEVEL | INTR_ACTIVE_LOW, acpi_intr, ai, "acpi");
        if (ai->ai_cookie == NULL) {
                kmem_free(ai, sizeof(*ai));
                return UACPI_STATUS_INTERNAL_ERROR;
        }
        *out = ai;
        return UACPI_STATUS_OK;
}

uacpi_status
uacpi_kernel_uninstall_interrupt_handler(uacpi_interrupt_handler fn,
    uacpi_handle h)
{
        struct acpi_irq *ai = h;

        (void)fn;
        intr_disestablish(ai->ai_cookie);
        kmem_free(ai, sizeof(*ai));
        return UACPI_STATUS_OK;
}

/* --- Deferred work: one thread, in order. --- */

struct acpi_work {
        struct acpi_work *aw_next;
        uacpi_work_handler aw_fn;
        uacpi_handle    aw_ctx;
};

static struct spinlock acpi_work_lock = SPINLOCK_INITIALIZER("acpiwork");
static struct sema acpi_work_sema;
static struct acpi_work *acpi_work_head, *acpi_work_tail;
static unsigned int acpi_work_running;
static int acpi_work_started;

static void
acpi_work_thread(void *arg __unused)
{
        for (;;) {
                struct acpi_work *aw;
                uacpi_work_handler fn;
                uacpi_handle ctx;
                int s;

                psema(&acpi_work_sema);
                s = spin_lock(&acpi_work_lock);
                aw = acpi_work_head;
                if (aw == NULL) {
                        spin_unlock(&acpi_work_lock, s);
                        continue;
                }
                if ((acpi_work_head = aw->aw_next) == NULL)
                        acpi_work_tail = NULL;
                /* Out of the item before the handler, which may reuse it. */
                fn = aw->aw_fn;
                ctx = aw->aw_ctx;
                acpi_work_running++;
                spin_unlock(&acpi_work_lock, s);
                fn(ctx);
                s = spin_lock(&acpi_work_lock);
                acpi_work_running--;
                spin_unlock(&acpi_work_lock, s);
        }
}

uacpi_handle
uacpi_kernel_create_work_item(void)
{
        if (!acpi_work_started) {
                acpi_work_started = 1;
                sema_init(&acpi_work_sema, 0, "acpiwork");
                (void)kthread_create(acpi_work_thread, NULL, PRI_KERNEL,
                    "acpiwork");
        }
        return kmem_zalloc(sizeof(struct acpi_work), KM_SLEEP);
}

void
uacpi_kernel_free_work_item(uacpi_handle h)
{
        kmem_free(h, sizeof(struct acpi_work));
}

void
uacpi_kernel_schedule_work(uacpi_work_type type, uacpi_handle item,
    uacpi_work_handler fn, uacpi_handle ctx)
{
        struct acpi_work *aw = item;
        int s;

        (void)type;
        aw->aw_fn = fn;
        aw->aw_ctx = ctx;
        aw->aw_next = NULL;
        s = spin_lock(&acpi_work_lock);
        if (acpi_work_tail != NULL)
                acpi_work_tail->aw_next = aw;
        else
                acpi_work_head = aw;
        acpi_work_tail = aw;
        spin_unlock(&acpi_work_lock, s);
        vsema(&acpi_work_sema);
}

uacpi_status
uacpi_kernel_wait_for_work_completion(void)
{
        for (;;) {
                int s, busy;

                if (atomic_load_32(&acpi_intr_inflight) != 0) {
                        delay(1);
                        continue;
                }
                s = spin_lock(&acpi_work_lock);
                busy = acpi_work_head != NULL || acpi_work_running != 0;
                spin_unlock(&acpi_work_lock, s);
                if (!busy)
                        return UACPI_STATUS_OK;
                delay(1);
        }
}
