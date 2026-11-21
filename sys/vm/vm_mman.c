/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * Memory management system calls: brk, mmap, munmap, and mprotect.
 *
 * mmap places a mapping without MAP_FIXED at the first free range at or
 * above the hint, or above VM_MMAP_BASE, finding and mapping it in one
 * locked step.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/errno.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/proc.h>
#include <sys/sysent.h>
#include <sys/systm.h>
#include <sys/vnode.h>

#include <machine/param.h>
#include <machine/vmparam.h>

#include <vm/as.h>
#include <vm/seg_vn.h>

/*
 * brk(addr): set the break. Below the start of the heap (the end of the
 * program's data) is ENOMEM, as in UnixWare. So is growing past
 * RLIMIT_DATA, or past RLIMIT_VMEM in all.
 */
int
sys_brk(const uint64_t *args, uint64_t *rv __unused)
{
        struct proc *p = curproc;
        struct as *as = p->p_as;
        vaddr_t addr = (vaddr_t)args[0];
        size_t newsize;
        int error;

        mutex_lock(&as->a_brklock);
        mutex_lock(&p->p_lock);
        if (addr < as->a_brkbase || addr >= USRSTACK - VM_STACK_SIZE) {
                mutex_unlock(&p->p_lock);
                mutex_unlock(&as->a_brklock);
                return ENOMEM;
        }
        newsize = pround(addr) - as->a_brkbase;
        if ((rlim_t)newsize > p->p_rlimit[RLIMIT_DATA].rlim_cur ||
            (newsize > as->a_brksize &&
            p->p_rlimit[RLIMIT_VMEM].rlim_cur != RLIM_INFINITY &&
            (rlim_t)(as_size(p->p_as) + newsize - as->a_brksize) >
            p->p_rlimit[RLIMIT_VMEM].rlim_cur)) {
                mutex_unlock(&p->p_lock);
                mutex_unlock(&as->a_brklock);
                return ENOMEM;
        }
        error = as_brk(p->p_as, as->a_brkbase, as->a_brksize, newsize);
        if (error == 0)
                as->a_brksize = newsize;
        mutex_unlock(&p->p_lock);
        mutex_unlock(&as->a_brklock);
        return error;
}

/* mmap(addr, len, prot, flags, fd, off) */
int
sys_mmap(const uint64_t *args, uint64_t *rv)
{
        struct proc *p = curproc;
        vaddr_t addr = (vaddr_t)args[0];
        size_t len = (size_t)args[1];
        unsigned int prot = (unsigned int)args[2];
        int flags = (int)args[3], fd = (int)args[4];
        off_t off = (off_t)args[5];
        int type = flags & MAP_TYPE;
        struct segvn_crargs a;
        struct file *fp = NULL;
        rlim_t vmem;
        int error;

        if (len == 0 || (prot & ~PROT_ALL) != 0 ||
            (type != MAP_SHARED && type != MAP_PRIVATE) ||
            (flags & ~(MAP_TYPE | MAP_FIXED | MAP_ANON)) != 0 ||
            (off & PAGEOFFSET) != 0 || len > VM_USER_MAX)
                return EINVAL;
        len = pround(len);
        vmem = rlimit_cur(p, RLIMIT_VMEM);
        if (vmem != RLIM_INFINITY && (rlim_t)(as_size(p->p_as) + len) > vmem)
                return ENOMEM;
        a.type = type;
        a.prot = prot;
        a.maxprot = PROT_ALL;
        a.offset = off;
        a.vp = NULL;
        if ((flags & MAP_ANON) == 0) {
                if ((fp = fd_get(p, fd)) == NULL)
                        return EBADF;
                if (fp->f_vnode->v_type != VREG) {
                        frele(fp);
                        return ENODEV;
                }
                if ((fp->f_flag & FREAD) == 0) {
                        frele(fp);
                        return EACCES;
                }
                a.vp = fp->f_vnode;
                if (type == MAP_SHARED)
                        a.maxprot &= ~(unsigned int)PROT_WRITE;
        }
        if ((flags & MAP_FIXED) != 0) {
                if ((addr & PAGEOFFSET) != 0) {
                        error = EINVAL;
                        goto out;
                }
                (void)as_unmap(p->p_as, addr, len);
                error = as_map(p->p_as, addr, len, segvn_create, &a);
        } else {
                error = as_map_anywhere(p->p_as, len, addr, segvn_create, &a,
                    &addr);
        }
        if (error == 0)
                rv[0] = addr;
out:
        if (fp != NULL)
                frele(fp);      /* the segment holds the vnode */
        return error;
}

int
sys_munmap(const uint64_t *args, uint64_t *rv __unused)
{
        vaddr_t addr = (vaddr_t)args[0];

        if ((addr & PAGEOFFSET) != 0 || args[1] == 0)
                return EINVAL;
        return as_unmap(curproc->p_as, addr, pround((size_t)args[1]));
}

int
sys_mprotect(const uint64_t *args, uint64_t *rv __unused)
{
        vaddr_t addr = (vaddr_t)args[0];

        if ((addr & PAGEOFFSET) != 0 || args[1] == 0 ||
            (args[2] & ~(uint64_t)PROT_ALL) != 0)
                return EINVAL;
        return as_setprot(curproc->p_as, addr, pround((size_t)args[1]),
            (unsigned int)args[2]);
}
