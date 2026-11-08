/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * exec: replace the calling process's image with a program.
 *
 * Only statically linked ELF-64 executables for amd64 are run so far.
 * Each PT_LOAD is mapped privately from the file through seg_vn, so the
 * text is the page cache's own pages; the part of a page past p_filesz
 * is zeroed, and the rest of p_memsz is demand-zero. The stack is
 * VM_STACK_SIZE of demand-zero memory below USRSTACK, all mapped at once
 * as UnixWare does.
 *
 * The initial stack follows the AMD64 psABI, which UnixWare's own
 * layout matches: argc, the argv pointers and a NULL, the environment
 * pointers and a NULL, then the auxiliary vector (always present: the
 * psABI requires it even for static programs), then the strings. %rsp
 * points at argc and is 16-byte aligned.
 *
 * Errors before the new address space replaces the old one are returned
 * to the caller; after that point there is nothing to return to, and
 * the process is killed, as in UnixWare.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/cred.h>
#include <sys/elf.h>
#include <sys/errno.h>
#include <sys/kmem.h>
#include <sys/mman.h>
#include <sys/pathname.h>
#include <sys/proc.h>
#include <sys/signal.h>
#include <sys/sysent.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <sys/vnode.h>
#include <sys/wait.h>

#include <machine/intr.h>
#include <machine/md_var.h>
#include <machine/param.h>
#include <machine/vmparam.h>

#include <vm/as.h>
#include <vm/hat.h>
#include <vm/seg_vn.h>

#define ARG_MAX         (64 * 1024)     /* argument and environment bytes */
#define EXEC_MAXPHDR    32
#define NAUXV           8

struct execargs {
        char            *ea_buf;        /* argv strings, then env strings */
        size_t          ea_len;
        int             ea_argc;
        int             ea_envc;
};

struct execargs *
execargs_alloc(void)
{
        struct execargs *ea = kmem_zalloc(sizeof(*ea), KM_SLEEP);

        ea->ea_buf = kmem_alloc(ARG_MAX, KM_SLEEP);
        return ea;
}

void
execargs_free(struct execargs *ea)
{
        kmem_free(ea->ea_buf, ARG_MAX);
        kmem_free(ea, sizeof(*ea));
}

/* Append a kernel string, to argv or (isenv) to the environment. */
int
execargs_add(struct execargs *ea, const char *s, int isenv)
{
        size_t len = strlen(s) + 1;

        if (!isenv && ea->ea_envc != 0)
                panic("execargs_add: argument after the environment");
        if (len > ARG_MAX - ea->ea_len)
                return E2BIG;
        memcpy(ea->ea_buf + ea->ea_len, s, len);
        ea->ea_len += len;
        if (isenv)
                ea->ea_envc++;
        else
                ea->ea_argc++;
        return 0;
}

/* Append a user string. */
static int
execargs_copyin(struct execargs *ea, const char *us, int isenv)
{
        size_t done;
        int error;

        if (!isenv && ea->ea_envc != 0)
                panic("execargs_copyin: argument after the environment");
        error = copyinstr(us, ea->ea_buf + ea->ea_len, ARG_MAX - ea->ea_len,
            &done);
        if (error == ENAMETOOLONG)
                return E2BIG;
        if (error != 0)
                return error;
        ea->ea_len += done;
        if (isenv)
                ea->ea_envc++;
        else
                ea->ea_argc++;
        return 0;
}

/* Copy a user vector of string pointers, up to its NULL. */
int
execargs_copyin_vec(struct execargs *ea, char *const *uvec, int isenv)
{
        for (size_t i = 0; uvec != NULL; i++) {
                const char *us;
                int error;

                if ((error = copyin(&uvec[i], &us, sizeof(us))) != 0)
                        return error;
                if (us == NULL)
                        return 0;
                if ((error = execargs_copyin(ea, us, isenv)) != 0)
                        return error;
        }
        return 0;
}

