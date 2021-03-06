/******************************************************************************
 * xen-x86_32.h
 *
 * Guest OS interface to x86 32-bit Xen.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Copyright (c) 2004-2007, K A Fraser
 */

#ifndef __XEN_PUBLIC_ARCH_X86_XEN_X86_32_H__
#define __XEN_PUBLIC_ARCH_X86_XEN_X86_32_H__

/*
 * Hypercall interface:
 *  Input:  %ebx, %ecx, %edx, %esi, %edi, %ebp (arguments 1-6)
 *  Output: %eax
 * Access is via hypercall page (set up by guest loader or via a Xen MSR):
 *  call hypercall_page + hypercall-number * 32
 * Clobbered: Argument registers (e.g., 2-arg hypercall clobbers %ebx,%ecx)
 */

#ifndef __ASSEMBLY__

struct arch_vcpu_info {
    UINTN cr2;
    UINTN pad[5]; /* sizeof(vcpu_info_t) == 64 */
};
typedef struct arch_vcpu_info arch_vcpu_info_t;

#endif /* !__ASSEMBLY__ */

#endif /* __XEN_PUBLIC_ARCH_X86_XEN_X86_32_H__ */

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
