/* $Id$ */
/** @file
 * CSAM - Guest OS Code Scanning and Analysis Manager - Any Context
 */

/*
 * Copyright (C) 2006-2007 Sun Microsystems, Inc.
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 *
 * Please contact Sun Microsystems, Inc., 4150 Network Circle, Santa
 * Clara, CA 95054 USA or visit http://www.sun.com if you need
 * additional information or have any questions.
 */


/*******************************************************************************
*   Header Files                                                               *
*******************************************************************************/
#define LOG_GROUP LOG_GROUP_CSAM
#include <VBox/cpum.h>
#include <VBox/stam.h>
#include <VBox/patm.h>
#include <VBox/csam.h>
#include <VBox/pgm.h>
#include <VBox/mm.h>
#include <VBox/sup.h>
#include <VBox/mm.h>
#include <VBox/rem.h>
#include <VBox/param.h>
#include <iprt/avl.h>
#include "CSAMInternal.h"
#include <VBox/vm.h>
#include <VBox/dbg.h>
#include <VBox/err.h>
#include <VBox/log.h>
#include <iprt/assert.h>
#include <VBox/dis.h>
#include <VBox/disopcode.h>
#include <iprt/string.h>
#include <iprt/asm.h>

/**
 * \#PF Handler callback for virtual access handler ranges. (CSAM self-modifying
 * code monitor)
 *
 * Important to realize that a physical page in a range can have aliases, and
 * for ALL and WRITE handlers these will also trigger.
 *
 * @returns VBox status code (appropriate for GC return).
 * @param   pVM         VM Handle.
 * @param   uErrorCode   CPU Error code.
 * @param   pRegFrame   Trap register frame.
 * @param   pvFault     The fault address (cr2).
 * @param   pvRange     The base address of the handled virtual range.
 * @param   offRange    The offset of the access into this range.
 *                      (If it's a EIP range this's the EIP, if not it's pvFault.)
 */
VMMRCDECL(int) CSAMGCCodePageWriteHandler(PVM pVM, RTGCUINT uErrorCode, PCPUMCTXCORE pRegFrame, RTGCPTR pvFault, RTGCPTR pvRange, uintptr_t offRange)
{
    PPATMGCSTATE pPATMGCState;
    bool         fPatchCode = PATMIsPatchGCAddr(pVM, pRegFrame->eip);
    int          rc;
    PVMCPU       pVCpu = VMMGetCpu0(pVM);

    Assert(pVM->csam.s.cDirtyPages < CSAM_MAX_DIRTY_PAGES);

    /* Flush the recompilers translation block cache as the guest seems to be modifying instructions. */
    REMFlushTBs(pVM);

    pPATMGCState = PATMQueryGCState(pVM);
    Assert(pPATMGCState);

    Assert(pPATMGCState->fPIF || fPatchCode);
    /** When patch code is executing instructions that must complete, then we must *never* interrupt it. */
    if (!pPATMGCState->fPIF && fPatchCode)
    {
        Log(("CSAMGCCodePageWriteHandler: fPIF=0 -> stack fault in patch generated code at %08RX32!\n", pRegFrame->eip));
        /** @note there are cases when pages previously used for code are now used for stack; patch generated code will fault (pushf))
         *  Just make the page r/w and continue.
         */
        /*
         * Make this particular page R/W.
         */
        rc = PGMShwModifyPage(pVCpu, pvFault, 1, X86_PTE_RW, ~(uint64_t)X86_PTE_RW);
        AssertMsgRC(rc, ("PGMShwModifyPage -> rc=%Rrc\n", rc));
        ASMInvalidatePage((void *)pvFault);
        return VINF_SUCCESS;
    }

    uint32_t cpl;

    if (pRegFrame->eflags.Bits.u1VM)
        cpl = 3;
    else
        cpl = (pRegFrame->ss & X86_SEL_RPL);

    Log(("CSAMGCCodePageWriteHandler: code page write at %RGv original address %RGv (cpl=%d)\n", pvFault, (RTGCUINTPTR)pvRange + offRange, cpl));

    /* If user code is modifying one of our monitored pages, then we can safely make it r/w as it's no longer being used for supervisor code. */
    if (cpl != 3)
    {
        rc = PATMGCHandleWriteToPatchPage(pVM, pRegFrame, (RTRCPTR)((RTRCUINTPTR)pvRange + offRange), 4 /** @todo */);
        if (rc == VINF_SUCCESS)
            return rc;
        if (rc == VINF_EM_RAW_EMULATE_INSTR)
        {
            STAM_COUNTER_INC(&pVM->csam.s.StatDangerousWrite);
            return VINF_EM_RAW_EMULATE_INSTR;
        }
        Assert(rc == VERR_PATCH_NOT_FOUND);
    }

    VMCPU_FF_SET(pVCpu, VMCPU_FF_CSAM_PENDING_ACTION);

    /* Note that pvFault might be a different address in case of aliases. So use pvRange + offset instead!. */
    pVM->csam.s.pvDirtyBasePage[pVM->csam.s.cDirtyPages] = (RTRCPTR)((RTGCUINTPTR)pvRange + offRange);
    pVM->csam.s.pvDirtyFaultPage[pVM->csam.s.cDirtyPages] = (RTRCPTR)((RTGCUINTPTR)pvRange + offRange);
    if (++pVM->csam.s.cDirtyPages == CSAM_MAX_DIRTY_PAGES)
        return VINF_CSAM_PENDING_ACTION;

    /*
     * Make this particular page R/W. The VM_FF_CSAM_FLUSH_DIRTY_PAGE handler will reset it to readonly again.
     */
    Log(("CSAMGCCodePageWriteHandler: enabled r/w for page %RGv\n", pvFault));
    rc = PGMShwModifyPage(pVCpu, pvFault, 1, X86_PTE_RW, ~(uint64_t)X86_PTE_RW);
    AssertMsgRC(rc, ("PGMShwModifyPage -> rc=%Rrc\n", rc));
    ASMInvalidatePage((void *)pvFault);

    STAM_COUNTER_INC(&pVM->csam.s.StatCodePageModified);
    return VINF_SUCCESS;
}