static unsigned int
elf_prot(uint32_t flags)
{
        return ((flags & PF_R) != 0 ? PROT_READ : 0) |
            ((flags & PF_W) != 0 ? PROT_WRITE : 0) |
            ((flags & PF_X) != 0 ? PROT_EXEC : 0);
}

/* Read and check the ELF header and program headers of vp. */
static int
elf_read_headers(struct vnode *vp, Elf64_Ehdr *eh, Elf64_Phdr *ph)
{
        size_t resid;
        int error;

        if ((error = vn_rdwr(UIO_READ, vp, eh, sizeof(*eh), 0, 0,
            &resid)) != 0)
                return error;
        if (resid != 0 || eh->e_ident[0] != 0x7f || eh->e_ident[1] != 'E' ||
            eh->e_ident[2] != 'L' || eh->e_ident[3] != 'F' ||
            eh->e_ident[EI_CLASS] != ELFCLASS64 ||
            eh->e_ident[EI_DATA] != ELFDATA2LSB ||
            eh->e_machine != EM_X86_64 || eh->e_type != ET_EXEC ||
            eh->e_phentsize != sizeof(Elf64_Phdr) || eh->e_phnum == 0 ||
            eh->e_phnum > EXEC_MAXPHDR ||
            eh->e_entry < VM_USER_MIN || eh->e_entry >= VM_USER_MAX)
                return ENOEXEC;
        if ((error = vn_rdwr(UIO_READ, vp, ph, (size_t)eh->e_phnum *
            sizeof(*ph), (off_t)eh->e_phoff, 0, &resid)) != 0)
                return error;
        if (resid != 0)
                return ENOEXEC;
        for (int i = 0; i < eh->e_phnum; i++) {
                if (ph[i].p_type == PT_INTERP)
                        return ENOEXEC;         /* no dynamic linking yet */
        }
        return 0;
}

/*
 * Build the new address space: the program's segments and the stack.
 * *brkp gets the end of the highest segment, *phdrp the address of the
 * program headers in the image, or 0.
 */
static int
elf_map(struct as *as, struct vnode *vp, const Elf64_Ehdr *eh,
    const Elf64_Phdr *ph, vaddr_t *brkp, vaddr_t *phdrp)
{
        struct segvn_crargs a;
        vaddr_t brk = 0;
        int error;

        *phdrp = 0;
        for (int i = 0; i < eh->e_phnum; i++) {
                const Elf64_Phdr *p = &ph[i];
                vaddr_t vbase, fend, mend;
                off_t off;

                if (p->p_type != PT_LOAD || p->p_memsz == 0)
                        continue;
                if (p->p_filesz > p->p_memsz ||
                    (p->p_vaddr & PAGEOFFSET) != (p->p_offset & PAGEOFFSET) ||
                    p->p_vaddr < VM_USER_MIN ||
                    p->p_memsz > USRSTACK - VM_STACK_SIZE - p->p_vaddr)
                        return ENOEXEC;
                vbase = ptrunc(p->p_vaddr);
                off = (off_t)(p->p_offset - (p->p_vaddr - vbase));
                fend = p->p_vaddr + p->p_filesz;
                mend = p->p_vaddr + p->p_memsz;
                a.prot = elf_prot(p->p_flags);
                a.maxprot = PROT_ALL;
                a.type = MAP_PRIVATE;
                if (p->p_filesz != 0) {
                        a.vp = vp;
                        a.offset = off;
                        if ((error = as_map(as, vbase, pround(fend) - vbase,
                            segvn_create, &a)) != 0)
                                return error;
                        /* A partial last page is zeroed past filesz. */
                        if (mend > fend && (fend & PAGEOFFSET) != 0 &&
                            (a.prot & PROT_WRITE) == 0)
                                return ENOEXEC;
                }
                if (pround(mend) > pround(fend)) {
                        vaddr_t zbase = p->p_filesz != 0 ? pround(fend) :
                            vbase;

                        a.vp = NULL;
                        a.offset = 0;
                        if ((error = as_map(as, zbase, pround(mend) - zbase,
                            segvn_create, &a)) != 0)
                                return error;
                }
                if (p->p_offset <= eh->e_phoff && eh->e_phoff +
                    (uint64_t)eh->e_phnum * sizeof(Elf64_Phdr) <=
                    p->p_offset + p->p_filesz)
                        *phdrp = p->p_vaddr + (eh->e_phoff - p->p_offset);
                if (pround(mend) > brk)
                        brk = pround(mend);
        }
        if (brk == 0)
                return ENOEXEC;
        a.vp = NULL;
        a.offset = 0;
        a.type = MAP_PRIVATE;
        a.prot = PROT_READ | PROT_WRITE;
        a.maxprot = PROT_ALL;
        if ((error = as_map(as, USRSTACK - VM_STACK_SIZE, VM_STACK_SIZE,
            segvn_create, &a)) != 0)
                return error;
        *brkp = brk;
        return 0;
}

