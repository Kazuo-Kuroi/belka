/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 *
 * Copying between kernel and user memory.
 *
 * Each routine checks that the user range lies below the end of user
 * space, then sets curthread->kt_onfault to its recovery point before
 * touching it. A page fault on a user address with kt_onfault set is
 * resolved through the process's address space if it can be; if not,
 * trap() resumes at the recovery point, which returns EFAULT. All three
 * return 0 or an errno value.
 */

#include <machine/asm.h>
#include <machine/frameasm.h>

/* int copyin(const void *uaddr, void *kaddr, size_t len) */
ENTRY(copyin)
        movq    %rdi, %rax
        addq    %rdx, %rax
        jc      copy_efault
        movabsq $USER_VA_END, %r8
        cmpq    %r8, %rax
        ja      copy_efault
        movq    %gs:8, %r9
        leaq    copy_fault(%rip), %r8
        movq    %r8, KT_ONFAULT(%r9)
        xchgq   %rdi, %rsi
        movq    %rdx, %rcx
        rep movsb
        movq    $0, KT_ONFAULT(%r9)
        xorl    %eax, %eax
        ret
END(copyin)

/* int copyout(const void *kaddr, void *uaddr, size_t len) */
ENTRY(copyout)
        movq    %rsi, %rax
        addq    %rdx, %rax
        jc      copy_efault
        movabsq $USER_VA_END, %r8
        cmpq    %r8, %rax
        ja      copy_efault
        movq    %gs:8, %r9
        leaq    copy_fault(%rip), %r8
        movq    %r8, KT_ONFAULT(%r9)
        xchgq   %rdi, %rsi              /* movsb copies %rsi to %rdi */
        movq    %rdx, %rcx
        rep movsb
        movq    $0, KT_ONFAULT(%r9)
        xorl    %eax, %eax
        ret
END(copyout)

ENTRY(copy_fault)
        movq    %gs:8, %r9
        movq    $0, KT_ONFAULT(%r9)
copy_efault:
        movl    $ASM_EFAULT, %eax
        ret
END(copy_fault)

/*
 * int copyinstr(const char *uaddr, char *kaddr, size_t maxlen,
 *     size_t *done)
 *
 * Copy a string of at most maxlen bytes, terminator included; *done, if
 * not NULL, gets the number of bytes copied. ENAMETOOLONG if no
 * terminator was found within maxlen bytes.
 */
ENTRY(copyinstr)
        movq    %gs:8, %r9
        leaq    copystr_fault(%rip), %r8
        movq    %r8, KT_ONFAULT(%r9)
        movabsq $USER_VA_END, %r10
        xorl    %r11d, %r11d
1:
        cmpq    %rdx, %r11
        jae     3f
        cmpq    %r10, %rdi
        jae     copystr_fault
        movb    (%rdi), %al
        movb    %al, (%rsi)
        incq    %rdi
        incq    %rsi
        incq    %r11
        testb   %al, %al
        jnz     1b
        xorl    %eax, %eax
        jmp     4f
3:
        movl    $ASM_ENAMETOOLONG, %eax
4:
        movq    $0, KT_ONFAULT(%r9)
        testq   %rcx, %rcx
        jz      5f
        movq    %r11, (%rcx)
5:
        ret
END(copyinstr)

ENTRY(copystr_fault)
        movq    %gs:8, %r9
        movq    $0, KT_ONFAULT(%r9)
        movl    $ASM_EFAULT, %eax
        ret
END(copystr_fault)