/* Zero the parts of partial last pages past p_filesz, in the new image. */
static int
elf_zero_tails(const Elf64_Ehdr *eh, const Elf64_Phdr *ph)
{
        static const char zeros[PAGESIZE];

        for (int i = 0; i < eh->e_phnum; i++) {
                const Elf64_Phdr *p = &ph[i];
                vaddr_t fend = p->p_vaddr + p->p_filesz;
                int error;

                if (p->p_type != PT_LOAD || p->p_filesz == 0 ||
                    p->p_memsz <= p->p_filesz || (fend & PAGEOFFSET) == 0)
                        continue;
                if ((error = copyout(zeros, (void *)(uintptr_t)fend,
                    pround(fend) - fend)) != 0)
                        return error;
        }
        return 0;
}

/* Lay out and copy the initial stack. Returns the user %rsp, or 0. */
static uint64_t
exec_stack(const struct execargs *ea, const Elf64_Ehdr *eh, vaddr_t phdr)
{
        size_t nptr = 1 + (size_t)ea->ea_argc + 1 + (size_t)ea->ea_envc + 1;
        size_t vecsize = nptr * 8 + NAUXV * sizeof(Elf64_auxv_t);
        vaddr_t strbase = (USRSTACK - ea->ea_len) & ~(vaddr_t)15;
        vaddr_t sp = (strbase - vecsize) & ~(vaddr_t)15;
        uint64_t *vec = kmem_zalloc(vecsize, KM_SLEEP);
        Elf64_auxv_t *aux;
        size_t n = 0, off = 0;
        int error;

        vec[n++] = (uint64_t)ea->ea_argc;
        for (int i = 0; i < ea->ea_argc + ea->ea_envc; i++) {
                if (i == ea->ea_argc)
                        vec[n++] = 0;
                vec[n++] = strbase + off;
                off += strlen(ea->ea_buf + off) + 1;
        }
        if (ea->ea_envc == 0 && ea->ea_argc >= 0)
                vec[n++] = 0;
        vec[n++] = 0;
        aux = (Elf64_auxv_t *)(void *)&vec[n];
        aux[0].a_type = AT_PHDR;
        aux[0].a_val = phdr;
        aux[1].a_type = AT_PHENT;
        aux[1].a_val = sizeof(Elf64_Phdr);
        aux[2].a_type = AT_PHNUM;
        aux[2].a_val = eh->e_phnum;
        aux[3].a_type = AT_PAGESZ;
        aux[3].a_val = PAGESIZE;
        aux[4].a_type = AT_ENTRY;
        aux[4].a_val = eh->e_entry;
        aux[5].a_type = AT_BASE;
        aux[5].a_val = 0;
        aux[6].a_type = AT_FLAGS;
        aux[6].a_val = 0;
        aux[7].a_type = AT_NULL;
        aux[7].a_val = 0;

        error = copyout(ea->ea_buf, (void *)(uintptr_t)strbase, ea->ea_len);
        if (error == 0)
                error = copyout(vec, (void *)(uintptr_t)sp, vecsize);
        kmem_free(vec, vecsize);
        return error == 0 ? sp : 0;
}

/*
 * Run the program at path (a kernel string) in the calling process,
 * with the arguments in ea, which is freed. Returns an error only if the
 * old image is intact; on success the LWP's registers are those of the
 * new program.
 */
int
exec_common(const char *path, struct execargs *ea)
{
        struct lwp *l = curlwp;
        struct proc *p = l->l_proc;
        Elf64_Ehdr eh;
        Elf64_Phdr *ph = kmem_alloc(EXEC_MAXPHDR * sizeof(*ph), KM_SLEEP);
        struct vnode *vp;
        struct as *nas, *oas;
        vaddr_t brk, phdr;
        uint64_t sp;
        const char *base = path;
        int error, s;

        if ((error = lookupname(path, FOLLOW, &vp)) != 0)
                goto fail;
        if (vp->v_type != VREG) {
                error = EACCES;
                goto fail_vp;
        }
        if ((error = VOP_ACCESS(vp, VEXEC, kcred)) != 0 ||
            (error = elf_read_headers(vp, &eh, ph)) != 0)
                goto fail_vp;
        nas = as_alloc();
        if ((error = elf_map(nas, vp, &eh, ph, &brk, &phdr)) != 0) {
                as_free(nas);
                goto fail_vp;
        }

        /* The point of no return: switch to the new image. */
        s = splhi();
        oas = p->p_as;
        p->p_as = nas;
        hat_activate(nas->a_hat);
        splx(s);
        if (oas != NULL)
                as_free(oas);
        VN_RELE(vp);            /* the mappings hold it */

        if (elf_zero_tails(&eh, ph) != 0 ||
            (sp = exec_stack(ea, &eh, phdr)) == 0) {
                kmem_free(ph, EXEC_MAXPHDR * sizeof(*ph));
                execargs_free(ea);
                proc_fault(SIGKILL, 0);
        }
        mutex_lock(&p->p_lock);
        p->p_brkbase = brk;
        p->p_brksize = 0;
        for (const char *c = path; *c != '\0'; c++) {
                if (*c == '/')
                        base = c + 1;
        }
        memset(p->p_comm, 0, PSCOMSIZ);
        for (size_t i = 0; i + 1 < PSCOMSIZ && base[i] != '\0'; i++)
                p->p_comm[i] = base[i];
        mutex_unlock(&p->p_lock);
        fd_close_exec(p);
        md_setregs(l, eh.e_entry, sp);
        kmem_free(ph, EXEC_MAXPHDR * sizeof(*ph));
        execargs_free(ea);
        return 0;

fail_vp:
        VN_RELE(vp);
fail:
        kmem_free(ph, EXEC_MAXPHDR * sizeof(*ph));
        execargs_free(ea);
        return error;
}

/* execve(path, argv, envp) and exec(path, argv). */
static int
exec_syscall(const char *upath, char *const *uargv, char *const *uenvp)
{
        char *path = kmem_alloc(MAXPATHLEN, KM_SLEEP);
        struct execargs *ea = execargs_alloc();
        int error;

        if ((error = copyinstr(upath, path, MAXPATHLEN, NULL)) != 0 ||
            (error = execargs_copyin_vec(ea, uargv, 0)) != 0 ||
            (error = execargs_copyin_vec(ea, uenvp, 1)) != 0) {
                execargs_free(ea);
                kmem_free(path, MAXPATHLEN);
                return error;
        }
        error = exec_common(path, ea);
        kmem_free(path, MAXPATHLEN);
        return error != 0 ? error : EJUSTRETURN;
}

int
sys_execve(const uint64_t *args, uint64_t *rv __unused)
{
        return exec_syscall((const char *)(uintptr_t)args[0],
            (char *const *)(uintptr_t)args[1],
            (char *const *)(uintptr_t)args[2]);
}

int
sys_exec(const uint64_t *args, uint64_t *rv __unused)
{
        return exec_syscall((const char *)(uintptr_t)args[0],
            (char *const *)(uintptr_t)args[1], NULL);
}
