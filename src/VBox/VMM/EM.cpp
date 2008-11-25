/* $Id$ */
/** @file
 * EM - Execution Monitor / Manager.
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

/** @page pg_em         EM - The Execution Monitor / Manager
 *
 * The Execution Monitor/Manager is responsible for running the VM, scheduling
 * the right kind of execution (Raw-mode, Hardware Assisted, Recompiled or
 * Interpreted), and keeping the CPU states in sync. The function
 * EMR3ExecuteVM() is the 'main-loop' of the VM, while each of the execution
 * modes has different inner loops (emR3RawExecute, emR3HwAccExecute, and
 * emR3RemExecute).
 *
 * The interpreted execution is only used to avoid switching between
 * raw-mode/hwaccm and the recompiler when fielding virtualization traps/faults.
 * The interpretation is thus implemented as part of EM.
 *
 * @see grp_em
 */

/*******************************************************************************
*   Header Files                                                               *
*******************************************************************************/
#define LOG_GROUP LOG_GROUP_EM
#include <VBox/em.h>
#include <VBox/vmm.h>
#ifdef VBOX_WITH_VMI
# include <VBox/parav.h>
#endif
#include <VBox/patm.h>
#include <VBox/csam.h>
#include <VBox/selm.h>
#include <VBox/trpm.h>
#include <VBox/iom.h>
#include <VBox/dbgf.h>
#include <VBox/pgm.h>
#include <VBox/rem.h>
#include <VBox/tm.h>
#include <VBox/mm.h>
#include <VBox/ssm.h>
#include <VBox/pdmapi.h>
#include <VBox/pdmcritsect.h>
#include <VBox/pdmqueue.h>
#include <VBox/hwaccm.h>
#include <VBox/patm.h>
#include "EMInternal.h"
#include <VBox/vm.h>
#include <VBox/cpumdis.h>
#include <VBox/dis.h>
#include <VBox/disopcode.h>
#include <VBox/dbgf.h>

#include <VBox/log.h>
#include <iprt/thread.h>
#include <iprt/assert.h>
#include <iprt/asm.h>
#include <iprt/semaphore.h>
#include <iprt/string.h>
#include <iprt/avl.h>
#include <iprt/stream.h>
#include <VBox/param.h>
#include <VBox/err.h>


/*******************************************************************************
*   Internal Functions                                                         *
*******************************************************************************/
static DECLCALLBACK(int) emR3Save(PVM pVM, PSSMHANDLE pSSM);
static DECLCALLBACK(int) emR3Load(PVM pVM, PSSMHANDLE pSSM, uint32_t u32Version);
static int emR3Debug(PVM pVM, int rc);
static int emR3RemStep(PVM pVM);
static int emR3RemExecute(PVM pVM, bool *pfFFDone);
static int emR3RawResumeHyper(PVM pVM);
static int emR3RawStep(PVM pVM);
DECLINLINE(int) emR3RawHandleRC(PVM pVM, PCPUMCTX pCtx, int rc);
DECLINLINE(int) emR3RawUpdateForceFlag(PVM pVM, PCPUMCTX pCtx, int rc);
static int emR3RawForcedActions(PVM pVM, PCPUMCTX pCtx);
static int emR3RawExecute(PVM pVM, bool *pfFFDone);
DECLINLINE(int) emR3RawExecuteInstruction(PVM pVM, const char *pszPrefix, int rcGC = VINF_SUCCESS);
static int emR3HighPriorityPostForcedActions(PVM pVM, int rc);
static int emR3ForcedActions(PVM pVM, int rc);
static int emR3RawGuestTrap(PVM pVM);
static int emR3PatchTrap(PVM pVM, PCPUMCTX pCtx, int gcret);


/**
 * Initializes the EM.
 *
 * @returns VBox status code.
 * @param   pVM         The VM to operate on.
 */
VMMR3DECL(int) EMR3Init(PVM pVM)
{
    LogFlow(("EMR3Init\n"));
    /*
     * Assert alignment and sizes.
     */
    AssertRelease(!(RT_OFFSETOF(VM, em.s) & 31));
    AssertRelease(sizeof(pVM->em.s) <= sizeof(pVM->em.padding));
    AssertReleaseMsg(sizeof(pVM->em.s.u.FatalLongJump) <= sizeof(pVM->em.s.u.achPaddingFatalLongJump),
        ("%d bytes, padding %d\n", sizeof(pVM->em.s.u.FatalLongJump), sizeof(pVM->em.s.u.achPaddingFatalLongJump)));

    /*
     * Init the structure.
     */
    pVM->em.s.offVM = RT_OFFSETOF(VM, em.s);
    int rc = CFGMR3QueryBool(CFGMR3GetRoot(pVM), "RawR3Enabled", &pVM->fRawR3Enabled);
    if (RT_FAILURE(rc))
        pVM->fRawR3Enabled = true;
    rc = CFGMR3QueryBool(CFGMR3GetRoot(pVM), "RawR0Enabled", &pVM->fRawR0Enabled);
    if (RT_FAILURE(rc))
        pVM->fRawR0Enabled = true;
    Log(("EMR3Init: fRawR3Enabled=%d fRawR0Enabled=%d\n", pVM->fRawR3Enabled, pVM->fRawR0Enabled));
    pVM->em.s.enmState = EMSTATE_NONE;
    pVM->em.s.fForceRAW = false;

    pVM->em.s.pCtx = CPUMQueryGuestCtxPtr(pVM);
    pVM->em.s.pPatmGCState = PATMR3QueryGCStateHC(pVM);
    AssertMsg(pVM->em.s.pPatmGCState, ("PATMR3QueryGCStateHC failed!\n"));

    /*
     * Saved state.
     */
    rc = SSMR3RegisterInternal(pVM, "em", 0, EM_SAVED_STATE_VERSION, 16,
                               NULL, emR3Save, NULL,
                               NULL, emR3Load, NULL);
    if (RT_FAILURE(rc))
        return rc;

    /*
     * Statistics.
     */
#ifdef VBOX_WITH_STATISTICS
    PEMSTATS pStats;
    rc = MMHyperAlloc(pVM, sizeof(*pStats), 0, MM_TAG_EM, (void **)&pStats);
    if (RT_FAILURE(rc))
        return rc;
    pVM->em.s.pStatsR3 = pStats;
    pVM->em.s.pStatsR0 = MMHyperR3ToR0(pVM, pStats);
    pVM->em.s.pStatsRC = MMHyperR3ToRC(pVM, pStats);

    STAM_REG(pVM, &pStats->StatRZEmulate,               STAMTYPE_PROFILE, "/EM/RZ/Interpret",                   STAMUNIT_TICKS_PER_CALL, "Profiling of EMInterpretInstruction.");
    STAM_REG(pVM, &pStats->StatR3Emulate,               STAMTYPE_PROFILE, "/EM/R3/Interpret",                   STAMUNIT_TICKS_PER_CALL, "Profiling of EMInterpretInstruction.");

    STAM_REG(pVM, &pStats->StatRZInterpretSucceeded,    STAMTYPE_COUNTER, "/EM/RZ/Interpret/Success",           STAMUNIT_OCCURENCES,    "The number of times an instruction was successfully interpreted.");
    STAM_REG(pVM, &pStats->StatR3InterpretSucceeded,    STAMTYPE_COUNTER, "/EM/R3/Interpret/Success",           STAMUNIT_OCCURENCES,    "The number of times an instruction was successfully interpreted.");

    STAM_REG_USED(pVM, &pStats->StatRZAnd,                  STAMTYPE_COUNTER, "/EM/RZ/Interpret/Success/And",       STAMUNIT_OCCURENCES,    "The number of times AND was successfully interpreted.");
    STAM_REG_USED(pVM, &pStats->StatR3And,                  STAMTYPE_COUNTER, "/EM/R3/Interpret/Success/And",       STAMUNIT_OCCURENCES,    "The number of times AND was successfully interpreted.");
    STAM_REG_USED(pVM, &pStats->StatRZAdd,                  STAMTYPE_COUNTER, "/EM/RZ/Interpret/Success/Add",       STAMUNIT_OCCURENCES,    "The number of times ADD was successfully interpreted.");
    STAM_REG_USED(pVM, &pStats->StatR3Add,                  STAMTYPE_COUNTER, "/EM/R3/Interpret/Success/Add",       STAMUNIT_OCCURENCES,    "The number of times ADD was successfully interpreted.");
    STAM_REG_USED(pVM, &pStats->StatRZAdc,                  STAMTYPE_COUNTER, "/EM/RZ/Interpret/Success/Adc",       STAMUNIT_OCCURENCES,    "The number of times ADC was successfully interpreted.");
    STAM_REG_USED(pVM, &pStats->StatR3Adc,                  STAMTYPE_COUNTER, "/EM/R3/Interpret/Success/Adc",       STAMUNIT_OCCURENCES,    "The number of times ADC was successfully interpreted.");
    STAM_REG_USED(pVM, &pStats->StatRZSub,                  STAMTYPE_COUNTER, "/EM/RZ/Interpret/Success/Sub",       STAMUNIT_OCCURENCES,    "The number of times SUB was successfully interpreted.");
    STAM_REG_USED(pVM, &pStats->StatR3Sub,                  STAMTYPE_COUNTER, "/EM/R3/Interpret/Success/Sub",       STAMUNIT_OCCURENCES,    "The number of times SUB was successfully interpreted.");
    STAM_REG_USED(pVM, &pStats->StatRZCpuId,                STAMTYPE_COUNTER, "/EM/RZ/Interpret/Success/CpuId",     STAMUNIT_OCCURENCES,    "The number of times CPUID was successfully interpreted.");
    STAM_REG_USED(pVM, &pStats->StatR3CpuId,                STAMTYPE_COUNTER, "/EM/R3/Interpret/Success/CpuId",     STAMUNIT_OCCURENCES,    "The number of times CPUID was successfully interpreted.");
    STAM_REG_USED(pVM, &pStats->StatRZDec,                  STAMTYPE_COUNTER, "/EM/RZ/Interpret/Success/Dec",       STAMUNIT_OCCURENCES,    "The number of times DEC was successfully interpreted.");
    STAM_REG_USED(pVM, &pStats->StatR3Dec,                  STAMTYPE_COUNTER, "/EM/R3/Interpret/Success/Dec",       STAMUNIT_OCCURENCES,    "The number of times DEC was successfully interpreted.");
    STAM_REG_USED(pVM, &pStats->StatRZHlt,                  STAMTYPE_COUNTER, "/EM/RZ/Interpret/Success/Hlt",       STAMUNIT_OCCURENCES,    "The number of times HLT was successfully interpreted.");
    STAM_REG_USED(pVM, &pStats->StatR3Hlt,                  STAMTYPE_COUNTER, "/EM/R3/Interpret/Success/Hlt",       STAMUNIT_OCCURENCES,    "The number of times HLT was successfully interpreted.");
    STAM_REG_USED(pVM, &pStats->StatRZInc,                  STAMTYPE_COUNTER, "/EM/RZ/Interpret/Success/Inc",       STAMUNIT_OCCURENCES,    "The number of times INC was successfully interpreted.");
    STAM_REG_USED(pVM, &pStats->StatR3Inc,                  STAMTYPE_COUNTER, "/EM/R3/Interpret/Success/Inc",       STAMUNIT_OCCURENCES,    "The number of times INC was successfully interpreted.");
    STAM_REG_USED(pVM, &pStats->StatRZInvlPg,               STAMTYPE_COUNTER, "/EM/RZ/Interpret/Success/Invlpg",    STAMUNIT_OCCURENCES,    "The number of times INVLPG was successfully interpreted.");
    STAM_REG_USED(pVM, &pStats->StatR3InvlPg,               STAMTYPE_COUNTER, "/EM/R3/Interpret/Success/Invlpg",    STAMUNIT_OCCURENCES,    "The number of times INVLPG was successfully interpreted.");
    STAM_REG_USED(pVM, &pStats->StatRZIret,                 STAMTYPE_COUNTER, "/EM/RZ/Interpret/Success/Iret",      STAMUNIT_OCCURENCES,    "The number of times IRET was successfully interpreted.");
    STAM_REG_USED(pVM, &pStats->StatR3Iret,                 STAMTYPE_COUNTER, "/EM/R3/Interpret/Success/Iret",      STAMUNIT_OCCURENCES,    "The number of times IRET was successfully interpreted.");
    STAM_REG_USED(pVM, &pStats->StatRZLLdt,                 STAMTYPE_COUNTER, "/EM/RZ/Interpret/Success/LLdt",      STAMUNIT_OCCURENCES,    "The number of times LLDT was successfully interpreted.");
    STAM_REG_USED(pVM, &pStats->StatR3LLdt,                 STAMTYPE_COUNTER, "/EM/R3/Interpret/Success/LLdt",      STAMUNIT_OCCURENCES,    "The number of times LLDT was successfully interpreted.");
    STAM_REG_USED(pVM, &pStats->StatRZLIdt,                 STAMTYPE_COUNTER, "/EM/RZ/Interpret/Success/LIdt",      STAMUNIT_OCCURENCES,    "The number of times LIDT was successfully interpreted.");
    STAM_REG_USED(pVM, &pStats->StatR3LIdt,                 STAMTYPE_COUNTER, "/EM/R3/Interpret/Success/LIdt",      STAMUNIT_OCCURENCES,    "The number of times LIDT was successfully interpreted.");
    STAM_REG_USED(pVM, &pStats->StatRZLGdt,                 STAMTYPE_COUNTER, "/EM/RZ/Interpret/Success/LGdt",      STAMUNIT_OCCURENCES,    "The number of times LGDT was successfully interpreted.");
    STAM_REG_USED(pVM, &pStats->StatR3LGdt,                 STAMTYPE_COUNTER, "/EM/R3/Interpret/Success/LGdt",      STAMUNIT_OCCURENCES,    "The number of times LGDT was successfully interpreted.");
    STAM_REG_USED(pVM, &pStats->StatRZMov,                  STAMTYPE_COUNTER, "/EM/RZ/Interpret/Success/Mov",       STAMUNIT_OCCURENCES,    "The number of times MOV was successfully interpreted.");
    STAM_REG_USED(pVM, &pStats->StatR3Mov,                  STAMTYPE_COUNTER, "/EM/R3/Interpret/Success/Mov",       STAMUNIT_OCCURENCES,    "The number of times MOV was successfully interpreted.");
    STAM_REG_USED(pVM, &pStats->StatRZMovCRx,               STAMTYPE_COUNTER, "/EM/RZ/Interpret/Success/MovCRx",    STAMUNIT_OCCURENCES,    "The number of times MOV CRx was successfully interpreted.");
    STAM_REG_USED(pVM, &pStats->StatR3MovCRx,               STAMTYPE_COUNTER, "/EM/R3/Interpret/Success/MovCRx",    STAMUNIT_OCCURENCES,    "The number of times MOV CRx was successfully interpreted.");
    STAM_REG_USED(pVM, &pStats->StatRZMovDRx,               STAMTYPE_COUNTER, "/EM/RZ/Interpret/Success/MovDRx",    STAMUNIT_OCCURENCES,    "The number of times MOV DRx was successfully interpreted.");
    STAM_REG_USED(pVM, &pStats->StatR3MovDRx,               STAMTYPE_COUNTER, "/EM/R3/Interpret/Success/MovDRx",    STAMUNIT_OCCURENCES,    "The number of times MOV DRx was successfully interpreted.");
    STAM_REG_USED(pVM, &pStats->StatRZOr,                   STAMTYPE_COUNTER, "/EM/RZ/Interpret/Success/Or",        STAMUNIT_OCCURENCES,    "The number of times OR was successfully interpreted.");
    STAM_REG_USED(pVM, &pStats->StatR3Or,                   STAMTYPE_COUNTER, "/EM/R3/Interpret/Success/Or",        STAMUNIT_OCCURENCES,    "The number of times OR was successfully interpreted.");
    STAM_REG_USED(pVM, &pStats->StatRZPop,                  STAMTYPE_COUNTER, "/EM/RZ/Interpret/Success/Pop",       STAMUNIT_OCCURENCES,    "The number of times POP was successfully interpreted.");
    STAM_REG_USED(pVM, &pStats->StatR3Pop,                  STAMTYPE_COUNTER, "/EM/R3/Interpret/Success/Pop",       STAMUNIT_OCCURENCES,    "The number of times POP was successfully interpreted.");
    STAM_REG_USED(pVM, &pStats->StatRZRdtsc,                STAMTYPE_COUNTER, "/EM/RZ/Interpret/Success/Rdtsc",     STAMUNIT_OCCURENCES,    "The number of times RDTSC was successfully interpreted.");
    STAM_REG_USED(pVM, &pStats->StatR3Rdtsc,                STAMTYPE_COUNTER, "/EM/R3/Interpret/Success/Rdtsc",     STAMUNIT_OCCURENCES,    "The number of times RDTSC was successfully interpreted.");
    STAM_REG_USED(pVM, &pStats->StatRZSti,                  STAMTYPE_COUNTER, "/EM/RZ/Interpret/Success/Sti",       STAMUNIT_OCCURENCES,    "The number of times STI was successfully interpreted.");
    STAM_REG_USED(pVM, &pStats->StatR3Sti,                  STAMTYPE_COUNTER, "/EM/R3/Interpret/Success/Sti",       STAMUNIT_OCCURENCES,    "The number of times STI was successfully interpreted.");
    STAM_REG_USED(pVM, &pStats->StatRZXchg,                 STAMTYPE_COUNTER, "/EM/RZ/Interpret/Success/Xchg",      STAMUNIT_OCCURENCES,    "The number of times XCHG was successfully interpreted.");
    STAM_REG_USED(pVM, &pStats->StatR3Xchg,                 STAMTYPE_COUNTER, "/EM/R3/Interpret/Success/Xchg",      STAMUNIT_OCCURENCES,    "The number of times XCHG was successfully interpreted.");
    STAM_REG_USED(pVM, &pStats->StatRZXor,                  STAMTYPE_COUNTER, "/EM/RZ/Interpret/Success/Xor",       STAMUNIT_OCCURENCES,    "The number of times XOR was successfully interpreted.");
    STAM_REG_USED(pVM, &pStats->StatR3Xor,                  STAMTYPE_COUNTER, "/EM/R3/Interpret/Success/Xor",       STAMUNIT_OCCURENCES,    "The number of times XOR was successfully interpreted.");
    STAM_REG_USED(pVM, &pStats->StatRZMonitor,              STAMTYPE_COUNTER, "/EM/RZ/Interpret/Success/Monitor",   STAMUNIT_OCCURENCES,    "The number of times MONITOR was successfully interpreted.");
    STAM_REG_USED(pVM, &pStats->StatR3Monitor,              STAMTYPE_COUNTER, "/EM/R3/Interpret/Success/Monitor",   STAMUNIT_OCCURENCES,    "The number of times MONITOR was successfully interpreted.");
    STAM_REG_USED(pVM, &pStats->StatRZMWait,                STAMTYPE_COUNTER, "/EM/RZ/Interpret/Success/MWait",     STAMUNIT_OCCURENCES,    "The number of times MWAIT was successfully interpreted.");
    STAM_REG_USED(pVM, &pStats->StatR3MWait,                STAMTYPE_COUNTER, "/EM/R3/Interpret/Success/MWait",     STAMUNIT_OCCURENCES,    "The number of times MWAIT was successfully interpreted.");
    STAM_REG_USED(pVM, &pStats->StatRZBtr,                  STAMTYPE_COUNTER, "/EM/RZ/Interpret/Success/Btr",       STAMUNIT_OCCURENCES,    "The number of times BTR was successfully interpreted.");
    STAM_REG_USED(pVM, &pStats->StatR3Btr,                  STAMTYPE_COUNTER, "/EM/R3/Interpret/Success/Btr",       STAMUNIT_OCCURENCES,    "The number of times BTR was successfully interpreted.");
    STAM_REG_USED(pVM, &pStats->StatRZBts,                  STAMTYPE_COUNTER, "/EM/RZ/Interpret/Success/Bts",       STAMUNIT_OCCURENCES,    "The number of times BTS was successfully interpreted.");
    STAM_REG_USED(pVM, &pStats->StatR3Bts,                  STAMTYPE_COUNTER, "/EM/R3/Interpret/Success/Bts",       STAMUNIT_OCCURENCES,    "The number of times BTS was successfully interpreted.");
    STAM_REG_USED(pVM, &pStats->StatRZBtc,                  STAMTYPE_COUNTER, "/EM/RZ/Interpret/Success/Btc",       STAMUNIT_OCCURENCES,    "The number of times BTC was successfully interpreted.");
    STAM_REG_USED(pVM, &pStats->StatR3Btc,                  STAMTYPE_COUNTER, "/EM/R3/Interpret/Success/Btc",       STAMUNIT_OCCURENCES,    "The number of times BTC was successfully interpreted.");
    STAM_REG_USED(pVM, &pStats->StatRZCmpXchg,              STAMTYPE_COUNTER, "/EM/RZ/Interpret/Success/CmpXchg",   STAMUNIT_OCCURENCES,    "The number of times CMPXCHG was successfully interpreted.");
    STAM_REG_USED(pVM, &pStats->StatR3CmpXchg,              STAMTYPE_COUNTER, "/EM/R3/Interpret/Success/CmpXchg",   STAMUNIT_OCCURENCES,    "The number of times CMPXCHG was successfully interpreted.");
    STAM_REG_USED(pVM, &pStats->StatRZCmpXchg8b,            STAMTYPE_COUNTER, "/EM/RZ/Interpret/Success/CmpXchg8b",   STAMUNIT_OCCURENCES,  "The number of times CMPXCHG8B was successfully interpreted.");
    STAM_REG_USED(pVM, &pStats->StatR3CmpXchg8b,            STAMTYPE_COUNTER, "/EM/R3/Interpret/Success/CmpXchg8b",   STAMUNIT_OCCURENCES,  "The number of times CMPXCHG8B was successfully interpreted.");
    STAM_REG_USED(pVM, &pStats->StatRZXAdd,                 STAMTYPE_COUNTER, "/EM/RZ/Interpret/Success/XAdd",      STAMUNIT_OCCURENCES,    "The number of times XADD was successfully interpreted.");
    STAM_REG_USED(pVM, &pStats->StatR3XAdd,                 STAMTYPE_COUNTER, "/EM/R3/Interpret/Success/XAdd",      STAMUNIT_OCCURENCES,    "The number of times XADD was successfully interpreted.");
    STAM_REG_USED(pVM, &pStats->StatR3Rdmsr,                STAMTYPE_COUNTER, "/EM/R3/Interpret/Success/Rdmsr",      STAMUNIT_OCCURENCES,   "The number of times RDMSR was successfully interpreted.");
    STAM_REG_USED(pVM, &pStats->StatRZRdmsr,                STAMTYPE_COUNTER, "/EM/RZ/Interpret/Success/Rdmsr",      STAMUNIT_OCCURENCES,   "The number of times RDMSR was successfully interpreted.");
    STAM_REG_USED(pVM, &pStats->StatR3Wrmsr,                STAMTYPE_COUNTER, "/EM/R3/Interpret/Success/Wrmsr",      STAMUNIT_OCCURENCES,   "The number of times WRMSR was successfully interpreted.");
    STAM_REG_USED(pVM, &pStats->StatRZWrmsr,                STAMTYPE_COUNTER, "/EM/RZ/Interpret/Success/Wrmsr",      STAMUNIT_OCCURENCES,   "The number of times WRMSR was successfully interpreted.");
    STAM_REG_USED(pVM, &pStats->StatR3StosWD,               STAMTYPE_COUNTER, "/EM/R3/Interpret/Success/Stoswd",     STAMUNIT_OCCURENCES,   "The number of times STOSWD was successfully interpreted.");
    STAM_REG_USED(pVM, &pStats->StatRZStosWD,               STAMTYPE_COUNTER, "/EM/RZ/Interpret/Success/Stoswd",     STAMUNIT_OCCURENCES,   "The number of times STOSWD was successfully interpreted.");
    STAM_REG_USED(pVM, &pStats->StatRZWbInvd,               STAMTYPE_COUNTER, "/EM/RZ/Interpret/Success/WbInvd",     STAMUNIT_OCCURENCES,   "The number of times WBINVD was successfully interpreted.");
    STAM_REG_USED(pVM, &pStats->StatR3WbInvd,               STAMTYPE_COUNTER, "/EM/R3/Interpret/Success/WbInvd",     STAMUNIT_OCCURENCES,   "The number of times WBINVD was successfully interpreted.");
    STAM_REG_USED(pVM, &pStats->StatRZLmsw,                 STAMTYPE_COUNTER, "/EM/RZ/Interpret/Success/Lmsw",       STAMUNIT_OCCURENCES,   "The number of times LMSW was successfully interpreted.");
    STAM_REG_USED(pVM, &pStats->StatR3Lmsw,                 STAMTYPE_COUNTER, "/EM/R3/Interpret/Success/Lmsw",       STAMUNIT_OCCURENCES,   "The number of times LMSW was successfully interpreted.");

    STAM_REG(pVM, &pStats->StatRZInterpretFailed,           STAMTYPE_COUNTER, "/EM/RZ/Interpret/Failed",            STAMUNIT_OCCURENCES,    "The number of times an instruction was not interpreted.");
    STAM_REG(pVM, &pStats->StatR3InterpretFailed,           STAMTYPE_COUNTER, "/EM/R3/Interpret/Failed",            STAMUNIT_OCCURENCES,    "The number of times an instruction was not interpreted.");

    STAM_REG_USED(pVM, &pStats->StatRZFailedAnd,            STAMTYPE_COUNTER, "/EM/RZ/Interpret/Failed/And",        STAMUNIT_OCCURENCES,    "The number of times AND was not interpreted.");
    STAM_REG_USED(pVM, &pStats->StatR3FailedAnd,            STAMTYPE_COUNTER, "/EM/R3/Interpret/Failed/And",        STAMUNIT_OCCURENCES,    "The number of times AND was not interpreted.");
    STAM_REG_USED(pVM, &pStats->StatRZFailedCpuId,          STAMTYPE_COUNTER, "/EM/RZ/Interpret/Failed/CpuId",      STAMUNIT_OCCURENCES,    "The number of times CPUID was not interpreted.");
    STAM_REG_USED(pVM, &pStats->StatR3FailedCpuId,          STAMTYPE_COUNTER, "/EM/R3/Interpret/Failed/CpuId",      STAMUNIT_OCCURENCES,    "The number of times CPUID was not interpreted.");
    STAM_REG_USED(pVM, &pStats->StatRZFailedDec,            STAMTYPE_COUNTER, "/EM/RZ/Interpret/Failed/Dec",        STAMUNIT_OCCURENCES,    "The number of times DEC was not interpreted.");
    STAM_REG_USED(pVM, &pStats->StatR3FailedDec,            STAMTYPE_COUNTER, "/EM/R3/Interpret/Failed/Dec",        STAMUNIT_OCCURENCES,    "The number of times DEC was not interpreted.");
    STAM_REG_USED(pVM, &pStats->StatRZFailedHlt,            STAMTYPE_COUNTER, "/EM/RZ/Interpret/Failed/Hlt",        STAMUNIT_OCCURENCES,    "The number of times HLT was not interpreted.");
    STAM_REG_USED(pVM, &pStats->StatR3FailedHlt,            STAMTYPE_COUNTER, "/EM/R3/Interpret/Failed/Hlt",        STAMUNIT_OCCURENCES,    "The number of times HLT was not interpreted.");
    STAM_REG_USED(pVM, &pStats->StatRZFailedInc,            STAMTYPE_COUNTER, "/EM/RZ/Interpret/Failed/Inc",        STAMUNIT_OCCURENCES,    "The number of times INC was not interpreted.");
    STAM_REG_USED(pVM, &pStats->StatR3FailedInc,            STAMTYPE_COUNTER, "/EM/R3/Interpret/Failed/Inc",        STAMUNIT_OCCURENCES,    "The number of times INC was not interpreted.");
    STAM_REG_USED(pVM, &pStats->StatRZFailedInvlPg,         STAMTYPE_COUNTER, "/EM/RZ/Interpret/Failed/InvlPg",     STAMUNIT_OCCURENCES,    "The number of times INVLPG was not interpreted.");
    STAM_REG_USED(pVM, &pStats->StatR3FailedInvlPg,         STAMTYPE_COUNTER, "/EM/R3/Interpret/Failed/InvlPg",     STAMUNIT_OCCURENCES,    "The number of times INVLPG was not interpreted.");
    STAM_REG_USED(pVM, &pStats->StatRZFailedIret,           STAMTYPE_COUNTER, "/EM/RZ/Interpret/Failed/Iret",       STAMUNIT_OCCURENCES,    "The number of times IRET was not interpreted.");
    STAM_REG_USED(pVM, &pStats->StatR3FailedIret,           STAMTYPE_COUNTER, "/EM/R3/Interpret/Failed/Iret",       STAMUNIT_OCCURENCES,    "The number of times IRET was not interpreted.");
    STAM_REG_USED(pVM, &pStats->StatRZFailedLLdt,           STAMTYPE_COUNTER, "/EM/RZ/Interpret/Failed/LLdt",       STAMUNIT_OCCURENCES,    "The number of times LLDT was not interpreted.");
    STAM_REG_USED(pVM, &pStats->StatR3FailedLLdt,           STAMTYPE_COUNTER, "/EM/R3/Interpret/Failed/LLdt",       STAMUNIT_OCCURENCES,    "The number of times LLDT was not interpreted.");
    STAM_REG_USED(pVM, &pStats->StatRZFailedLIdt,           STAMTYPE_COUNTER, "/EM/RZ/Interpret/Failed/LIdt",       STAMUNIT_OCCURENCES,    "The number of times LIDT was not interpreted.");
    STAM_REG_USED(pVM, &pStats->StatR3FailedLIdt,           STAMTYPE_COUNTER, "/EM/R3/Interpret/Failed/LIdt",       STAMUNIT_OCCURENCES,    "The number of times LIDT was not interpreted.");
    STAM_REG_USED(pVM, &pStats->StatRZFailedLGdt,           STAMTYPE_COUNTER, "/EM/RZ/Interpret/Failed/LGdt",       STAMUNIT_OCCURENCES,    "The number of times LGDT was not interpreted.");
    STAM_REG_USED(pVM, &pStats->StatR3FailedLGdt,           STAMTYPE_COUNTER, "/EM/R3/Interpret/Failed/LGdt",       STAMUNIT_OCCURENCES,    "The number of times LGDT was not interpreted.");
    STAM_REG_USED(pVM, &pStats->StatRZFailedMov,            STAMTYPE_COUNTER, "/EM/RZ/Interpret/Failed/Mov",        STAMUNIT_OCCURENCES,    "The number of times MOV was not interpreted.");
    STAM_REG_USED(pVM, &pStats->StatR3FailedMov,            STAMTYPE_COUNTER, "/EM/R3/Interpret/Failed/Mov",        STAMUNIT_OCCURENCES,    "The number of times MOV was not interpreted.");
    STAM_REG_USED(pVM, &pStats->StatRZFailedMovCRx,         STAMTYPE_COUNTER, "/EM/RZ/Interpret/Failed/MovCRx",     STAMUNIT_OCCURENCES,    "The number of times MOV CRx was not interpreted.");
    STAM_REG_USED(pVM, &pStats->StatR3FailedMovCRx,         STAMTYPE_COUNTER, "/EM/R3/Interpret/Failed/MovCRx",     STAMUNIT_OCCURENCES,    "The number of times MOV CRx was not interpreted.");
    STAM_REG_USED(pVM, &pStats->StatRZFailedMovDRx,         STAMTYPE_COUNTER, "/EM/RZ/Interpret/Failed/MovDRx",     STAMUNIT_OCCURENCES,    "The number of times MOV DRx was not interpreted.");
    STAM_REG_USED(pVM, &pStats->StatR3FailedMovDRx,         STAMTYPE_COUNTER, "/EM/R3/Interpret/Failed/MovDRx",     STAMUNIT_OCCURENCES,    "The number of times MOV DRx was not interpreted.");
    STAM_REG_USED(pVM, &pStats->StatRZFailedOr,             STAMTYPE_COUNTER, "/EM/RZ/Interpret/Failed/Or",         STAMUNIT_OCCURENCES,    "The number of times OR was not interpreted.");
    STAM_REG_USED(pVM, &pStats->StatR3FailedOr,             STAMTYPE_COUNTER, "/EM/R3/Interpret/Failed/Or",         STAMUNIT_OCCURENCES,    "The number of times OR was not interpreted.");
    STAM_REG_USED(pVM, &pStats->StatRZFailedPop,            STAMTYPE_COUNTER, "/EM/RZ/Interpret/Failed/Pop",        STAMUNIT_OCCURENCES,    "The number of times POP was not interpreted.");
    STAM_REG_USED(pVM, &pStats->StatR3FailedPop,            STAMTYPE_COUNTER, "/EM/R3/Interpret/Failed/Pop",        STAMUNIT_OCCURENCES,    "The number of times POP was not interpreted.");
    STAM_REG_USED(pVM, &pStats->StatRZFailedSti,            STAMTYPE_COUNTER, "/EM/RZ/Interpret/Failed/Sti",        STAMUNIT_OCCURENCES,    "The number of times STI was not interpreted.");
    STAM_REG_USED(pVM, &pStats->StatR3FailedSti,            STAMTYPE_COUNTER, "/EM/R3/Interpret/Failed/Sti",        STAMUNIT_OCCURENCES,    "The number of times STI was not interpreted.");
    STAM_REG_USED(pVM, &pStats->StatRZFailedXchg,           STAMTYPE_COUNTER, "/EM/RZ/Interpret/Failed/Xchg",       STAMUNIT_OCCURENCES,    "The number of times XCHG was not interpreted.");
    STAM_REG_USED(pVM, &pStats->StatR3FailedXchg,           STAMTYPE_COUNTER, "/EM/R3/Interpret/Failed/Xchg",       STAMUNIT_OCCURENCES,    "The number of times XCHG was not interpreted.");
    STAM_REG_USED(pVM, &pStats->StatRZFailedXor,            STAMTYPE_COUNTER, "/EM/RZ/Interpret/Failed/Xor",        STAMUNIT_OCCURENCES,    "The number of times XOR was not interpreted.");
    STAM_REG_USED(pVM, &pStats->StatR3FailedXor,            STAMTYPE_COUNTER, "/EM/R3/Interpret/Failed/Xor",        STAMUNIT_OCCURENCES,    "The number of times XOR was not interpreted.");
    STAM_REG_USED(pVM, &pStats->StatRZFailedMonitor,        STAMTYPE_COUNTER, "/EM/RZ/Interpret/Failed/Monitor",    STAMUNIT_OCCURENCES,    "The number of times MONITOR was not interpreted.");
    STAM_REG_USED(pVM, &pStats->StatR3FailedMonitor,        STAMTYPE_COUNTER, "/EM/R3/Interpret/Failed/Monitor",    STAMUNIT_OCCURENCES,    "The number of times MONITOR was not interpreted.");
    STAM_REG_USED(pVM, &pStats->StatRZFailedMWait,          STAMTYPE_COUNTER, "/EM/RZ/Interpret/Failed/MWait",      STAMUNIT_OCCURENCES,    "The number of times MONITOR was not interpreted.");
    STAM_REG_USED(pVM, &pStats->StatR3FailedMWait,          STAMTYPE_COUNTER, "/EM/R3/Interpret/Failed/MWait",      STAMUNIT_OCCURENCES,    "The number of times MONITOR was not interpreted.");
    STAM_REG_USED(pVM, &pStats->StatRZFailedRdtsc,          STAMTYPE_COUNTER, "/EM/RZ/Interpret/Failed/Rdtsc",      STAMUNIT_OCCURENCES,    "The number of times RDTSC was not interpreted.");
    STAM_REG_USED(pVM, &pStats->StatR3FailedRdtsc,          STAMTYPE_COUNTER, "/EM/R3/Interpret/Failed/Rdtsc",      STAMUNIT_OCCURENCES,    "The number of times RDTSC was not interpreted.");
    STAM_REG_USED(pVM, &pStats->StatRZFailedRdmsr,          STAMTYPE_COUNTER, "/EM/RZ/Interpret/Failed/Rdmsr",      STAMUNIT_OCCURENCES,    "The number of times RDMSR was not interpreted.");
    STAM_REG_USED(pVM, &pStats->StatR3FailedRdmsr,          STAMTYPE_COUNTER, "/EM/R3/Interpret/Failed/Rdmsr",      STAMUNIT_OCCURENCES,    "The number of times RDMSR was not interpreted.");
    STAM_REG_USED(pVM, &pStats->StatRZFailedWrmsr,          STAMTYPE_COUNTER, "/EM/RZ/Interpret/Failed/Wrmsr",      STAMUNIT_OCCURENCES,    "The number of times WRMSR was not interpreted.");
    STAM_REG_USED(pVM, &pStats->StatR3FailedWrmsr,          STAMTYPE_COUNTER, "/EM/R3/Interpret/Failed/Wrmsr",      STAMUNIT_OCCURENCES,    "The number of times WRMSR was not interpreted.");
    STAM_REG_USED(pVM, &pStats->StatRZFailedLmsw,           STAMTYPE_COUNTER, "/EM/RZ/Interpret/Failed/Lmsw",       STAMUNIT_OCCURENCES,    "The number of times LMSW was not interpreted.");
    STAM_REG_USED(pVM, &pStats->StatR3FailedLmsw,           STAMTYPE_COUNTER, "/EM/R3/Interpret/Failed/Lmsw",       STAMUNIT_OCCURENCES,    "The number of times LMSW was not interpreted.");

    STAM_REG_USED(pVM, &pStats->StatRZFailedMisc,           STAMTYPE_COUNTER, "/EM/RZ/Interpret/Failed/Misc",       STAMUNIT_OCCURENCES,    "The number of times some misc instruction was encountered.");
    STAM_REG_USED(pVM, &pStats->StatR3FailedMisc,           STAMTYPE_COUNTER, "/EM/R3/Interpret/Failed/Misc",       STAMUNIT_OCCURENCES,    "The number of times some misc instruction was encountered.");
    STAM_REG_USED(pVM, &pStats->StatRZFailedAdd,            STAMTYPE_COUNTER, "/EM/RZ/Interpret/Failed/Add",        STAMUNIT_OCCURENCES,    "The number of times ADD was not interpreted.");
    STAM_REG_USED(pVM, &pStats->StatR3FailedAdd,            STAMTYPE_COUNTER, "/EM/R3/Interpret/Failed/Add",        STAMUNIT_OCCURENCES,    "The number of times ADD was not interpreted.");
    STAM_REG_USED(pVM, &pStats->StatRZFailedAdc,            STAMTYPE_COUNTER, "/EM/RZ/Interpret/Failed/Adc",        STAMUNIT_OCCURENCES,    "The number of times ADC was not interpreted.");
    STAM_REG_USED(pVM, &pStats->StatR3FailedAdc,            STAMTYPE_COUNTER, "/EM/R3/Interpret/Failed/Adc",        STAMUNIT_OCCURENCES,    "The number of times ADC was not interpreted.");
    STAM_REG_USED(pVM, &pStats->StatRZFailedBtr,            STAMTYPE_COUNTER, "/EM/RZ/Interpret/Failed/Btr",        STAMUNIT_OCCURENCES,    "The number of times BTR was not interpreted.");
    STAM_REG_USED(pVM, &pStats->StatR3FailedBtr,            STAMTYPE_COUNTER, "/EM/R3/Interpret/Failed/Btr",        STAMUNIT_OCCURENCES,    "The number of times BTR was not interpreted.");
    STAM_REG_USED(pVM, &pStats->StatRZFailedBts,            STAMTYPE_COUNTER, "/EM/RZ/Interpret/Failed/Bts",        STAMUNIT_OCCURENCES,    "The number of times BTS was not interpreted.");
    STAM_REG_USED(pVM, &pStats->StatR3FailedBts,            STAMTYPE_COUNTER, "/EM/R3/Interpret/Failed/Bts",        STAMUNIT_OCCURENCES,    "The number of times BTS was not interpreted.");
    STAM_REG_USED(pVM, &pStats->StatRZFailedBtc,            STAMTYPE_COUNTER, "/EM/RZ/Interpret/Failed/Btc",        STAMUNIT_OCCURENCES,    "The number of times BTC was not interpreted.");
    STAM_REG_USED(pVM, &pStats->StatR3FailedBtc,            STAMTYPE_COUNTER, "/EM/R3/Interpret/Failed/Btc",        STAMUNIT_OCCURENCES,    "The number of times BTC was not interpreted.");
    STAM_REG_USED(pVM, &pStats->StatRZFailedCli,            STAMTYPE_COUNTER, "/EM/RZ/Interpret/Failed/Cli",        STAMUNIT_OCCURENCES,    "The number of times CLI was not interpreted.");
    STAM_REG_USED(pVM, &pStats->StatR3FailedCli,            STAMTYPE_COUNTER, "/EM/R3/Interpret/Failed/Cli",        STAMUNIT_OCCURENCES,    "The number of times CLI was not interpreted.");
    STAM_REG_USED(pVM, &pStats->StatRZFailedCmpXchg,        STAMTYPE_COUNTER, "/EM/RZ/Interpret/Failed/CmpXchg",    STAMUNIT_OCCURENCES,    "The number of times CMPXCHG was not interpreted.");
    STAM_REG_USED(pVM, &pStats->StatR3FailedCmpXchg,        STAMTYPE_COUNTER, "/EM/R3/Interpret/Failed/CmpXchg",    STAMUNIT_OCCURENCES,    "The number of times CMPXCHG was not interpreted.");
    STAM_REG_USED(pVM, &pStats->StatRZFailedCmpXchg8b,      STAMTYPE_COUNTER, "/EM/RZ/Interpret/Failed/CmpXchg8b",  STAMUNIT_OCCURENCES,    "The number of times CMPXCHG8B was not interpreted.");
    STAM_REG_USED(pVM, &pStats->StatR3FailedCmpXchg8b,      STAMTYPE_COUNTER, "/EM/R3/Interpret/Failed/CmpXchg8b",  STAMUNIT_OCCURENCES,    "The number of times CMPXCHG8B was not interpreted.");
    STAM_REG_USED(pVM, &pStats->StatRZFailedXAdd,           STAMTYPE_COUNTER, "/EM/RZ/Interpret/Failed/XAdd",       STAMUNIT_OCCURENCES,    "The number of times XADD was not interpreted.");
    STAM_REG_USED(pVM, &pStats->StatR3FailedXAdd,           STAMTYPE_COUNTER, "/EM/R3/Interpret/Failed/XAdd",       STAMUNIT_OCCURENCES,    "The number of times XADD was not interpreted.");
    STAM_REG_USED(pVM, &pStats->StatRZFailedMovNTPS,        STAMTYPE_COUNTER, "/EM/RZ/Interpret/Failed/MovNTPS",    STAMUNIT_OCCURENCES,    "The number of times MOVNTPS was not interpreted.");
    STAM_REG_USED(pVM, &pStats->StatR3FailedMovNTPS,        STAMTYPE_COUNTER, "/EM/R3/Interpret/Failed/MovNTPS",    STAMUNIT_OCCURENCES,    "The number of times MOVNTPS was not interpreted.");
    STAM_REG_USED(pVM, &pStats->StatRZFailedStosWD,         STAMTYPE_COUNTER, "/EM/RZ/Interpret/Failed/StosWD",     STAMUNIT_OCCURENCES,    "The number of times STOSWD was not interpreted.");
    STAM_REG_USED(pVM, &pStats->StatR3FailedStosWD,         STAMTYPE_COUNTER, "/EM/R3/Interpret/Failed/StosWD",     STAMUNIT_OCCURENCES,    "The number of times STOSWD was not interpreted.");
    STAM_REG_USED(pVM, &pStats->StatRZFailedSub,            STAMTYPE_COUNTER, "/EM/RZ/Interpret/Failed/Sub",        STAMUNIT_OCCURENCES,    "The number of times SUB was not interpreted.");
    STAM_REG_USED(pVM, &pStats->StatR3FailedSub,            STAMTYPE_COUNTER, "/EM/R3/Interpret/Failed/Sub",        STAMUNIT_OCCURENCES,    "The number of times SUB was not interpreted.");
    STAM_REG_USED(pVM, &pStats->StatRZFailedWbInvd,         STAMTYPE_COUNTER, "/EM/RZ/Interpret/Failed/WbInvd",     STAMUNIT_OCCURENCES,    "The number of times WBINVD was not interpreted.");
    STAM_REG_USED(pVM, &pStats->StatR3FailedWbInvd,         STAMTYPE_COUNTER, "/EM/R3/Interpret/Failed/WbInvd",     STAMUNIT_OCCURENCES,    "The number of times WBINVD was not interpreted.");

    STAM_REG_USED(pVM, &pStats->StatRZFailedUserMode,       STAMTYPE_COUNTER, "/EM/RZ/Interpret/Failed/UserMode",   STAMUNIT_OCCURENCES,    "The number of rejections because of CPL.");
    STAM_REG_USED(pVM, &pStats->StatR3FailedUserMode,       STAMTYPE_COUNTER, "/EM/R3/Interpret/Failed/UserMode",   STAMUNIT_OCCURENCES,    "The number of rejections because of CPL.");
    STAM_REG_USED(pVM, &pStats->StatRZFailedPrefix,         STAMTYPE_COUNTER, "/EM/RZ/Interpret/Failed/Prefix",     STAMUNIT_OCCURENCES,    "The number of rejections because of prefix .");
    STAM_REG_USED(pVM, &pStats->StatR3FailedPrefix,         STAMTYPE_COUNTER, "/EM/R3/Interpret/Failed/Prefix",     STAMUNIT_OCCURENCES,    "The number of rejections because of prefix .");

    STAM_REG_USED(pVM, &pStats->StatCli,                    STAMTYPE_COUNTER, "/EM/R3/PrivInst/Cli",                STAMUNIT_OCCURENCES,    "Number of cli instructions.");
    STAM_REG_USED(pVM, &pStats->StatSti,                    STAMTYPE_COUNTER, "/EM/R3/PrivInst/Sti",                STAMUNIT_OCCURENCES,    "Number of sli instructions.");
    STAM_REG_USED(pVM, &pStats->StatIn,                     STAMTYPE_COUNTER, "/EM/R3/PrivInst/In",                 STAMUNIT_OCCURENCES,    "Number of in instructions.");
    STAM_REG_USED(pVM, &pStats->StatOut,                    STAMTYPE_COUNTER, "/EM/R3/PrivInst/Out",                STAMUNIT_OCCURENCES,    "Number of out instructions.");
    STAM_REG_USED(pVM, &pStats->StatHlt,                    STAMTYPE_COUNTER, "/EM/R3/PrivInst/Hlt",                STAMUNIT_OCCURENCES,    "Number of hlt instructions not handled in GC because of PATM.");
    STAM_REG_USED(pVM, &pStats->StatInvlpg,                 STAMTYPE_COUNTER, "/EM/R3/PrivInst/Invlpg",             STAMUNIT_OCCURENCES,    "Number of invlpg instructions.");
    STAM_REG_USED(pVM, &pStats->StatMisc,                   STAMTYPE_COUNTER, "/EM/R3/PrivInst/Misc",               STAMUNIT_OCCURENCES,    "Number of misc. instructions.");
    STAM_REG_USED(pVM, &pStats->StatMovWriteCR[0],          STAMTYPE_COUNTER, "/EM/R3/PrivInst/Mov CR0, X",         STAMUNIT_OCCURENCES,    "Number of mov CR0 read instructions.");
    STAM_REG_USED(pVM, &pStats->StatMovWriteCR[1],          STAMTYPE_COUNTER, "/EM/R3/PrivInst/Mov CR1, X",         STAMUNIT_OCCURENCES,    "Number of mov CR1 read instructions.");
    STAM_REG_USED(pVM, &pStats->StatMovWriteCR[2],          STAMTYPE_COUNTER, "/EM/R3/PrivInst/Mov CR2, X",         STAMUNIT_OCCURENCES,    "Number of mov CR2 read instructions.");
    STAM_REG_USED(pVM, &pStats->StatMovWriteCR[3],          STAMTYPE_COUNTER, "/EM/R3/PrivInst/Mov CR3, X",         STAMUNIT_OCCURENCES,    "Number of mov CR3 read instructions.");
    STAM_REG_USED(pVM, &pStats->StatMovWriteCR[4],          STAMTYPE_COUNTER, "/EM/R3/PrivInst/Mov CR4, X",         STAMUNIT_OCCURENCES,    "Number of mov CR4 read instructions.");
    STAM_REG_USED(pVM, &pStats->StatMovReadCR[0],           STAMTYPE_COUNTER, "/EM/R3/PrivInst/Mov X, CR0",         STAMUNIT_OCCURENCES,    "Number of mov CR0 write instructions.");
    STAM_REG_USED(pVM, &pStats->StatMovReadCR[1],           STAMTYPE_COUNTER, "/EM/R3/PrivInst/Mov X, CR1",         STAMUNIT_OCCURENCES,    "Number of mov CR1 write instructions.");
    STAM_REG_USED(pVM, &pStats->StatMovReadCR[2],           STAMTYPE_COUNTER, "/EM/R3/PrivInst/Mov X, CR2",         STAMUNIT_OCCURENCES,    "Number of mov CR2 write instructions.");
    STAM_REG_USED(pVM, &pStats->StatMovReadCR[3],           STAMTYPE_COUNTER, "/EM/R3/PrivInst/Mov X, CR3",         STAMUNIT_OCCURENCES,    "Number of mov CR3 write instructions.");
    STAM_REG_USED(pVM, &pStats->StatMovReadCR[4],           STAMTYPE_COUNTER, "/EM/R3/PrivInst/Mov X, CR4",         STAMUNIT_OCCURENCES,    "Number of mov CR4 write instructions.");
    STAM_REG_USED(pVM, &pStats->StatMovDRx,                 STAMTYPE_COUNTER, "/EM/R3/PrivInst/MovDRx",             STAMUNIT_OCCURENCES,    "Number of mov DRx instructions.");
    STAM_REG_USED(pVM, &pStats->StatIret,                   STAMTYPE_COUNTER, "/EM/R3/PrivInst/Iret",               STAMUNIT_OCCURENCES,    "Number of iret instructions.");
    STAM_REG_USED(pVM, &pStats->StatMovLgdt,                STAMTYPE_COUNTER, "/EM/R3/PrivInst/Lgdt",               STAMUNIT_OCCURENCES,    "Number of lgdt instructions.");
    STAM_REG_USED(pVM, &pStats->StatMovLidt,                STAMTYPE_COUNTER, "/EM/R3/PrivInst/Lidt",               STAMUNIT_OCCURENCES,    "Number of lidt instructions.");
    STAM_REG_USED(pVM, &pStats->StatMovLldt,                STAMTYPE_COUNTER, "/EM/R3/PrivInst/Lldt",               STAMUNIT_OCCURENCES,    "Number of lldt instructions.");
    STAM_REG_USED(pVM, &pStats->StatSysEnter,               STAMTYPE_COUNTER, "/EM/R3/PrivInst/Sysenter",           STAMUNIT_OCCURENCES,    "Number of sysenter instructions.");
    STAM_REG_USED(pVM, &pStats->StatSysExit,                STAMTYPE_COUNTER, "/EM/R3/PrivInst/Sysexit",            STAMUNIT_OCCURENCES,    "Number of sysexit instructions.");
    STAM_REG_USED(pVM, &pStats->StatSysCall,                STAMTYPE_COUNTER, "/EM/R3/PrivInst/Syscall",            STAMUNIT_OCCURENCES,    "Number of syscall instructions.");
    STAM_REG_USED(pVM, &pStats->StatSysRet,                 STAMTYPE_COUNTER, "/EM/R3/PrivInst/Sysret",             STAMUNIT_OCCURENCES,    "Number of sysret instructions.");

    STAM_REG(pVM, &pVM->em.s.StatTotalClis,             STAMTYPE_COUNTER, "/EM/Cli/Total",              STAMUNIT_OCCURENCES,     "Total number of cli instructions executed.");
    pVM->em.s.pCliStatTree = 0;
#endif /* VBOX_WITH_STATISTICS */

    /* these should be considered for release statistics. */
    STAM_REL_REG(pVM, &pVM->em.s.StatForcedActions,     STAMTYPE_PROFILE, "/PROF/EM/ForcedActions",     STAMUNIT_TICKS_PER_CALL, "Profiling forced action execution.");
    STAM_REG(pVM, &pVM->em.s.StatIOEmu,                 STAMTYPE_PROFILE, "/PROF/EM/Emulation/IO",      STAMUNIT_TICKS_PER_CALL, "Profiling of emR3RawExecuteIOInstruction.");
    STAM_REG(pVM, &pVM->em.s.StatPrivEmu,               STAMTYPE_PROFILE, "/PROF/EM/Emulation/Priv",    STAMUNIT_TICKS_PER_CALL, "Profiling of emR3RawPrivileged.");
    STAM_REG(pVM, &pVM->em.s.StatMiscEmu,               STAMTYPE_PROFILE, "/PROF/EM/Emulation/Misc",    STAMUNIT_TICKS_PER_CALL, "Profiling of emR3RawExecuteInstruction.");

    STAM_REL_REG(pVM, &pVM->em.s.StatHalted,            STAMTYPE_PROFILE, "/PROF/EM/Halted",            STAMUNIT_TICKS_PER_CALL, "Profiling halted state (VMR3WaitHalted).");
    STAM_REG(pVM, &pVM->em.s.StatHwAccEntry,            STAMTYPE_PROFILE, "/PROF/EM/HwAccEnter",        STAMUNIT_TICKS_PER_CALL, "Profiling Hardware Accelerated Mode entry overhead.");
    STAM_REG(pVM, &pVM->em.s.StatHwAccExec,             STAMTYPE_PROFILE, "/PROF/EM/HwAccExec",         STAMUNIT_TICKS_PER_CALL, "Profiling Hardware Accelerated Mode execution.");
    STAM_REG(pVM, &pVM->em.s.StatREMEmu,                STAMTYPE_PROFILE, "/PROF/EM/REMEmuSingle",      STAMUNIT_TICKS_PER_CALL, "Profiling single instruction REM execution.");
    STAM_REG(pVM, &pVM->em.s.StatREMExec,               STAMTYPE_PROFILE, "/PROF/EM/REMExec",           STAMUNIT_TICKS_PER_CALL, "Profiling REM execution.");
    STAM_REG(pVM, &pVM->em.s.StatREMSync,               STAMTYPE_PROFILE, "/PROF/EM/REMSync",           STAMUNIT_TICKS_PER_CALL, "Profiling REM context syncing.");
    STAM_REL_REG(pVM, &pVM->em.s.StatREMTotal,          STAMTYPE_PROFILE, "/PROF/EM/REMTotal",          STAMUNIT_TICKS_PER_CALL, "Profiling emR3RemExecute (excluding FFs).");
    STAM_REG(pVM, &pVM->em.s.StatRAWEntry,              STAMTYPE_PROFILE, "/PROF/EM/RAWEnter",          STAMUNIT_TICKS_PER_CALL, "Profiling Raw Mode entry overhead.");
    STAM_REG(pVM, &pVM->em.s.StatRAWExec,               STAMTYPE_PROFILE, "/PROF/EM/RAWExec",           STAMUNIT_TICKS_PER_CALL, "Profiling Raw Mode execution.");
    STAM_REG(pVM, &pVM->em.s.StatRAWTail,               STAMTYPE_PROFILE, "/PROF/EM/RAWTail",           STAMUNIT_TICKS_PER_CALL, "Profiling Raw Mode tail overhead.");
    STAM_REL_REG(pVM, &pVM->em.s.StatRAWTotal,          STAMTYPE_PROFILE, "/PROF/EM/RAWTotal",          STAMUNIT_TICKS_PER_CALL, "Profiling emR3RawExecute (excluding FFs).");
    STAM_REL_REG(pVM, &pVM->em.s.StatTotal,         STAMTYPE_PROFILE_ADV, "/PROF/EM/Total",             STAMUNIT_TICKS_PER_CALL, "Profiling EMR3ExecuteVM.");


    return VINF_SUCCESS;
}


/**
 * Initializes the per-VCPU EM.
 *
 * @returns VBox status code.
 * @param   pVM         The VM to operate on.
 */
VMMR3DECL(int) EMR3InitCPU(PVM pVM)
{
    LogFlow(("EMR3InitCPU\n"));
    return VINF_SUCCESS;
}


/**
 * Applies relocations to data and code managed by this
 * component. This function will be called at init and
 * whenever the VMM need to relocate it self inside the GC.
 *
 * @param   pVM     The VM.
 */
VMMR3DECL(void) EMR3Relocate(PVM pVM)
{
    LogFlow(("EMR3Relocate\n"));
    if (pVM->em.s.pStatsR3)
        pVM->em.s.pStatsRC = MMHyperR3ToRC(pVM, pVM->em.s.pStatsR3);
}


/**
 * Reset notification.
 *
 * @param   pVM
 */
VMMR3DECL(void) EMR3Reset(PVM pVM)
{
    LogFlow(("EMR3Reset: \n"));
    pVM->em.s.fForceRAW = false;
}


/**
 * Terminates the EM.
 *
 * Termination means cleaning up and freeing all resources,
 * the VM it self is at this point powered off or suspended.
 *
 * @returns VBox status code.
 * @param   pVM         The VM to operate on.
 */
VMMR3DECL(int) EMR3Term(PVM pVM)
{
    AssertMsg(pVM->em.s.offVM, ("bad init order!\n"));

    return VINF_SUCCESS;
}

/**
 * Terminates the per-VCPU EM.
 *
 * Termination means cleaning up and freeing all resources,
 * the VM it self is at this point powered off or suspended.
 *
 * @returns VBox status code.
 * @param   pVM         The VM to operate on.
 */
VMMR3DECL(int) EMR3TermCPU(PVM pVM)
{
    return 0;
}

/**
 * Execute state save operation.
 *
 * @returns VBox status code.
 * @param   pVM             VM Handle.
 * @param   pSSM            SSM operation handle.
 */
static DECLCALLBACK(int) emR3Save(PVM pVM, PSSMHANDLE pSSM)
{
    return SSMR3PutBool(pSSM, pVM->em.s.fForceRAW);
}


/**
 * Execute state load operation.
 *
 * @returns VBox status code.
 * @param   pVM             VM Handle.
 * @param   pSSM            SSM operation handle.
 * @param   u32Version      Data layout version.
 */
static DECLCALLBACK(int) emR3Load(PVM pVM, PSSMHANDLE pSSM, uint32_t u32Version)
{
    /*
     * Validate version.
     */
    if (u32Version != EM_SAVED_STATE_VERSION)
    {
        AssertMsgFailed(("emR3Load: Invalid version u32Version=%d (current %d)!\n", u32Version, EM_SAVED_STATE_VERSION));
        return VERR_SSM_UNSUPPORTED_DATA_UNIT_VERSION;
    }

    /*
     * Load the saved state.
     */
    int rc = SSMR3GetBool(pSSM, &pVM->em.s.fForceRAW);
    if (RT_FAILURE(rc))
        pVM->em.s.fForceRAW = false;

    Assert(!pVM->em.s.pCliStatTree);
    return rc;
}


/**
 * Enables or disables a set of raw-mode execution modes.
 *
 * @returns VINF_SUCCESS on success.
 * @returns VINF_RESCHEDULE if a rescheduling might be required.
 * @returns VERR_INVALID_PARAMETER on an invalid enmMode value.
 *
 * @param   pVM         The VM to operate on.
 * @param   enmMode     The execution mode change.
 * @thread  The emulation thread.
 */
VMMR3DECL(int) EMR3RawSetMode(PVM pVM, EMRAWMODE enmMode)
{
    switch (enmMode)
    {
        case EMRAW_NONE:
            pVM->fRawR3Enabled = false;
            pVM->fRawR0Enabled = false;
            break;
        case EMRAW_RING3_ENABLE:
            pVM->fRawR3Enabled = true;
            break;
        case EMRAW_RING3_DISABLE:
            pVM->fRawR3Enabled = false;
            break;
        case EMRAW_RING0_ENABLE:
            pVM->fRawR0Enabled = true;
            break;
        case EMRAW_RING0_DISABLE:
            pVM->fRawR0Enabled = false;
            break;
        default:
            AssertMsgFailed(("Invalid enmMode=%d\n", enmMode));
            return VERR_INVALID_PARAMETER;
    }
    Log(("EMR3SetRawMode: fRawR3Enabled=%RTbool fRawR0Enabled=%RTbool\n",
          pVM->fRawR3Enabled, pVM->fRawR0Enabled));
    return pVM->em.s.enmState == EMSTATE_RAW ? VINF_EM_RESCHEDULE : VINF_SUCCESS;
}


/**
 * Raise a fatal error.
 *
 * Safely terminate the VM with full state report and stuff. This function
 * will naturally never return.
 *
 * @param   pVM         VM handle.
 * @param   rc          VBox status code.
 */
VMMR3DECL(void) EMR3FatalError(PVM pVM, int rc)
{
    longjmp(pVM->em.s.u.FatalLongJump, rc);
    AssertReleaseMsgFailed(("longjmp returned!\n"));
}


/**
 * Gets the EM state name.
 *
 * @returns pointer to read only state name,
 * @param   enmState    The state.
 */
VMMR3DECL(const char *) EMR3GetStateName(EMSTATE enmState)
{
    switch (enmState)
    {
        case EMSTATE_NONE:              return "EMSTATE_NONE";
        case EMSTATE_RAW:               return "EMSTATE_RAW";
        case EMSTATE_HWACC:             return "EMSTATE_HWACC";
        case EMSTATE_REM:               return "EMSTATE_REM";
        case EMSTATE_PARAV:             return "EMSTATE_PARAV";
        case EMSTATE_HALTED:            return "EMSTATE_HALTED";
        case EMSTATE_SUSPENDED:         return "EMSTATE_SUSPENDED";
        case EMSTATE_TERMINATING:       return "EMSTATE_TERMINATING";
        case EMSTATE_DEBUG_GUEST_RAW:   return "EMSTATE_DEBUG_GUEST_RAW";
        case EMSTATE_DEBUG_GUEST_REM:   return "EMSTATE_DEBUG_GUEST_REM";
        case EMSTATE_DEBUG_HYPER:       return "EMSTATE_DEBUG_HYPER";
        case EMSTATE_GURU_MEDITATION:   return "EMSTATE_GURU_MEDITATION";
        default:                        return "Unknown!";
    }
}


#ifdef VBOX_WITH_STATISTICS
/**
 * Just a braindead function to keep track of cli addresses.
 * @param   pVM         VM handle.
 * @param   GCPtrInstr  The EIP of the cli instruction.
 */
static void emR3RecordCli(PVM pVM, RTGCPTR GCPtrInstr)
{
    PCLISTAT pRec;

    pRec = (PCLISTAT)RTAvlPVGet(&pVM->em.s.pCliStatTree, (AVLPVKEY)GCPtrInstr);
    if (!pRec)
    {
        /* New cli instruction; insert into the tree. */
        pRec = (PCLISTAT)MMR3HeapAllocZ(pVM, MM_TAG_EM, sizeof(*pRec));
        Assert(pRec);
        if (!pRec)
            return;
        pRec->Core.Key = (AVLPVKEY)GCPtrInstr;

        char szCliStatName[32];
        RTStrPrintf(szCliStatName, sizeof(szCliStatName), "/EM/Cli/0x%RGv", GCPtrInstr);
        STAM_REG(pVM, &pRec->Counter, STAMTYPE_COUNTER, szCliStatName, STAMUNIT_OCCURENCES, "Number of times cli was executed.");

        bool fRc = RTAvlPVInsert(&pVM->em.s.pCliStatTree, &pRec->Core);
        Assert(fRc); NOREF(fRc);
    }
    STAM_COUNTER_INC(&pRec->Counter);
    STAM_COUNTER_INC(&pVM->em.s.StatTotalClis);
}
#endif /* VBOX_WITH_STATISTICS */


/**
 * Debug loop.
 *
 * @returns VBox status code for EM.
 * @param   pVM     VM handle.
 * @param   rc      Current EM VBox status code..
 */
static int emR3Debug(PVM pVM, int rc)
{
    for (;;)
    {
        Log(("emR3Debug: rc=%Rrc\n", rc));
        const int rcLast = rc;

        /*
         * Debug related RC.
         */
        switch (rc)
        {
            /*
             * Single step an instruction.
             */
            case VINF_EM_DBG_STEP:
                if (    pVM->em.s.enmState == EMSTATE_DEBUG_GUEST_RAW
                    ||  pVM->em.s.enmState == EMSTATE_DEBUG_HYPER
                    ||  pVM->em.s.fForceRAW /* paranoia */)
                    rc = emR3RawStep(pVM);
                else
                {
                    Assert(pVM->em.s.enmState == EMSTATE_DEBUG_GUEST_REM);
                    rc = emR3RemStep(pVM);
                }
                break;

            /*
             * Simple events: stepped, breakpoint, stop/assertion.
             */
            case VINF_EM_DBG_STEPPED:
                rc = DBGFR3Event(pVM, DBGFEVENT_STEPPED);
                break;

            case VINF_EM_DBG_BREAKPOINT:
                rc = DBGFR3EventBreakpoint(pVM, DBGFEVENT_BREAKPOINT);
                break;

            case VINF_EM_DBG_STOP:
                rc = DBGFR3EventSrc(pVM, DBGFEVENT_DEV_STOP, NULL, 0, NULL, NULL);
                break;

            case VINF_EM_DBG_HYPER_STEPPED:
                rc = DBGFR3Event(pVM, DBGFEVENT_STEPPED_HYPER);
                break;

            case VINF_EM_DBG_HYPER_BREAKPOINT:
                rc = DBGFR3EventBreakpoint(pVM, DBGFEVENT_BREAKPOINT_HYPER);
                break;

            case VINF_EM_DBG_HYPER_ASSERTION:
                RTPrintf("\nVINF_EM_DBG_HYPER_ASSERTION:\n%s%s\n", VMMR3GetRZAssertMsg1(pVM), VMMR3GetRZAssertMsg2(pVM));
                rc = DBGFR3EventAssertion(pVM, DBGFEVENT_ASSERTION_HYPER, VMMR3GetRZAssertMsg1(pVM), VMMR3GetRZAssertMsg2(pVM));
                break;

            /*
             * Guru meditation.
             */
            case VERR_VMM_RING0_ASSERTION: /** @todo Make a guru meditation event! */
                rc = DBGFR3EventSrc(pVM, DBGFEVENT_DEV_STOP, "VERR_VMM_RING0_ASSERTION", 0, NULL, NULL);
                break;
            case VERR_REM_TOO_MANY_TRAPS: /** @todo Make a guru meditation event! */
                rc = DBGFR3EventSrc(pVM, DBGFEVENT_DEV_STOP, "VERR_REM_TOO_MANY_TRAPS", 0, NULL, NULL);
                break;

            default: /** @todo don't use default for guru, but make special errors code! */
                rc = DBGFR3Event(pVM, DBGFEVENT_FATAL_ERROR);
                break;
        }

        /*
         * Process the result.
         */
        do
        {
            switch (rc)
            {
                /*
                 * Continue the debugging loop.
                 */
                case VINF_EM_DBG_STEP:
                case VINF_EM_DBG_STOP:
                case VINF_EM_DBG_STEPPED:
                case VINF_EM_DBG_BREAKPOINT:
                case VINF_EM_DBG_HYPER_STEPPED:
                case VINF_EM_DBG_HYPER_BREAKPOINT:
                case VINF_EM_DBG_HYPER_ASSERTION:
                    break;

                /*
                 * Resuming execution (in some form) has to be done here if we got
                 * a hypervisor debug event.
                 */
                case VINF_SUCCESS:
                case VINF_EM_RESUME:
                case VINF_EM_SUSPEND:
                case VINF_EM_RESCHEDULE:
                case VINF_EM_RESCHEDULE_RAW:
                case VINF_EM_RESCHEDULE_REM:
                case VINF_EM_HALT:
                    if (pVM->em.s.enmState == EMSTATE_DEBUG_HYPER)
                    {
                        rc = emR3RawResumeHyper(pVM);
                        if (rc != VINF_SUCCESS && RT_SUCCESS(rc))
                            continue;
                    }
                    if (rc == VINF_SUCCESS)
                        rc = VINF_EM_RESCHEDULE;
                    return rc;

                /*
                 * The debugger isn't attached.
                 * We'll simply turn the thing off since that's the easiest thing to do.
                 */
                case VERR_DBGF_NOT_ATTACHED:
                    switch (rcLast)
                    {
                        case VINF_EM_DBG_HYPER_STEPPED:
                        case VINF_EM_DBG_HYPER_BREAKPOINT:
                        case VINF_EM_DBG_HYPER_ASSERTION:
                        case VERR_TRPM_PANIC:
                        case VERR_TRPM_DONT_PANIC:
                        case VERR_VMM_RING0_ASSERTION:
                            return rcLast;
                    }
                    return VINF_EM_OFF;

                /*
                 * Status codes terminating the VM in one or another sense.
                 */
                case VINF_EM_TERMINATE:
                case VINF_EM_OFF:
                case VINF_EM_RESET:
                case VINF_EM_RAW_STALE_SELECTOR:
                case VINF_EM_RAW_IRET_TRAP:
                case VERR_TRPM_PANIC:
                case VERR_TRPM_DONT_PANIC:
                case VERR_VMM_RING0_ASSERTION:
                case VERR_INTERNAL_ERROR:
                    return rc;

                /*
                 * The rest is unexpected, and will keep us here.
                 */
                default:
                    AssertMsgFailed(("Unxpected rc %Rrc!\n", rc));
                    break;
            }
        } while (false);
    } /* debug for ever */
}


/**
 * Steps recompiled code.
 *
 * @returns VBox status code. The most important ones are: VINF_EM_STEP_EVENT,
 *          VINF_EM_RESCHEDULE, VINF_EM_SUSPEND, VINF_EM_RESET and VINF_EM_TERMINATE.
 *
 * @param   pVM         VM handle.
 */
static int emR3RemStep(PVM pVM)
{
    LogFlow(("emR3RemStep: cs:eip=%04x:%08x\n", CPUMGetGuestCS(pVM),  CPUMGetGuestEIP(pVM)));

    /*
     * Switch to REM, step instruction, switch back.
     */
    int rc = REMR3State(pVM);
    if (RT_SUCCESS(rc))
    {
        rc = REMR3Step(pVM);
        REMR3StateBack(pVM);
    }
    LogFlow(("emR3RemStep: returns %Rrc cs:eip=%04x:%08x\n", rc, CPUMGetGuestCS(pVM),  CPUMGetGuestEIP(pVM)));
    return rc;
}


/**
 * Executes recompiled code.
 *
 * This function contains the recompiler version of the inner
 * execution loop (the outer loop being in EMR3ExecuteVM()).
 *
 * @returns VBox status code. The most important ones are: VINF_EM_RESCHEDULE,
 *          VINF_EM_SUSPEND, VINF_EM_RESET and VINF_EM_TERMINATE.
 *
 * @param   pVM         VM handle.
 * @param   pfFFDone    Where to store an indicator telling wheter or not
 *                      FFs were done before returning.
 *
 */
static int emR3RemExecute(PVM pVM, bool *pfFFDone)
{
#ifdef LOG_ENABLED
    PCPUMCTX pCtx = pVM->em.s.pCtx;
    uint32_t cpl = CPUMGetGuestCPL(pVM, CPUMCTX2CORE(pCtx));

    if (pCtx->eflags.Bits.u1VM)
        Log(("EMV86: %04X:%08X IF=%d\n", pCtx->cs, pCtx->eip, pCtx->eflags.Bits.u1IF));
    else
        Log(("EMR%d: %04X:%08X ESP=%08X IF=%d CR0=%x\n", cpl, pCtx->cs, pCtx->eip, pCtx->esp, pCtx->eflags.Bits.u1IF, (uint32_t)pCtx->cr0));
#endif
    STAM_REL_PROFILE_ADV_START(&pVM->em.s.StatREMTotal, a);

#if defined(VBOX_STRICT) && defined(DEBUG_bird)
    AssertMsg(   VM_FF_ISPENDING(pVM, VM_FF_PGM_SYNC_CR3|VM_FF_PGM_SYNC_CR3_NON_GLOBAL)
              || !MMHyperIsInsideArea(pVM, CPUMGetGuestEIP(pVM)),  /** @todo #1419 - get flat address. */
              ("cs:eip=%RX16:%RX32\n", CPUMGetGuestCS(pVM), CPUMGetGuestEIP(pVM)));
#endif

    /*
     * Spin till we get a forced action which returns anything but VINF_SUCCESS
     * or the REM suggests raw-mode execution.
     */
    *pfFFDone = false;
    bool    fInREMState = false;
    int     rc = VINF_SUCCESS;
    for (;;)
    {
        /*
         * Update REM state if not already in sync.
         */
        if (!fInREMState)
        {
            STAM_PROFILE_START(&pVM->em.s.StatREMSync, b);
            rc = REMR3State(pVM);
            STAM_PROFILE_STOP(&pVM->em.s.StatREMSync, b);
            if (RT_FAILURE(rc))
                break;
            fInREMState = true;

            /*
             * We might have missed the raising of VMREQ, TIMER and some other
             * imporant FFs while we were busy switching the state. So, check again.
             */
            if (VM_FF_ISPENDING(pVM, VM_FF_REQUEST | VM_FF_TIMER | VM_FF_PDM_QUEUES | VM_FF_DBGF | VM_FF_TERMINATE | VM_FF_RESET))
            {
                LogFlow(("emR3RemExecute: Skipping run, because FF is set. %#x\n", pVM->fForcedActions));
                goto l_REMDoForcedActions;
            }
        }


        /*
         * Execute REM.
         */
        STAM_PROFILE_START(&pVM->em.s.StatREMExec, c);
        rc = REMR3Run(pVM);
        STAM_PROFILE_STOP(&pVM->em.s.StatREMExec, c);


        /*
         * Deal with high priority post execution FFs before doing anything else.
         */
        if (VM_FF_ISPENDING(pVM, VM_FF_HIGH_PRIORITY_POST_MASK))
            rc = emR3HighPriorityPostForcedActions(pVM, rc);

        /*
         * Process the returned status code.
         * (Try keep this short! Call functions!)
         */
        if (rc != VINF_SUCCESS)
        {
            if (rc >= VINF_EM_FIRST && rc <= VINF_EM_LAST)
                break;
            if (rc != VINF_REM_INTERRUPED_FF)
            {
                /*
                 * Anything which is not known to us means an internal error
                 * and the termination of the VM!
                 */
                AssertMsg(rc == VERR_REM_TOO_MANY_TRAPS, ("Unknown GC return code: %Rra\n", rc));
                break;
            }
        }


        /*
         * Check and execute forced actions.
         * Sync back the VM state before calling any of these.
         */
#ifdef VBOX_HIGH_RES_TIMERS_HACK
        TMTimerPoll(pVM);
#endif
        if (VM_FF_ISPENDING(pVM, VM_FF_ALL_BUT_RAW_MASK & ~(VM_FF_CSAM_PENDING_ACTION | VM_FF_CSAM_SCAN_PAGE)))
        {
l_REMDoForcedActions:
            if (fInREMState)
            {
                STAM_PROFILE_START(&pVM->em.s.StatREMSync, d);
                REMR3StateBack(pVM);
                STAM_PROFILE_STOP(&pVM->em.s.StatREMSync, d);
                fInREMState = false;
            }
            STAM_REL_PROFILE_ADV_SUSPEND(&pVM->em.s.StatREMTotal, a);
            rc = emR3ForcedActions(pVM, rc);
            STAM_REL_PROFILE_ADV_RESUME(&pVM->em.s.StatREMTotal, a);
            if (    rc != VINF_SUCCESS
                &&  rc != VINF_EM_RESCHEDULE_REM)
            {
                *pfFFDone = true;
                break;
            }
        }

    } /* The Inner Loop, recompiled execution mode version. */


    /*
     * Returning. Sync back the VM state if required.
     */
    if (fInREMState)
    {
        STAM_PROFILE_START(&pVM->em.s.StatREMSync, e);
        REMR3StateBack(pVM);
        STAM_PROFILE_STOP(&pVM->em.s.StatREMSync, e);
    }

    STAM_REL_PROFILE_ADV_STOP(&pVM->em.s.StatREMTotal, a);
    return rc;
}


/**
 * Resumes executing hypervisor after a debug event.
 *
 * This is kind of special since our current guest state is
 * potentially out of sync.
 *
 * @returns VBox status code.
 * @param   pVM     The VM handle.
 */
static int emR3RawResumeHyper(PVM pVM)
{
    int         rc;
    PCPUMCTX    pCtx = pVM->em.s.pCtx;
    Assert(pVM->em.s.enmState == EMSTATE_DEBUG_HYPER);
    Log(("emR3RawResumeHyper: cs:eip=%RTsel:%RGr efl=%RGr\n", pCtx->cs, pCtx->eip, pCtx->eflags));

    /*
     * Resume execution.
     */
    CPUMRawEnter(pVM, NULL);
    CPUMSetHyperEFlags(pVM, CPUMGetHyperEFlags(pVM) | X86_EFL_RF);
    rc = VMMR3ResumeHyper(pVM);
    Log(("emR3RawStep: cs:eip=%RTsel:%RGr efl=%RGr - returned from GC with rc=%Rrc\n", pCtx->cs, pCtx->eip, pCtx->eflags, rc));
    rc = CPUMRawLeave(pVM, NULL, rc);
    VM_FF_CLEAR(pVM, VM_FF_RESUME_GUEST_MASK);

    /*
     * Deal with the return code.
     */
    rc = emR3HighPriorityPostForcedActions(pVM, rc);
    rc = emR3RawHandleRC(pVM, pCtx, rc);
    rc = emR3RawUpdateForceFlag(pVM, pCtx, rc);
    return rc;
}


/**
 * Steps rawmode.
 *
 * @returns VBox status code.
 * @param   pVM     The VM handle.
 */
static int emR3RawStep(PVM pVM)
{
    Assert(   pVM->em.s.enmState == EMSTATE_DEBUG_HYPER
           || pVM->em.s.enmState == EMSTATE_DEBUG_GUEST_RAW
           || pVM->em.s.enmState == EMSTATE_DEBUG_GUEST_REM);
    int         rc;
    PCPUMCTX    pCtx   = pVM->em.s.pCtx;
    bool        fGuest = pVM->em.s.enmState != EMSTATE_DEBUG_HYPER;
#ifndef DEBUG_sandervl
    Log(("emR3RawStep: cs:eip=%RTsel:%RGr efl=%RGr\n", fGuest ? CPUMGetGuestCS(pVM) : CPUMGetHyperCS(pVM),
         fGuest ? CPUMGetGuestEIP(pVM) : CPUMGetHyperEIP(pVM), fGuest ? CPUMGetGuestEFlags(pVM) : CPUMGetHyperEFlags(pVM)));
#endif
    if (fGuest)
    {
        /*
         * Check vital forced actions, but ignore pending interrupts and timers.
         */
        if (VM_FF_ISPENDING(pVM, VM_FF_HIGH_PRIORITY_PRE_RAW_MASK))
        {
            rc = emR3RawForcedActions(pVM, pCtx);
            if (RT_FAILURE(rc))
                return rc;
        }

        /*
         * Set flags for single stepping.
         */
        CPUMSetGuestEFlags(pVM, CPUMGetGuestEFlags(pVM) | X86_EFL_TF | X86_EFL_RF);
    }
    else
        CPUMSetHyperEFlags(pVM, CPUMGetHyperEFlags(pVM) | X86_EFL_TF | X86_EFL_RF);

    /*
     * Single step.
     * We do not start time or anything, if anything we should just do a few nanoseconds.
     */
    CPUMRawEnter(pVM, NULL);
    do
    {
        if (pVM->em.s.enmState == EMSTATE_DEBUG_HYPER)
            rc = VMMR3ResumeHyper(pVM);
        else
            rc = VMMR3RawRunGC(pVM);
#ifndef DEBUG_sandervl
        Log(("emR3RawStep: cs:eip=%RTsel:%RGr efl=%RGr - GC rc %Rrc\n", fGuest ? CPUMGetGuestCS(pVM) : CPUMGetHyperCS(pVM),
             fGuest ? CPUMGetGuestEIP(pVM) : CPUMGetHyperEIP(pVM), fGuest ? CPUMGetGuestEFlags(pVM) : CPUMGetHyperEFlags(pVM), rc));
#endif
    } while (   rc == VINF_SUCCESS
             || rc == VINF_EM_RAW_INTERRUPT);
    rc = CPUMRawLeave(pVM, NULL, rc);
    VM_FF_CLEAR(pVM, VM_FF_RESUME_GUEST_MASK);

    /*
     * Make sure the trap flag is cleared.
     * (Too bad if the guest is trying to single step too.)
     */
    if (fGuest)
        CPUMSetGuestEFlags(pVM, CPUMGetGuestEFlags(pVM) & ~X86_EFL_TF);
    else
        CPUMSetHyperEFlags(pVM, CPUMGetHyperEFlags(pVM) & ~X86_EFL_TF);

    /*
     * Deal with the return codes.
     */
    rc = emR3HighPriorityPostForcedActions(pVM, rc);
    rc = emR3RawHandleRC(pVM, pCtx, rc);
    rc = emR3RawUpdateForceFlag(pVM, pCtx, rc);
    return rc;
}


#ifdef DEBUG

/**
 * Steps hardware accelerated mode.
 *
 * @returns VBox status code.
 * @param   pVM     The VM handle.
 * @param   idCpu   VMCPU id.
 */
static int emR3HwAccStep(PVM pVM, RTCPUID idCpu)
{
    Assert(pVM->em.s.enmState == EMSTATE_DEBUG_GUEST_HWACC);

    int         rc;
    PCPUMCTX    pCtx   = pVM->em.s.pCtx;
    VM_FF_CLEAR(pVM, (VM_FF_SELM_SYNC_GDT | VM_FF_SELM_SYNC_LDT | VM_FF_TRPM_SYNC_IDT | VM_FF_SELM_SYNC_TSS));

    /*
     * Check vital forced actions, but ignore pending interrupts and timers.
     */
    if (VM_FF_ISPENDING(pVM, VM_FF_HIGH_PRIORITY_PRE_RAW_MASK))
    {
        rc = emR3RawForcedActions(pVM, pCtx);
        if (RT_FAILURE(rc))
            return rc;
    }
    /*
     * Set flags for single stepping.
     */
    CPUMSetGuestEFlags(pVM, CPUMGetGuestEFlags(pVM) | X86_EFL_TF | X86_EFL_RF);

    /*
     * Single step.
     * We do not start time or anything, if anything we should just do a few nanoseconds.
     */
    do
    {
        rc = VMMR3HwAccRunGC(pVM, idCpu);
    } while (   rc == VINF_SUCCESS
             || rc == VINF_EM_RAW_INTERRUPT);
    VM_FF_CLEAR(pVM, VM_FF_RESUME_GUEST_MASK);

    /*
     * Make sure the trap flag is cleared.
     * (Too bad if the guest is trying to single step too.)
     */
    CPUMSetGuestEFlags(pVM, CPUMGetGuestEFlags(pVM) & ~X86_EFL_TF);

    /*
     * Deal with the return codes.
     */
    rc = emR3HighPriorityPostForcedActions(pVM, rc);
    rc = emR3RawHandleRC(pVM, pCtx, rc);
    rc = emR3RawUpdateForceFlag(pVM, pCtx, rc);
    return rc;
}


void emR3SingleStepExecRaw(PVM pVM, uint32_t cIterations)
{
    EMSTATE  enmOldState = pVM->em.s.enmState;

    pVM->em.s.enmState = EMSTATE_DEBUG_GUEST_RAW;

    Log(("Single step BEGIN:\n"));
    for (uint32_t i = 0; i < cIterations; i++)
    {
        DBGFR3PrgStep(pVM);
        DBGFR3DisasInstrCurrentLog(pVM, "RSS: ");
        emR3RawStep(pVM);
    }
    Log(("Single step END:\n"));
    CPUMSetGuestEFlags(pVM, CPUMGetGuestEFlags(pVM) & ~X86_EFL_TF);
    pVM->em.s.enmState = enmOldState;
}


void emR3SingleStepExecHwAcc(PVM pVM, RTCPUID idCpu, uint32_t cIterations)
{
    EMSTATE  enmOldState = pVM->em.s.enmState;

    pVM->em.s.enmState = EMSTATE_DEBUG_GUEST_HWACC;

    Log(("Single step BEGIN:\n"));
    for (uint32_t i = 0; i < cIterations; i++)
    {
        DBGFR3PrgStep(pVM);
        DBGFR3DisasInstrCurrentLog(pVM, "RSS: ");
        emR3HwAccStep(pVM, idCpu);
    }
    Log(("Single step END:\n"));
    CPUMSetGuestEFlags(pVM, CPUMGetGuestEFlags(pVM) & ~X86_EFL_TF);
    pVM->em.s.enmState = enmOldState;
}


void emR3SingleStepExecRem(PVM pVM, uint32_t cIterations)
{
    EMSTATE  enmOldState = pVM->em.s.enmState;

    pVM->em.s.enmState = EMSTATE_DEBUG_GUEST_REM;

    Log(("Single step BEGIN:\n"));
    for (uint32_t i = 0; i < cIterations; i++)
    {
        DBGFR3PrgStep(pVM);
        DBGFR3DisasInstrCurrentLog(pVM, "RSS: ");
        emR3RemStep(pVM);
    }
    Log(("Single step END:\n"));
    CPUMSetGuestEFlags(pVM, CPUMGetGuestEFlags(pVM) & ~X86_EFL_TF);
    pVM->em.s.enmState = enmOldState;
}

#endif /* DEBUG */


/**
 * Executes one (or perhaps a few more) instruction(s).
 *
 * @returns VBox status code suitable for EM.
 *
 * @param   pVM         VM handle.
 * @param   rcGC        GC return code
 * @param   pszPrefix   Disassembly prefix. If not NULL we'll disassemble the
 *                      instruction and prefix the log output with this text.
 */
#ifdef LOG_ENABLED
static int emR3RawExecuteInstructionWorker(PVM pVM, int rcGC, const char *pszPrefix)
#else
static int emR3RawExecuteInstructionWorker(PVM pVM, int rcGC)
#endif
{
    PCPUMCTX pCtx = pVM->em.s.pCtx;
    int      rc;

    /*
     *
     * The simple solution is to use the recompiler.
     * The better solution is to disassemble the current instruction and
     * try handle as many as possible without using REM.
     *
     */

#ifdef LOG_ENABLED
    /*
     * Disassemble the instruction if requested.
     */
    if (pszPrefix)
    {
        DBGFR3InfoLog(pVM, "cpumguest", pszPrefix);
        DBGFR3DisasInstrCurrentLog(pVM, pszPrefix);
    }
#endif /* LOG_ENABLED */

    /*
     * PATM is making life more interesting.
     * We cannot hand anything to REM which has an EIP inside patch code. So, we'll
     * tell PATM there is a trap in this code and have it take the appropriate actions
     * to allow us execute the code in REM.
     */
    if (PATMIsPatchGCAddr(pVM, pCtx->eip))
    {
        Log(("emR3RawExecuteInstruction: In patch block. eip=%RRv\n", (RTRCPTR)pCtx->eip));

        RTGCPTR pNewEip;
        rc = PATMR3HandleTrap(pVM, pCtx, pCtx->eip, &pNewEip);
        switch (rc)
        {
            /*
             * It's not very useful to emulate a single instruction and then go back to raw
             * mode; just execute the whole block until IF is set again.
             */
            case VINF_SUCCESS:
                Log(("emR3RawExecuteInstruction: Executing instruction starting at new address %RGv IF=%d VMIF=%x\n",
                     pNewEip, pCtx->eflags.Bits.u1IF, pVM->em.s.pPatmGCState->uVMFlags));
                pCtx->eip = pNewEip;
                Assert(pCtx->eip);

                if (pCtx->eflags.Bits.u1IF)
                {
                    /*
                     * The last instruction in the patch block needs to be executed!! (sti/sysexit for example)
                     */
                    Log(("PATCH: IF=1 -> emulate last instruction as it can't be interrupted!!\n"));
                    return emR3RawExecuteInstruction(pVM, "PATCHIR");
                }
                else if (rcGC == VINF_PATM_PENDING_IRQ_AFTER_IRET)
                {
                    /* special case: iret, that sets IF,  detected a pending irq/event */
                    return emR3RawExecuteInstruction(pVM, "PATCHIRET");
                }
                return VINF_EM_RESCHEDULE_REM;

            /*
             * One instruction.
             */
            case VINF_PATCH_EMULATE_INSTR:
                Log(("emR3RawExecuteInstruction: Emulate patched instruction at %RGv IF=%d VMIF=%x\n",
                     pNewEip, pCtx->eflags.Bits.u1IF, pVM->em.s.pPatmGCState->uVMFlags));
                pCtx->eip = pNewEip;
                return emR3RawExecuteInstruction(pVM, "PATCHIR");

            /*
             * The patch was disabled, hand it to the REM.
             */
            case VERR_PATCH_DISABLED:
                Log(("emR3RawExecuteInstruction: Disabled patch -> new eip %RGv IF=%d VMIF=%x\n",
                     pNewEip, pCtx->eflags.Bits.u1IF, pVM->em.s.pPatmGCState->uVMFlags));
                pCtx->eip = pNewEip;
                if (pCtx->eflags.Bits.u1IF)
                {
                    /*
                     * The last instruction in the patch block needs to be executed!! (sti/sysexit for example)
                     */
                    Log(("PATCH: IF=1 -> emulate last instruction as it can't be interrupted!!\n"));
                    return emR3RawExecuteInstruction(pVM, "PATCHIR");
                }
                return VINF_EM_RESCHEDULE_REM;

            /* Force continued patch exection; usually due to write monitored stack. */
            case VINF_PATCH_CONTINUE:
                return VINF_SUCCESS;

            default:
                AssertReleaseMsgFailed(("Unknown return code %Rrc from PATMR3HandleTrap\n", rc));
                return VERR_INTERNAL_ERROR;
        }
    }

#if 0
    /* Try our own instruction emulator before falling back to the recompiler. */
    DISCPUSTATE Cpu;
    rc = CPUMR3DisasmInstrCPU(pVM, pCtx, pCtx->rip, &Cpu, "GEN EMU");
    if (RT_SUCCESS(rc))
    {
        uint32_t size;

        switch (Cpu.pCurInstr->opcode)
        {
        /* @todo we can do more now */
        case OP_MOV:
        case OP_AND:
        case OP_OR:
        case OP_XOR:
        case OP_POP:
        case OP_INC:
        case OP_DEC:
        case OP_XCHG:
            STAM_PROFILE_START(&pVM->em.s.StatMiscEmu, a);
            rc = EMInterpretInstructionCPU(pVM, &Cpu, CPUMCTX2CORE(pCtx), 0, &size);
            if (RT_SUCCESS(rc))
            {
                pCtx->rip += Cpu.opsize;
                STAM_PROFILE_STOP(&pVM->em.s.StatMiscEmu, a);
                return rc;
            }
            if (rc != VERR_EM_INTERPRETER)
                AssertMsgFailedReturn(("rc=%Rrc\n", rc), rc);
            STAM_PROFILE_STOP(&pVM->em.s.StatMiscEmu, a);
            break;
        }
    }
#endif /* 0 */
    STAM_PROFILE_START(&pVM->em.s.StatREMEmu, a);
    rc = REMR3EmulateInstruction(pVM);
    STAM_PROFILE_STOP(&pVM->em.s.StatREMEmu, a);

    return rc;
}


/**
 * Executes one (or perhaps a few more) instruction(s).
 * This is just a wrapper for discarding pszPrefix in non-logging builds.
 *
 * @returns VBox status code suitable for EM.
 * @param   pVM         VM handle.
 * @param   pszPrefix   Disassembly prefix. If not NULL we'll disassemble the
 *                      instruction and prefix the log output with this text.
 * @param   rcGC        GC return code
 */
DECLINLINE(int) emR3RawExecuteInstruction(PVM pVM, const char *pszPrefix, int rcGC)
{
#ifdef LOG_ENABLED
    return emR3RawExecuteInstructionWorker(pVM, rcGC, pszPrefix);
#else
    return emR3RawExecuteInstructionWorker(pVM, rcGC);
#endif
}

/**
 * Executes one (or perhaps a few more) IO instruction(s).
 *
 * @returns VBox status code suitable for EM.
 * @param   pVM         VM handle.
 */
int emR3RawExecuteIOInstruction(PVM pVM)
{
    int         rc;
    PCPUMCTX    pCtx = pVM->em.s.pCtx;

    STAM_PROFILE_START(&pVM->em.s.StatIOEmu, a);

    /** @todo probably we should fall back to the recompiler; otherwise we'll go back and forth between HC & GC
     *   as io instructions tend to come in packages of more than one
     */
    DISCPUSTATE Cpu;
    rc = CPUMR3DisasmInstrCPU(pVM, pCtx, pCtx->rip, &Cpu, "IO EMU");
    if (RT_SUCCESS(rc))
    {
        rc = VINF_EM_RAW_EMULATE_INSTR;

        if (!(Cpu.prefix & (PREFIX_REP | PREFIX_REPNE)))
        {
            switch (Cpu.pCurInstr->opcode)
            {
                case OP_IN:
                {
                    STAM_COUNTER_INC(&pVM->em.s.CTX_SUFF(pStats)->StatIn);
                    rc = IOMInterpretIN(pVM, CPUMCTX2CORE(pCtx), &Cpu);
                    break;
                }

                case OP_OUT:
                {
                    STAM_COUNTER_INC(&pVM->em.s.CTX_SUFF(pStats)->StatOut);
                    rc = IOMInterpretOUT(pVM, CPUMCTX2CORE(pCtx), &Cpu);
                    break;
                }
            }
        }
        else if (Cpu.prefix & PREFIX_REP)
        {
            switch (Cpu.pCurInstr->opcode)
            {
                case OP_INSB:
                case OP_INSWD:
                {
                    STAM_COUNTER_INC(&pVM->em.s.CTX_SUFF(pStats)->StatIn);
                    rc = IOMInterpretINS(pVM, CPUMCTX2CORE(pCtx), &Cpu);
                    break;
                }

                case OP_OUTSB:
                case OP_OUTSWD:
                {
                    STAM_COUNTER_INC(&pVM->em.s.CTX_SUFF(pStats)->StatOut);
                    rc = IOMInterpretOUTS(pVM, CPUMCTX2CORE(pCtx), &Cpu);
                    break;
                }
            }
        }

        /*
         * Handled the I/O return codes.
         * (The unhandled cases end up with rc == VINF_EM_RAW_EMULATE_INSTR.)
         */
        if (IOM_SUCCESS(rc))
        {
            pCtx->rip += Cpu.opsize;
            STAM_PROFILE_STOP(&pVM->em.s.StatIOEmu, a);
            return rc;
        }

        if (rc == VINF_EM_RAW_GUEST_TRAP)
        {
            STAM_PROFILE_STOP(&pVM->em.s.StatIOEmu, a);
            rc = emR3RawGuestTrap(pVM);
            return rc;
        }
        AssertMsg(rc != VINF_TRPM_XCPT_DISPATCHED, ("Handle VINF_TRPM_XCPT_DISPATCHED\n"));

        if (RT_FAILURE(rc))
        {
            STAM_PROFILE_STOP(&pVM->em.s.StatIOEmu, a);
            return rc;
        }
        AssertMsg(rc == VINF_EM_RAW_EMULATE_INSTR || rc == VINF_EM_RESCHEDULE_REM, ("rc=%Rrc\n", rc));
    }
    STAM_PROFILE_STOP(&pVM->em.s.StatIOEmu, a);
    return emR3RawExecuteInstruction(pVM, "IO: ");
}


/**
 * Handle a guest context trap.
 *
 * @returns VBox status code suitable for EM.
 * @param   pVM         VM handle.
 */
static int emR3RawGuestTrap(PVM pVM)
{
    PCPUMCTX pCtx = pVM->em.s.pCtx;

    /*
     * Get the trap info.
     */
    uint8_t         u8TrapNo;
    TRPMEVENT       enmType;
    RTGCUINT        uErrorCode;
    RTGCUINTPTR     uCR2;
    int rc = TRPMQueryTrapAll(pVM, &u8TrapNo, &enmType, &uErrorCode, &uCR2);
    if (RT_FAILURE(rc))
    {
        AssertReleaseMsgFailed(("No trap! (rc=%Rrc)\n", rc));
        return rc;
    }

    /*
     * Traps can be directly forwarded in hardware accelerated mode.
     */
    if (HWACCMR3IsActive(pVM))
    {
#ifdef LOGGING_ENABLED
        DBGFR3InfoLog(pVM, "cpumguest", "Guest trap");
        DBGFR3DisasInstrCurrentLog(pVM, "Guest trap");
#endif
        return VINF_EM_RESCHEDULE_HWACC;
    }

#if 1 /* Experimental: Review, disable if it causes trouble. */
    /*
     * Handle traps in patch code first.
     *
     * We catch a few of these cases in RC before returning to R3 (#PF, #GP, #BP)
     * but several traps isn't handled specially by TRPM in RC and we end up here
     * instead. One example is #DE.
     */
    uint32_t uCpl = CPUMGetGuestCPL(pVM, CPUMCTX2CORE(pCtx));
    if (    uCpl == 0
        &&  PATMIsPatchGCAddr(pVM, (RTGCPTR)pCtx->eip))
    {
        LogFlow(("emR3RawGuestTrap: trap %#x in patch code; eip=%08x\n", u8TrapNo, pCtx->eip));
        return emR3PatchTrap(pVM, pCtx, rc);
    }
#endif

    /*
     * If the guest gate is marked unpatched, then we will check again if we can patch it.
     * (This assumes that we've already tried and failed to dispatch the trap in
     * RC for the gates that already has been patched. Which is true for most high
     * volume traps, because these are handled specially, but not for odd ones like #DE.)
     */
    if (TRPMR3GetGuestTrapHandler(pVM, u8TrapNo) == TRPM_INVALID_HANDLER)
    {
        CSAMR3CheckGates(pVM, u8TrapNo, 1);
        Log(("emR3RawHandleRC: recheck gate %x -> valid=%d\n", u8TrapNo, TRPMR3GetGuestTrapHandler(pVM, u8TrapNo) != TRPM_INVALID_HANDLER));

        /* If it was successful, then we could go back to raw mode. */
        if (TRPMR3GetGuestTrapHandler(pVM, u8TrapNo) != TRPM_INVALID_HANDLER)
        {
            /* Must check pending forced actions as our IDT or GDT might be out of sync. */
            rc = EMR3CheckRawForcedActions(pVM);
            AssertRCReturn(rc, rc);

            TRPMERRORCODE enmError = uErrorCode != ~0U
                                   ? TRPM_TRAP_HAS_ERRORCODE
                                   : TRPM_TRAP_NO_ERRORCODE;
            rc = TRPMForwardTrap(pVM, CPUMCTX2CORE(pCtx), u8TrapNo, uErrorCode, enmError, TRPM_TRAP, -1);
            if (rc == VINF_SUCCESS /* Don't use RT_SUCCESS */)
            {
                TRPMResetTrap(pVM);
                return VINF_EM_RESCHEDULE_RAW;
            }
            AssertMsg(rc == VINF_EM_RAW_GUEST_TRAP, ("%Rrc\n", rc));
        }
    }

    /*
     * Scan kernel code that traps; we might not get another chance.
     */
    /** @todo move this up before the dispatching? */
    if (    (pCtx->ss & X86_SEL_RPL) <= 1
        &&  !pCtx->eflags.Bits.u1VM)
    {
        Assert(!PATMIsPatchGCAddr(pVM, pCtx->eip));
        CSAMR3CheckCodeEx(pVM, CPUMCTX2CORE(pCtx), pCtx->eip);
    }

    /*
     * Trap specific handling.
     */
    if (u8TrapNo == 6) /* (#UD) Invalid opcode. */
    {
        /*
         * If MONITOR & MWAIT are supported, then interpret them here.
         */
        DISCPUSTATE cpu;
        rc = CPUMR3DisasmInstrCPU(pVM, pCtx, pCtx->rip, &cpu, "Guest Trap (#UD): ");
        if (    RT_SUCCESS(rc)
            && (cpu.pCurInstr->opcode == OP_MONITOR || cpu.pCurInstr->opcode == OP_MWAIT))
        {
            uint32_t u32Dummy, u32Features, u32ExtFeatures;
            CPUMGetGuestCpuId(pVM, 1, &u32Dummy, &u32Dummy, &u32ExtFeatures, &u32Features);
            if (u32ExtFeatures & X86_CPUID_FEATURE_ECX_MONITOR)
            {
                rc = TRPMResetTrap(pVM);
                AssertRC(rc);

                uint32_t opsize;
                rc = EMInterpretInstructionCPU(pVM, &cpu, CPUMCTX2CORE(pCtx), 0, &opsize);
                if (RT_SUCCESS(rc))
                {
                    pCtx->rip += cpu.opsize;
                    return rc;
                }
                return emR3RawExecuteInstruction(pVM, "Monitor: ");
            }
        }
    }
    else if (u8TrapNo == 13) /* (#GP) Privileged exception */
    {
        /*
         * Handle I/O bitmap?
         */
        /** @todo We're not supposed to be here with a false guest trap concerning
         *        I/O access. We can easily handle those in RC.  */
        DISCPUSTATE cpu;
        rc = CPUMR3DisasmInstrCPU(pVM, pCtx, pCtx->rip, &cpu, "Guest Trap: ");
        if (    RT_SUCCESS(rc)
            &&  (cpu.pCurInstr->optype & OPTYPE_PORTIO))
        {
            /*
             * We should really check the TSS for the IO bitmap, but it's not like this
             * lazy approach really makes things worse.
             */
            rc = TRPMResetTrap(pVM);
            AssertRC(rc);
            return emR3RawExecuteInstruction(pVM, "IO Guest Trap: ");
        }
    }

#ifdef LOG_ENABLED
    DBGFR3InfoLog(pVM, "cpumguest", "Guest trap");
    DBGFR3DisasInstrCurrentLog(pVM, "Guest trap");

    /* Get guest page information. */
    uint64_t    fFlags = 0;
    RTGCPHYS    GCPhys = 0;
    int rc2 = PGMGstGetPage(pVM, uCR2, &fFlags, &GCPhys);
    Log(("emR3RawGuestTrap: cs:eip=%04x:%08x: trap=%02x err=%08x cr2=%08x cr0=%08x%s: Phys=%RGp fFlags=%08llx %s %s %s%s rc2=%d\n",
         pCtx->cs, pCtx->eip, u8TrapNo, uErrorCode, uCR2, (uint32_t)pCtx->cr0, (enmType == TRPM_SOFTWARE_INT) ? " software" : "",  GCPhys, fFlags,
         fFlags & X86_PTE_P  ? "P " : "NP", fFlags & X86_PTE_US ? "U"  : "S",
         fFlags & X86_PTE_RW ? "RW" : "R0", fFlags & X86_PTE_G  ? " G" : "", rc2));
#endif

    /*
     * #PG has CR2.
     * (Because of stuff like above we must set CR2 in a delayed fashion.)
     */
    if (u8TrapNo == 14 /* #PG */)
        pCtx->cr2 = uCR2;

    return VINF_EM_RESCHEDULE_REM;
}


/**
 * Handle a ring switch trap.
 * Need to do statistics and to install patches. The result is going to REM.
 *
 * @returns VBox status code suitable for EM.
 * @param   pVM     VM handle.
 */
int emR3RawRingSwitch(PVM pVM)
{
    int         rc;
    DISCPUSTATE Cpu;
    PCPUMCTX    pCtx = pVM->em.s.pCtx;

    /*
     * sysenter, syscall & callgate
     */
    rc = CPUMR3DisasmInstrCPU(pVM, pCtx, pCtx->rip, &Cpu, "RSWITCH: ");
    if (RT_SUCCESS(rc))
    {
        if (Cpu.pCurInstr->opcode == OP_SYSENTER)
        {
            if (pCtx->SysEnter.cs != 0)
            {
                rc = PATMR3InstallPatch(pVM, SELMToFlat(pVM, DIS_SELREG_CS, CPUMCTX2CORE(pCtx), pCtx->eip),
                                        (SELMGetCpuModeFromSelector(pVM, pCtx->eflags, pCtx->cs, &pCtx->csHid) == CPUMODE_32BIT) ? PATMFL_CODE32 : 0);
                if (RT_SUCCESS(rc))
                {
                    DBGFR3DisasInstrCurrentLog(pVM, "Patched sysenter instruction");
                    return VINF_EM_RESCHEDULE_RAW;
                }
            }
        }

#ifdef VBOX_WITH_STATISTICS
        switch (Cpu.pCurInstr->opcode)
        {
            case OP_SYSENTER:
                STAM_COUNTER_INC(&pVM->em.s.CTX_SUFF(pStats)->StatSysEnter);
                break;
            case OP_SYSEXIT:
                STAM_COUNTER_INC(&pVM->em.s.CTX_SUFF(pStats)->StatSysExit);
                break;
            case OP_SYSCALL:
                STAM_COUNTER_INC(&pVM->em.s.CTX_SUFF(pStats)->StatSysCall);
                break;
            case OP_SYSRET:
                STAM_COUNTER_INC(&pVM->em.s.CTX_SUFF(pStats)->StatSysRet);
                break;
        }
#endif
    }
    else
        AssertRC(rc);

    /* go to the REM to emulate a single instruction */
    return emR3RawExecuteInstruction(pVM, "RSWITCH: ");
}


/**
 * Handle a trap (\#PF or \#GP) in patch code
 *
 * @returns VBox status code suitable for EM.
 * @param   pVM     VM handle.
 * @param   pCtx    CPU context
 * @param   gcret   GC return code
 */
static int emR3PatchTrap(PVM pVM, PCPUMCTX pCtx, int gcret)
{
    uint8_t         u8TrapNo;
    int             rc;
    TRPMEVENT       enmType;
    RTGCUINT        uErrorCode;
    RTGCUINTPTR     uCR2;

    Assert(PATMIsPatchGCAddr(pVM, pCtx->eip));

    if (gcret == VINF_PATM_PATCH_INT3)
    {
        u8TrapNo   = 3;
        uCR2       = 0;
        uErrorCode = 0;
    }
    else if (gcret == VINF_PATM_PATCH_TRAP_GP)
    {
        /* No active trap in this case. Kind of ugly. */
        u8TrapNo   = X86_XCPT_GP;
        uCR2       = 0;
        uErrorCode = 0;
    }
    else
    {
        rc = TRPMQueryTrapAll(pVM, &u8TrapNo, &enmType, &uErrorCode, &uCR2);
        if (RT_FAILURE(rc))
        {
            AssertReleaseMsgFailed(("emR3PatchTrap: no trap! (rc=%Rrc) gcret=%Rrc\n", rc, gcret));
            return rc;
        }
        /* Reset the trap as we'll execute the original instruction again. */
        TRPMResetTrap(pVM);
    }

    /*
     * Deal with traps inside patch code.
     * (This code won't run outside GC.)
     */
    if (u8TrapNo != 1)
    {
#ifdef LOG_ENABLED
        DBGFR3InfoLog(pVM, "cpumguest", "Trap in patch code");
        DBGFR3DisasInstrCurrentLog(pVM, "Patch code");

        DISCPUSTATE Cpu;
        int         rc;

        rc = CPUMR3DisasmInstrCPU(pVM, pCtx, pCtx->eip, &Cpu, "Patch code: ");
        if (    RT_SUCCESS(rc)
            &&  Cpu.pCurInstr->opcode == OP_IRET)
        {
            uint32_t eip, selCS, uEFlags;

            /* Iret crashes are bad as we have already changed the flags on the stack */
            rc  = PGMPhysSimpleReadGCPtr(pVM, &eip,     pCtx->esp, 4);
            rc |= PGMPhysSimpleReadGCPtr(pVM, &selCS,   pCtx->esp+4, 4);
            rc |= PGMPhysSimpleReadGCPtr(pVM, &uEFlags, pCtx->esp+8, 4);
            if (rc == VINF_SUCCESS)
            {
                if (    (uEFlags & X86_EFL_VM)
                    ||  (selCS & X86_SEL_RPL) == 3)
                {
                    uint32_t selSS, esp;

                    rc |= PGMPhysSimpleReadGCPtr(pVM, &esp,     pCtx->esp + 12, 4);
                    rc |= PGMPhysSimpleReadGCPtr(pVM, &selSS,   pCtx->esp + 16, 4);

                    if (uEFlags & X86_EFL_VM)
                    {
                        uint32_t selDS, selES, selFS, selGS;
                        rc  = PGMPhysSimpleReadGCPtr(pVM, &selES,   pCtx->esp + 20, 4);
                        rc |= PGMPhysSimpleReadGCPtr(pVM, &selDS,   pCtx->esp + 24, 4);
                        rc |= PGMPhysSimpleReadGCPtr(pVM, &selFS,   pCtx->esp + 28, 4);
                        rc |= PGMPhysSimpleReadGCPtr(pVM, &selGS,   pCtx->esp + 32, 4);
                        if (rc == VINF_SUCCESS)
                        {
                            Log(("Patch code: IRET->VM stack frame: return address %04X:%08RX32 eflags=%08x ss:esp=%04X:%08RX32\n", selCS, eip, uEFlags, selSS, esp));
                            Log(("Patch code: IRET->VM stack frame: DS=%04X ES=%04X FS=%04X GS=%04X\n", selDS, selES, selFS, selGS));
                        }
                    }
                    else
                        Log(("Patch code: IRET stack frame: return address %04X:%08RX32 eflags=%08x ss:esp=%04X:%08RX32\n", selCS, eip, uEFlags, selSS, esp));
                }
                else
                    Log(("Patch code: IRET stack frame: return address %04X:%08RX32 eflags=%08x\n", selCS, eip, uEFlags));
            }
        }
#endif /* LOG_ENABLED */
        Log(("emR3PatchTrap: in patch: eip=%08x: trap=%02x err=%08x cr2=%08x cr0=%08x\n",
             pCtx->eip, u8TrapNo, uErrorCode, uCR2, (uint32_t)pCtx->cr0));

        RTGCPTR pNewEip;
        rc = PATMR3HandleTrap(pVM, pCtx, pCtx->eip, &pNewEip);
        switch (rc)
        {
            /*
             * Execute the faulting instruction.
             */
            case VINF_SUCCESS:
            {
                /** @todo execute a whole block */
                Log(("emR3PatchTrap: Executing faulting instruction at new address %RGv\n", pNewEip));
                if (!(pVM->em.s.pPatmGCState->uVMFlags & X86_EFL_IF))
                    Log(("emR3PatchTrap: Virtual IF flag disabled!!\n"));

                pCtx->eip = pNewEip;
                AssertRelease(pCtx->eip);

                if (pCtx->eflags.Bits.u1IF)
                {
                    /* Windows XP lets irets fault intentionally and then takes action based on the opcode; an
                     * int3 patch overwrites it and leads to blue screens. Remove the patch in this case.
                     */
                    if (    u8TrapNo == X86_XCPT_GP
                        &&  PATMIsInt3Patch(pVM, pCtx->eip, NULL, NULL))
                    {
                        /** @todo move to PATMR3HandleTrap */
                        Log(("Possible Windows XP iret fault at %08RX32\n", pCtx->eip));
                        PATMR3RemovePatch(pVM, pCtx->eip);
                    }

                    /** @todo Knoppix 5 regression when returning VINF_SUCCESS here and going back to raw mode. */
                    /* Note: possibly because a reschedule is required (e.g. iret to V86 code) */

                    return emR3RawExecuteInstruction(pVM, "PATCHIR");
                    /* Interrupts are enabled; just go back to the original instruction.
                    return VINF_SUCCESS; */
                }
                return VINF_EM_RESCHEDULE_REM;
            }

            /*
             * One instruction.
             */
            case VINF_PATCH_EMULATE_INSTR:
                Log(("emR3PatchTrap: Emulate patched instruction at %RGv IF=%d VMIF=%x\n",
                     pNewEip, pCtx->eflags.Bits.u1IF, pVM->em.s.pPatmGCState->uVMFlags));
                pCtx->eip = pNewEip;
                AssertRelease(pCtx->eip);
                return emR3RawExecuteInstruction(pVM, "PATCHEMUL: ");

            /*
             * The patch was disabled, hand it to the REM.
             */
            case VERR_PATCH_DISABLED:
                if (!(pVM->em.s.pPatmGCState->uVMFlags & X86_EFL_IF))
                    Log(("emR3PatchTrap: Virtual IF flag disabled!!\n"));
                pCtx->eip = pNewEip;
                AssertRelease(pCtx->eip);

                if (pCtx->eflags.Bits.u1IF)
                {
                    /*
                     * The last instruction in the patch block needs to be executed!! (sti/sysexit for example)
                     */
                    Log(("PATCH: IF=1 -> emulate last instruction as it can't be interrupted!!\n"));
                    return emR3RawExecuteInstruction(pVM, "PATCHIR");
                }
                return VINF_EM_RESCHEDULE_REM;

            /* Force continued patch exection; usually due to write monitored stack. */
            case VINF_PATCH_CONTINUE:
                return VINF_SUCCESS;

            /*
             * Anything else is *fatal*.
             */
            default:
                AssertReleaseMsgFailed(("Unknown return code %Rrc from PATMR3HandleTrap!\n", rc));
                return VERR_INTERNAL_ERROR;
        }
    }
    return VINF_SUCCESS;
}


/**
 * Handle a privileged instruction.
 *
 * @returns VBox status code suitable for EM.
 * @param   pVM     VM handle.
 */
int emR3RawPrivileged(PVM pVM)
{
    STAM_PROFILE_START(&pVM->em.s.StatPrivEmu, a);
    PCPUMCTX    pCtx = pVM->em.s.pCtx;

    Assert(!pCtx->eflags.Bits.u1VM);

    if (PATMIsEnabled(pVM))
    {
        /*
         * Check if in patch code.
         */
        if (PATMR3IsInsidePatchJump(pVM, pCtx->eip, NULL))
        {
#ifdef LOG_ENABLED
            DBGFR3InfoLog(pVM, "cpumguest", "PRIV");
#endif
            AssertMsgFailed(("FATAL ERROR: executing random instruction inside generated patch jump %08X\n", pCtx->eip));
            return VERR_EM_RAW_PATCH_CONFLICT;
        }
        if (   (pCtx->ss & X86_SEL_RPL) == 0
            && !pCtx->eflags.Bits.u1VM
            && !PATMIsPatchGCAddr(pVM, pCtx->eip))
        {
            int rc = PATMR3InstallPatch(pVM, SELMToFlat(pVM, DIS_SELREG_CS, CPUMCTX2CORE(pCtx), pCtx->eip),
                                        (SELMGetCpuModeFromSelector(pVM, pCtx->eflags, pCtx->cs, &pCtx->csHid) == CPUMODE_32BIT) ? PATMFL_CODE32 : 0);
            if (RT_SUCCESS(rc))
            {
#ifdef LOG_ENABLED
                DBGFR3InfoLog(pVM, "cpumguest", "PRIV");
#endif
                DBGFR3DisasInstrCurrentLog(pVM, "Patched privileged instruction");
                return VINF_SUCCESS;
            }
        }
    }

#ifdef LOG_ENABLED
    if (!PATMIsPatchGCAddr(pVM, pCtx->eip))
    {
        DBGFR3InfoLog(pVM, "cpumguest", "PRIV");
        DBGFR3DisasInstrCurrentLog(pVM, "Privileged instr: ");
    }
#endif

    /*
     * Instruction statistics and logging.
     */
    DISCPUSTATE Cpu;
    int         rc;

    rc = CPUMR3DisasmInstrCPU(pVM, pCtx, pCtx->rip, &Cpu, "PRIV: ");
    if (RT_SUCCESS(rc))
    {
#ifdef VBOX_WITH_STATISTICS
        PEMSTATS pStats = pVM->em.s.CTX_SUFF(pStats);
        switch (Cpu.pCurInstr->opcode)
        {
            case OP_INVLPG:
                STAM_COUNTER_INC(&pStats->StatInvlpg);
                break;
            case OP_IRET:
                STAM_COUNTER_INC(&pStats->StatIret);
                break;
            case OP_CLI:
                STAM_COUNTER_INC(&pStats->StatCli);
                emR3RecordCli(pVM, pCtx->rip);
                break;
            case OP_STI:
                STAM_COUNTER_INC(&pStats->StatSti);
                break;
            case OP_INSB:
            case OP_INSWD:
            case OP_IN:
            case OP_OUTSB:
            case OP_OUTSWD:
            case OP_OUT:
                AssertMsgFailed(("Unexpected privileged exception due to port IO\n"));
                break;

            case OP_MOV_CR:
                if (Cpu.param1.flags & USE_REG_GEN32)
                {
                    //read
                    Assert(Cpu.param2.flags & USE_REG_CR);
                    Assert(Cpu.param2.base.reg_ctrl <= USE_REG_CR4);
                    STAM_COUNTER_INC(&pStats->StatMovReadCR[Cpu.param2.base.reg_ctrl]);
                }
                else
                {
                    //write
                    Assert(Cpu.param1.flags & USE_REG_CR);
                    Assert(Cpu.param1.base.reg_ctrl <= USE_REG_CR4);
                    STAM_COUNTER_INC(&pStats->StatMovWriteCR[Cpu.param1.base.reg_ctrl]);
                }
                break;

            case OP_MOV_DR:
                STAM_COUNTER_INC(&pStats->StatMovDRx);
                break;
            case OP_LLDT:
                STAM_COUNTER_INC(&pStats->StatMovLldt);
                break;
            case OP_LIDT:
                STAM_COUNTER_INC(&pStats->StatMovLidt);
                break;
            case OP_LGDT:
                STAM_COUNTER_INC(&pStats->StatMovLgdt);
                break;
            case OP_SYSENTER:
                STAM_COUNTER_INC(&pStats->StatSysEnter);
                break;
            case OP_SYSEXIT:
                STAM_COUNTER_INC(&pStats->StatSysExit);
                break;
            case OP_SYSCALL:
                STAM_COUNTER_INC(&pStats->StatSysCall);
                break;
            case OP_SYSRET:
                STAM_COUNTER_INC(&pStats->StatSysRet);
                break;
            case OP_HLT:
                STAM_COUNTER_INC(&pStats->StatHlt);
                break;
            default:
                STAM_COUNTER_INC(&pStats->StatMisc);
                Log4(("emR3RawPrivileged: opcode=%d\n", Cpu.pCurInstr->opcode));
                break;
        }
#endif /* VBOX_WITH_STATISTICS */
        if (    (pCtx->ss & X86_SEL_RPL) == 0
            &&  !pCtx->eflags.Bits.u1VM
            &&  SELMGetCpuModeFromSelector(pVM, pCtx->eflags, pCtx->cs, &pCtx->csHid) == CPUMODE_32BIT)
        {
            uint32_t size;

            STAM_PROFILE_START(&pVM->em.s.StatPrivEmu, a);
            switch (Cpu.pCurInstr->opcode)
            {
                case OP_CLI:
                    pCtx->eflags.u32 &= ~X86_EFL_IF;
                    Assert(Cpu.opsize == 1);
                    pCtx->rip += Cpu.opsize;
                    STAM_PROFILE_STOP(&pVM->em.s.StatPrivEmu, a);
                    return VINF_EM_RESCHEDULE_REM; /* must go to the recompiler now! */

                case OP_STI:
                    pCtx->eflags.u32 |= X86_EFL_IF;
                    EMSetInhibitInterruptsPC(pVM, pCtx->rip + Cpu.opsize);
                    Assert(Cpu.opsize == 1);
                    pCtx->rip += Cpu.opsize;
                    STAM_PROFILE_STOP(&pVM->em.s.StatPrivEmu, a);
                    return VINF_SUCCESS;

                case OP_HLT:
                    if (PATMIsPatchGCAddr(pVM, (RTGCPTR)pCtx->eip))
                    {
                        PATMTRANSSTATE  enmState;
                        RTGCPTR         pOrgInstrGC = PATMR3PatchToGCPtr(pVM, pCtx->eip, &enmState);

                        if (enmState == PATMTRANS_OVERWRITTEN)
                        {
                            rc = PATMR3DetectConflict(pVM, pOrgInstrGC, pOrgInstrGC);
                            Assert(rc == VERR_PATCH_DISABLED);
                            /* Conflict detected, patch disabled */
                            Log(("emR3RawPrivileged: detected conflict -> disabled patch at %08RX32\n", pCtx->eip));

                            enmState = PATMTRANS_SAFE;
                        }

                        /* The translation had better be successful. Otherwise we can't recover. */
                        AssertReleaseMsg(pOrgInstrGC && enmState != PATMTRANS_OVERWRITTEN, ("Unable to translate instruction address at %08RX32\n", pCtx->eip));
                        if (enmState != PATMTRANS_OVERWRITTEN)
                            pCtx->eip = pOrgInstrGC;
                    }
                    /* no break; we could just return VINF_EM_HALT here */

                case OP_MOV_CR:
                case OP_MOV_DR:
#ifdef LOG_ENABLED
                    if (PATMIsPatchGCAddr(pVM, pCtx->eip))
                    {
                        DBGFR3InfoLog(pVM, "cpumguest", "PRIV");
                        DBGFR3DisasInstrCurrentLog(pVM, "Privileged instr: ");
                    }
#endif

                    rc = EMInterpretInstructionCPU(pVM, &Cpu, CPUMCTX2CORE(pCtx), 0, &size);
                    if (RT_SUCCESS(rc))
                    {
                        pCtx->rip += Cpu.opsize;
                        STAM_PROFILE_STOP(&pVM->em.s.StatPrivEmu, a);

                        if (    Cpu.pCurInstr->opcode == OP_MOV_CR
                            &&  Cpu.param1.flags == USE_REG_CR /* write */
                           )
                        {
                            /* Deal with CR0 updates inside patch code that force
                             * us to go to the recompiler.
                             */
                            if (   PATMIsPatchGCAddr(pVM, pCtx->rip)
                                && (pCtx->cr0 & (X86_CR0_WP|X86_CR0_PG|X86_CR0_PE)) != (X86_CR0_WP|X86_CR0_PG|X86_CR0_PE))
                            {
                                PATMTRANSSTATE  enmState;
                                RTGCPTR         pOrgInstrGC = PATMR3PatchToGCPtr(pVM, pCtx->rip, &enmState);

                                Assert(pCtx->eflags.Bits.u1IF == 0);
                                Log(("Force recompiler switch due to cr0 (%RGp) update\n", pCtx->cr0));
                                if (enmState == PATMTRANS_OVERWRITTEN)
                                {
                                    rc = PATMR3DetectConflict(pVM, pOrgInstrGC, pOrgInstrGC);
                                    Assert(rc == VERR_PATCH_DISABLED);
                                    /* Conflict detected, patch disabled */
                                    Log(("emR3RawPrivileged: detected conflict -> disabled patch at %RGv\n", (RTGCPTR)pCtx->rip));
                                    enmState = PATMTRANS_SAFE;
                                }
                                /* The translation had better be successful. Otherwise we can't recover. */
                                AssertReleaseMsg(pOrgInstrGC && enmState != PATMTRANS_OVERWRITTEN, ("Unable to translate instruction address at %RGv\n", (RTGCPTR)pCtx->rip));
                                if (enmState != PATMTRANS_OVERWRITTEN)
                                    pCtx->rip = pOrgInstrGC;
                            }

                            /* Reschedule is necessary as the execution/paging mode might have changed. */
                            return VINF_EM_RESCHEDULE;
                        }
                        return rc; /* can return VINF_EM_HALT as well. */
                    }
                    AssertMsgReturn(rc == VERR_EM_INTERPRETER, ("%Rrc\n", rc), rc);
                    break; /* fall back to the recompiler */
            }
            STAM_PROFILE_STOP(&pVM->em.s.StatPrivEmu, a);
        }
    }

    if (PATMIsPatchGCAddr(pVM, pCtx->eip))
        return emR3PatchTrap(pVM, pCtx, VINF_PATM_PATCH_TRAP_GP);

    return emR3RawExecuteInstruction(pVM, "PRIV");
}


/**
 * Update the forced rawmode execution modifier.
 *
 * This function is called when we're returning from the raw-mode loop(s). If we're
 * in patch code, it will set a flag forcing execution to be resumed in raw-mode,
 * if not in patch code, the flag will be cleared.
 *
 * We should never interrupt patch code while it's being executed. Cli patches can
 * contain big code blocks, but they are always executed with IF=0. Other patches
 * replace single instructions and should be atomic.
 *
 * @returns Updated rc.
 *
 * @param   pVM     The VM handle.
 * @param   pCtx    The guest CPU context.
 * @param   rc      The result code.
 */
DECLINLINE(int) emR3RawUpdateForceFlag(PVM pVM, PCPUMCTX pCtx, int rc)
{
    if (PATMIsPatchGCAddr(pVM, pCtx->eip)) /** @todo check cs selector base/type */
    {
        /* ignore reschedule attempts. */
        switch (rc)
        {
            case VINF_EM_RESCHEDULE:
            case VINF_EM_RESCHEDULE_REM:
                rc = VINF_SUCCESS;
                break;
        }
        pVM->em.s.fForceRAW = true;
    }
    else
        pVM->em.s.fForceRAW = false;
    return rc;
}


/**
 * Process a subset of the raw-mode return code.
 *
 * Since we have to share this with raw-mode single stepping, this inline
 * function has been created to avoid code duplication.
 *
 * @returns VINF_SUCCESS if it's ok to continue raw mode.
 * @returns VBox status code to return to the EM main loop.
 *
 * @param   pVM     The VM handle
 * @param   rc      The return code.
 * @param   pCtx    The guest cpu context.
 */
DECLINLINE(int) emR3RawHandleRC(PVM pVM, PCPUMCTX pCtx, int rc)
{
    switch (rc)
    {
        /*
         * Common & simple ones.
         */
        case VINF_SUCCESS:
            break;
        case VINF_EM_RESCHEDULE_RAW:
        case VINF_EM_RESCHEDULE_HWACC:
        case VINF_EM_RAW_INTERRUPT:
        case VINF_EM_RAW_TO_R3:
        case VINF_EM_RAW_TIMER_PENDING:
        case VINF_EM_PENDING_REQUEST:
            rc = VINF_SUCCESS;
            break;

        /*
         * Privileged instruction.
         */
        case VINF_EM_RAW_EXCEPTION_PRIVILEGED:
        case VINF_PATM_PATCH_TRAP_GP:
            rc = emR3RawPrivileged(pVM);
            break;

        /*
         * Got a trap which needs dispatching.
         */
        case VINF_EM_RAW_GUEST_TRAP:
            if (PATMR3IsInsidePatchJump(pVM, pCtx->eip, NULL))
            {
                AssertReleaseMsgFailed(("FATAL ERROR: executing random instruction inside generated patch jump %08X\n", CPUMGetGuestEIP(pVM)));
                rc = VERR_EM_RAW_PATCH_CONFLICT;
                break;
            }
            rc = emR3RawGuestTrap(pVM);
            break;

        /*
         * Trap in patch code.
         */
        case VINF_PATM_PATCH_TRAP_PF:
        case VINF_PATM_PATCH_INT3:
            rc = emR3PatchTrap(pVM, pCtx, rc);
            break;

        case VINF_PATM_DUPLICATE_FUNCTION:
            Assert(PATMIsPatchGCAddr(pVM, (RTGCPTR)pCtx->eip));
            rc = PATMR3DuplicateFunctionRequest(pVM, pCtx);
            AssertRC(rc);
            rc = VINF_SUCCESS;
            break;

        case VINF_PATM_CHECK_PATCH_PAGE:
            rc = PATMR3HandleMonitoredPage(pVM);
            AssertRC(rc);
            rc = VINF_SUCCESS;
            break;

        /*
         * Patch manager.
         */
        case VERR_EM_RAW_PATCH_CONFLICT:
            AssertReleaseMsgFailed(("%Rrc handling is not yet implemented\n", rc));
            break;

#ifdef VBOX_WITH_VMI
        /*
         * PARAV function.
         */
        case VINF_EM_RESCHEDULE_PARAV:
            rc = PARAVCallFunction(pVM);
            break;
#endif

        /*
         * Memory mapped I/O access - attempt to patch the instruction
         */
        case VINF_PATM_HC_MMIO_PATCH_READ:
            rc = PATMR3InstallPatch(pVM, SELMToFlat(pVM, DIS_SELREG_CS, CPUMCTX2CORE(pCtx), pCtx->eip),
                                    PATMFL_MMIO_ACCESS | ((SELMGetCpuModeFromSelector(pVM, pCtx->eflags, pCtx->cs, &pCtx->csHid) == CPUMODE_32BIT) ? PATMFL_CODE32 : 0));
            if (RT_FAILURE(rc))
                rc = emR3RawExecuteInstruction(pVM, "MMIO");
            break;

        case VINF_PATM_HC_MMIO_PATCH_WRITE:
            AssertFailed(); /* not yet implemented. */
            rc = emR3RawExecuteInstruction(pVM, "MMIO");
            break;

        /*
         * Conflict or out of page tables.
         *
         * VM_FF_PGM_SYNC_CR3 is set by the hypervisor and all we need to
         * do here is to execute the pending forced actions.
         */
        case VINF_PGM_SYNC_CR3:
            AssertMsg(VM_FF_ISPENDING(pVM, VM_FF_PGM_SYNC_CR3 | VM_FF_PGM_SYNC_CR3_NON_GLOBAL),
                      ("VINF_PGM_SYNC_CR3 and no VM_FF_PGM_SYNC_CR3*!\n"));
            rc = VINF_SUCCESS;
            break;

        /*
         * Paging mode change.
         */
        case VINF_PGM_CHANGE_MODE:
            rc = PGMChangeMode(pVM, pCtx->cr0, pCtx->cr4, pCtx->msrEFER);
            if (RT_SUCCESS(rc))
                rc = VINF_EM_RESCHEDULE;
            break;

        /*
         * CSAM wants to perform a task in ring-3. It has set an FF action flag.
         */
        case VINF_CSAM_PENDING_ACTION:
            rc = VINF_SUCCESS;
            break;

        /*
         * Invoked Interrupt gate - must directly (!) go to the recompiler.
         */
        case VINF_EM_RAW_INTERRUPT_PENDING:
        case VINF_EM_RAW_RING_SWITCH_INT:
            Assert(TRPMHasTrap(pVM));
            Assert(!PATMIsPatchGCAddr(pVM, (RTGCPTR)pCtx->eip));

            if (TRPMHasTrap(pVM))
            {
                /* If the guest gate is marked unpatched, then we will check again if we can patch it. */
                uint8_t u8Interrupt = TRPMGetTrapNo(pVM);
                if (TRPMR3GetGuestTrapHandler(pVM, u8Interrupt) == TRPM_INVALID_HANDLER)
                {
                    CSAMR3CheckGates(pVM, u8Interrupt, 1);
                    Log(("emR3RawHandleRC: recheck gate %x -> valid=%d\n", u8Interrupt, TRPMR3GetGuestTrapHandler(pVM, u8Interrupt) != TRPM_INVALID_HANDLER));
                    /* Note: If it was successful, then we could go back to raw mode, but let's keep things simple for now. */
                }
            }
            rc = VINF_EM_RESCHEDULE_REM;
            break;

        /*
         * Other ring switch types.
         */
        case VINF_EM_RAW_RING_SWITCH:
            rc = emR3RawRingSwitch(pVM);
            break;

        /*
         * REMGCNotifyInvalidatePage() failed because of overflow.
         */
        case VERR_REM_FLUSHED_PAGES_OVERFLOW:
            Assert((pCtx->ss & X86_SEL_RPL) != 1);
            REMR3ReplayInvalidatedPages(pVM);
            rc = VINF_SUCCESS;
            break;

        /*
         * I/O Port access - emulate the instruction.
         */
        case VINF_IOM_HC_IOPORT_READ:
        case VINF_IOM_HC_IOPORT_WRITE:
            rc = emR3RawExecuteIOInstruction(pVM);
            break;

        /*
         * Memory mapped I/O access - emulate the instruction.
         */
        case VINF_IOM_HC_MMIO_READ:
        case VINF_IOM_HC_MMIO_WRITE:
        case VINF_IOM_HC_MMIO_READ_WRITE:
            rc = emR3RawExecuteInstruction(pVM, "MMIO");
            break;

        /*
         * Execute instruction.
         */
        case VINF_EM_RAW_EMULATE_INSTR_LDT_FAULT:
            rc = emR3RawExecuteInstruction(pVM, "LDT FAULT: ");
            break;
        case VINF_EM_RAW_EMULATE_INSTR_GDT_FAULT:
            rc = emR3RawExecuteInstruction(pVM, "GDT FAULT: ");
            break;
        case VINF_EM_RAW_EMULATE_INSTR_IDT_FAULT:
            rc = emR3RawExecuteInstruction(pVM, "IDT FAULT: ");
            break;
        case VINF_EM_RAW_EMULATE_INSTR_TSS_FAULT:
            rc = emR3RawExecuteInstruction(pVM, "TSS FAULT: ");
            break;
        case VINF_EM_RAW_EMULATE_INSTR_PD_FAULT:
            rc = emR3RawExecuteInstruction(pVM, "PD FAULT: ");
            break;

        case VINF_EM_RAW_EMULATE_INSTR_HLT:
            /** @todo skip instruction and go directly to the halt state. (see REM for implementation details) */
            rc = emR3RawPrivileged(pVM);
            break;

        case VINF_PATM_PENDING_IRQ_AFTER_IRET:
            rc = emR3RawExecuteInstruction(pVM, "EMUL: ", VINF_PATM_PENDING_IRQ_AFTER_IRET);
            break;

        case VINF_EM_RAW_EMULATE_INSTR:
        case VINF_PATCH_EMULATE_INSTR:
            rc = emR3RawExecuteInstruction(pVM, "EMUL: ");
            break;

        /*
         * Stale selector and iret traps => REM.
         */
        case VINF_EM_RAW_STALE_SELECTOR:
        case VINF_EM_RAW_IRET_TRAP:
            /* We will not go to the recompiler if EIP points to patch code. */
            if (PATMIsPatchGCAddr(pVM, pCtx->eip))
            {
                pCtx->eip = PATMR3PatchToGCPtr(pVM, (RTGCPTR)pCtx->eip, 0);
            }
            LogFlow(("emR3RawHandleRC: %Rrc -> %Rrc\n", rc, VINF_EM_RESCHEDULE_REM));
            rc = VINF_EM_RESCHEDULE_REM;
            break;

        /*
         * Up a level.
         */
        case VINF_EM_TERMINATE:
        case VINF_EM_OFF:
        case VINF_EM_RESET:
        case VINF_EM_SUSPEND:
        case VINF_EM_HALT:
        case VINF_EM_RESUME:
        case VINF_EM_RESCHEDULE:
        case VINF_EM_RESCHEDULE_REM:
            break;

        /*
         * Up a level and invoke the debugger.
         */
        case VINF_EM_DBG_STEPPED:
        case VINF_EM_DBG_BREAKPOINT:
        case VINF_EM_DBG_STEP:
        case VINF_EM_DBG_HYPER_BREAKPOINT:
        case VINF_EM_DBG_HYPER_STEPPED:
        case VINF_EM_DBG_HYPER_ASSERTION:
        case VINF_EM_DBG_STOP:
            break;

        /*
         * Up a level, dump and debug.
         */
        case VERR_TRPM_DONT_PANIC:
        case VERR_TRPM_PANIC:
        case VERR_VMM_RING0_ASSERTION:
            break;

        /*
         * Up a level, after HwAccM have done some release logging.
         */
        case VERR_VMX_INVALID_VMCS_FIELD:
        case VERR_VMX_INVALID_VMCS_PTR:
        case VERR_VMX_INVALID_VMXON_PTR:
        case VERR_VMX_UNEXPECTED_INTERRUPTION_EXIT_CODE:
        case VERR_VMX_UNEXPECTED_EXCEPTION:
        case VERR_VMX_UNEXPECTED_EXIT_CODE:
        case VERR_VMX_INVALID_GUEST_STATE:
        case VERR_VMX_UNABLE_TO_START_VM:
        case VERR_VMX_UNABLE_TO_RESUME_VM:
            HWACCMR3CheckError(pVM, rc);
            break;
        /*
         * Anything which is not known to us means an internal error
         * and the termination of the VM!
         */
        default:
            AssertMsgFailed(("Unknown GC return code: %Rra\n", rc));
            break;
    }
    return rc;
}


/**
 * Check for pending raw actions
 *
 * @returns VBox status code.
 * @param   pVM         The VM to operate on.
 */
VMMR3DECL(int) EMR3CheckRawForcedActions(PVM pVM)
{
    return emR3RawForcedActions(pVM, pVM->em.s.pCtx);
}


/**
 * Process raw-mode specific forced actions.
 *
 * This function is called when any FFs in the VM_FF_HIGH_PRIORITY_PRE_RAW_MASK is pending.
 *
 * @returns VBox status code.
 *          Only the normal success/failure stuff, no VINF_EM_*.
 * @param   pVM         The VM handle.
 * @param   pCtx        The guest CPUM register context.
 */
static int emR3RawForcedActions(PVM pVM, PCPUMCTX pCtx)
{
   /*
    * Note that the order is *vitally* important!
    * Also note that SELMR3UpdateFromCPUM may trigger VM_FF_SELM_SYNC_TSS.
    */


    /*
     * Sync selector tables.
     */
    if (VM_FF_ISPENDING(pVM, VM_FF_SELM_SYNC_GDT | VM_FF_SELM_SYNC_LDT))
    {
        int rc = SELMR3UpdateFromCPUM(pVM);
        if (RT_FAILURE(rc))
            return rc;
    }

    /*
     * Sync IDT.
     */
    if (VM_FF_ISSET(pVM, VM_FF_TRPM_SYNC_IDT))
    {
        int rc = TRPMR3SyncIDT(pVM);
        if (RT_FAILURE(rc))
            return rc;
    }

    /*
     * Sync TSS.
     */
    if (VM_FF_ISSET(pVM, VM_FF_SELM_SYNC_TSS))
    {
        int rc = SELMR3SyncTSS(pVM);
        if (RT_FAILURE(rc))
            return rc;
    }

    /*
     * Sync page directory.
     */
    if (VM_FF_ISPENDING(pVM, VM_FF_PGM_SYNC_CR3 | VM_FF_PGM_SYNC_CR3_NON_GLOBAL))
    {
        int rc = PGMSyncCR3(pVM, pCtx->cr0, pCtx->cr3, pCtx->cr4, VM_FF_ISSET(pVM, VM_FF_PGM_SYNC_CR3));
        if (RT_FAILURE(rc))
            return rc;

        Assert(!VM_FF_ISPENDING(pVM, VM_FF_SELM_SYNC_GDT | VM_FF_SELM_SYNC_LDT));

        /* Prefetch pages for EIP and ESP */
        /** @todo This is rather expensive. Should investigate if it really helps at all. */
        rc = PGMPrefetchPage(pVM, SELMToFlat(pVM, DIS_SELREG_CS, CPUMCTX2CORE(pCtx), pCtx->rip));
        if (rc == VINF_SUCCESS)
            rc = PGMPrefetchPage(pVM, SELMToFlat(pVM, DIS_SELREG_SS, CPUMCTX2CORE(pCtx), pCtx->rsp));
        if (rc != VINF_SUCCESS)
        {
            if (rc != VINF_PGM_SYNC_CR3)
                return rc;
            rc = PGMSyncCR3(pVM, pCtx->cr0, pCtx->cr3, pCtx->cr4, VM_FF_ISSET(pVM, VM_FF_PGM_SYNC_CR3));
            if (RT_FAILURE(rc))
                return rc;
        }
        /** @todo maybe prefetch the supervisor stack page as well */
    }

    /*
     * Allocate handy pages (just in case the above actions have consumed some pages).
     */
    if (VM_FF_ISSET(pVM, VM_FF_PGM_NEED_HANDY_PAGES))
    {
        int rc = PGMR3PhysAllocateHandyPages(pVM);
        if (RT_FAILURE(rc))
            return rc;
    }

    return VINF_SUCCESS;
}


/**
 * Executes raw code.
 *
 * This function contains the raw-mode version of the inner
 * execution loop (the outer loop being in EMR3ExecuteVM()).
 *
 * @returns VBox status code. The most important ones are: VINF_EM_RESCHEDULE,
 *          VINF_EM_RESCHEDULE_REM, VINF_EM_SUSPEND, VINF_EM_RESET and VINF_EM_TERMINATE.
 *
 * @param   pVM         VM handle.
 * @param   pfFFDone    Where to store an indicator telling whether or not
 *                      FFs were done before returning.
 */
static int emR3RawExecute(PVM pVM, bool *pfFFDone)
{
    STAM_REL_PROFILE_ADV_START(&pVM->em.s.StatRAWTotal, a);

    int      rc = VERR_INTERNAL_ERROR;
    PCPUMCTX pCtx = pVM->em.s.pCtx;
    LogFlow(("emR3RawExecute: (cs:eip=%04x:%08x)\n", pCtx->cs, pCtx->eip));
    pVM->em.s.fForceRAW = false;
    *pfFFDone = false;


    /*
     *
     * Spin till we get a forced action or raw mode status code resulting in
     * in anything but VINF_SUCCESS or VINF_EM_RESCHEDULE_RAW.
     *
     */
    for (;;)
    {
        STAM_PROFILE_ADV_START(&pVM->em.s.StatRAWEntry, b);

        /*
         * Check various preconditions.
         */
#ifdef VBOX_STRICT
        Assert(REMR3QueryPendingInterrupt(pVM) == REM_NO_PENDING_IRQ);
        Assert(pCtx->eflags.Bits.u1VM || (pCtx->ss & X86_SEL_RPL) == 3 || (pCtx->ss & X86_SEL_RPL) == 0);
        AssertMsg(   (pCtx->eflags.u32 & X86_EFL_IF)
                  || PATMShouldUseRawMode(pVM, (RTGCPTR)pCtx->eip),
                  ("Tried to execute code with IF at EIP=%08x!\n", pCtx->eip));
        if (    !VM_FF_ISPENDING(pVM, VM_FF_PGM_SYNC_CR3 | VM_FF_PGM_SYNC_CR3_NON_GLOBAL)
            &&  PGMR3MapHasConflicts(pVM, pCtx->cr3, pVM->fRawR0Enabled))
        {
            AssertMsgFailed(("We should not get conflicts any longer!!!\n"));
            return VERR_INTERNAL_ERROR;
        }
#endif /* VBOX_STRICT */

        /*
         * Process high priority pre-execution raw-mode FFs.
         */
        if (VM_FF_ISPENDING(pVM, VM_FF_HIGH_PRIORITY_PRE_RAW_MASK))
        {
            rc = emR3RawForcedActions(pVM, pCtx);
            if (RT_FAILURE(rc))
                break;
        }

        /*
         * If we're going to execute ring-0 code, the guest state needs to
         * be modified a bit and some of the state components (IF, SS/CS RPL,
         * and perhaps EIP) needs to be stored with PATM.
         */
        rc = CPUMRawEnter(pVM, NULL);
        if (rc != VINF_SUCCESS)
        {
            STAM_PROFILE_ADV_STOP(&pVM->em.s.StatRAWEntry, b);
            break;
        }

        /*
         * Scan code before executing it. Don't bother with user mode or V86 code
         */
        if (    (pCtx->ss & X86_SEL_RPL) <= 1
            &&  !pCtx->eflags.Bits.u1VM
            && !PATMIsPatchGCAddr(pVM, pCtx->eip))
        {
            STAM_PROFILE_ADV_SUSPEND(&pVM->em.s.StatRAWEntry, b);
            CSAMR3CheckCodeEx(pVM, CPUMCTX2CORE(pCtx), pCtx->eip);
            STAM_PROFILE_ADV_RESUME(&pVM->em.s.StatRAWEntry, b);
        }

#ifdef LOG_ENABLED
        /*
         * Log important stuff before entering GC.
         */
        PPATMGCSTATE pGCState = PATMR3QueryGCStateHC(pVM);
        if (pCtx->eflags.Bits.u1VM)
            Log(("RV86: %04X:%08X IF=%d VMFlags=%x\n", pCtx->cs, pCtx->eip, pCtx->eflags.Bits.u1IF, pGCState->uVMFlags));
        else if ((pCtx->ss & X86_SEL_RPL) == 1)
        {
            bool fCSAMScanned = CSAMIsPageScanned(pVM, (RTGCPTR)pCtx->eip);
            Log(("RR0: %08X ESP=%08X IF=%d VMFlags=%x PIF=%d CPL=%d (Scanned=%d)\n", pCtx->eip, pCtx->esp, pCtx->eflags.Bits.u1IF, pGCState->uVMFlags, pGCState->fPIF, (pCtx->ss & X86_SEL_RPL), fCSAMScanned));
        }
        else if ((pCtx->ss & X86_SEL_RPL) == 3)
            Log(("RR3: %08X ESP=%08X IF=%d VMFlags=%x\n", pCtx->eip, pCtx->esp, pCtx->eflags.Bits.u1IF, pGCState->uVMFlags));
#endif /* LOG_ENABLED */



        /*
         * Execute the code.
         */
        STAM_PROFILE_ADV_STOP(&pVM->em.s.StatRAWEntry, b);
        STAM_PROFILE_START(&pVM->em.s.StatRAWExec, c);
        VMMR3Unlock(pVM);
        rc = VMMR3RawRunGC(pVM);
        VMMR3Lock(pVM);
        STAM_PROFILE_STOP(&pVM->em.s.StatRAWExec, c);
        STAM_PROFILE_ADV_START(&pVM->em.s.StatRAWTail, d);

        LogFlow(("RR0-E: %08X ESP=%08X IF=%d VMFlags=%x PIF=%d CPL=%d\n", pCtx->eip, pCtx->esp, pCtx->eflags.Bits.u1IF, pGCState->uVMFlags, pGCState->fPIF, (pCtx->ss & X86_SEL_RPL)));
        LogFlow(("VMMR3RawRunGC returned %Rrc\n", rc));



        /*
         * Restore the real CPU state and deal with high priority post
         * execution FFs before doing anything else.
         */
        rc = CPUMRawLeave(pVM, NULL, rc);
        VM_FF_CLEAR(pVM, VM_FF_RESUME_GUEST_MASK);
        if (VM_FF_ISPENDING(pVM, VM_FF_HIGH_PRIORITY_POST_MASK))
            rc = emR3HighPriorityPostForcedActions(pVM, rc);

#ifdef VBOX_STRICT
        /*
         * Assert TSS consistency & rc vs patch code.
         */
        if (   !VM_FF_ISPENDING(pVM, VM_FF_SELM_SYNC_TSS | VM_FF_SELM_SYNC_GDT) /* GDT implies TSS at the moment. */
            &&  EMIsRawRing0Enabled(pVM))
            SELMR3CheckTSS(pVM);
        switch (rc)
        {
            case VINF_SUCCESS:
            case VINF_EM_RAW_INTERRUPT:
            case VINF_PATM_PATCH_TRAP_PF:
            case VINF_PATM_PATCH_TRAP_GP:
            case VINF_PATM_PATCH_INT3:
            case VINF_PATM_CHECK_PATCH_PAGE:
            case VINF_EM_RAW_EXCEPTION_PRIVILEGED:
            case VINF_EM_RAW_GUEST_TRAP:
            case VINF_EM_RESCHEDULE_RAW:
                break;

            default:
                if (PATMIsPatchGCAddr(pVM, pCtx->eip) && !(pCtx->eflags.u32 & X86_EFL_TF))
                    LogIt(NULL, 0, LOG_GROUP_PATM, ("Patch code interrupted at %RRv for reason %Rrc\n", (RTRCPTR)CPUMGetGuestEIP(pVM), rc));
                break;
        }
        /*
         * Let's go paranoid!
         */
        if (    !VM_FF_ISPENDING(pVM, VM_FF_PGM_SYNC_CR3 | VM_FF_PGM_SYNC_CR3_NON_GLOBAL)
            &&  PGMR3MapHasConflicts(pVM, pCtx->cr3, pVM->fRawR0Enabled))
        {
            AssertMsgFailed(("We should not get conflicts any longer!!!\n"));
            return VERR_INTERNAL_ERROR;
        }
#endif /* VBOX_STRICT */

        /*
         * Process the returned status code.
         */
        if (rc >= VINF_EM_FIRST && rc <= VINF_EM_LAST)
        {
            STAM_PROFILE_ADV_STOP(&pVM->em.s.StatRAWTail, d);
            break;
        }
        rc = emR3RawHandleRC(pVM, pCtx, rc);
        if (rc != VINF_SUCCESS)
        {
            rc = emR3RawUpdateForceFlag(pVM, pCtx, rc);
            if (rc != VINF_SUCCESS)
            {
                STAM_PROFILE_ADV_STOP(&pVM->em.s.StatRAWTail, d);
                break;
            }
        }

        /*
         * Check and execute forced actions.
         */
#ifdef VBOX_HIGH_RES_TIMERS_HACK
        TMTimerPoll(pVM);
#endif
        STAM_PROFILE_ADV_STOP(&pVM->em.s.StatRAWTail, d);
        if (VM_FF_ISPENDING(pVM, ~VM_FF_HIGH_PRIORITY_PRE_RAW_MASK))
        {
            Assert(pCtx->eflags.Bits.u1VM || (pCtx->ss & X86_SEL_RPL) != 1);

            STAM_REL_PROFILE_ADV_SUSPEND(&pVM->em.s.StatRAWTotal, a);
            rc = emR3ForcedActions(pVM, rc);
            STAM_REL_PROFILE_ADV_RESUME(&pVM->em.s.StatRAWTotal, a);
            if (    rc != VINF_SUCCESS
                &&  rc != VINF_EM_RESCHEDULE_RAW)
            {
                rc = emR3RawUpdateForceFlag(pVM, pCtx, rc);
                if (rc != VINF_SUCCESS)
                {
                    *pfFFDone = true;
                    break;
                }
            }
        }
    }

    /*
     * Return to outer loop.
     */
#if defined(LOG_ENABLED) && defined(DEBUG)
    RTLogFlush(NULL);
#endif
    STAM_REL_PROFILE_ADV_STOP(&pVM->em.s.StatRAWTotal, a);
    return rc;
}


/**
 * Executes hardware accelerated raw code. (Intel VMX & AMD SVM)
 *
 * This function contains the raw-mode version of the inner
 * execution loop (the outer loop being in EMR3ExecuteVM()).
 *
 * @returns VBox status code. The most important ones are: VINF_EM_RESCHEDULE, VINF_EM_RESCHEDULE_RAW,
 *          VINF_EM_RESCHEDULE_REM, VINF_EM_SUSPEND, VINF_EM_RESET and VINF_EM_TERMINATE.
 *
 * @param   pVM         VM handle.
 * @param   idCpu       VMCPU id.
 * @param   pfFFDone    Where to store an indicator telling whether or not
 *                      FFs were done before returning.
 */
static int emR3HwAccExecute(PVM pVM, RTCPUID idCpu, bool *pfFFDone)
{
    int      rc = VERR_INTERNAL_ERROR;
    PCPUMCTX pCtx = pVM->em.s.pCtx;

    LogFlow(("emR3HwAccExecute%d: (cs:eip=%04x:%RGv)\n", idCpu, pCtx->cs, (RTGCPTR)pCtx->rip));
    *pfFFDone = false;

    STAM_COUNTER_INC(&pVM->em.s.StatHwAccExecuteEntry);

    /*
     * Spin till we get a forced action which returns anything but VINF_SUCCESS.
     */
    for (;;)
    {
        STAM_PROFILE_ADV_START(&pVM->em.s.StatHwAccEntry, a);

        /*
         * Check various preconditions.
         */
        VM_FF_CLEAR(pVM, (VM_FF_SELM_SYNC_GDT | VM_FF_SELM_SYNC_LDT | VM_FF_TRPM_SYNC_IDT | VM_FF_SELM_SYNC_TSS));

        /*
         * Process high priority pre-execution raw-mode FFs.
         */
        if (VM_FF_ISPENDING(pVM, VM_FF_HIGH_PRIORITY_PRE_RAW_MASK))
        {
            rc = emR3RawForcedActions(pVM, pCtx);
            if (RT_FAILURE(rc))
                break;
        }

#ifdef LOG_ENABLED
        /*
         * Log important stuff before entering GC.
         */
        if (TRPMHasTrap(pVM))
            Log(("Pending hardware interrupt=0x%x cs:rip=%04X:%RGv\n", TRPMGetTrapNo(pVM), pCtx->cs, (RTGCPTR)pCtx->rip));

        uint32_t cpl = CPUMGetGuestCPL(pVM, CPUMCTX2CORE(pCtx));
        if (pCtx->eflags.Bits.u1VM)
            Log(("HWV86: %08X IF=%d\n", pCtx->eip, pCtx->eflags.Bits.u1IF));
        else if (CPUMIsGuestIn64BitCode(pVM, CPUMCTX2CORE(pCtx)))
            Log(("HWR%d: %04X:%RGv ESP=%RGv IF=%d CR0=%x CR4=%x EFER=%x\n", cpl, pCtx->cs, (RTGCPTR)pCtx->rip, pCtx->rsp, pCtx->eflags.Bits.u1IF, (uint32_t)pCtx->cr0, (uint32_t)pCtx->cr4, (uint32_t)pCtx->msrEFER));
        else
            Log(("HWR%d: %04X:%08X ESP=%08X IF=%d CR0=%x CR4=%x EFER=%x\n", cpl, pCtx->cs,          pCtx->eip, pCtx->esp, pCtx->eflags.Bits.u1IF, (uint32_t)pCtx->cr0, (uint32_t)pCtx->cr4, (uint32_t)pCtx->msrEFER));
#endif /* LOG_ENABLED */

        /*
         * Execute the code.
         */
        STAM_PROFILE_ADV_STOP(&pVM->em.s.StatHwAccEntry, a);
        STAM_PROFILE_START(&pVM->em.s.StatHwAccExec, x);
        VMMR3Unlock(pVM);
        rc = VMMR3HwAccRunGC(pVM, idCpu);
        VMMR3Lock(pVM);
        STAM_PROFILE_STOP(&pVM->em.s.StatHwAccExec, x);

        /*
         * Deal with high priority post execution FFs before doing anything else.
         */
        VM_FF_CLEAR(pVM, VM_FF_RESUME_GUEST_MASK);
        if (VM_FF_ISPENDING(pVM, VM_FF_HIGH_PRIORITY_POST_MASK))
            rc = emR3HighPriorityPostForcedActions(pVM, rc);

        /*
         * Process the returned status code.
         */
        if (rc >= VINF_EM_FIRST && rc <= VINF_EM_LAST)
            break;

        rc = emR3RawHandleRC(pVM, pCtx, rc);
        if (rc != VINF_SUCCESS)
            break;

        /*
         * Check and execute forced actions.
         */
#ifdef VBOX_HIGH_RES_TIMERS_HACK
        TMTimerPoll(pVM);
#endif
        if (VM_FF_ISPENDING(pVM, VM_FF_ALL_MASK))
        {
            rc = emR3ForcedActions(pVM, rc);
            if (    rc != VINF_SUCCESS
                &&  rc != VINF_EM_RESCHEDULE_HWACC)
            {
                *pfFFDone = true;
                break;
            }
        }
    }
    /*
     * Return to outer loop.
     */
#if defined(LOG_ENABLED) && defined(DEBUG)
    RTLogFlush(NULL);
#endif
    return rc;
}


/**
 * Decides whether to execute RAW, HWACC or REM.
 *
 * @returns new EM state
 * @param   pVM     The VM.
 * @param   pCtx    The CPU context.
 */
DECLINLINE(EMSTATE) emR3Reschedule(PVM pVM, PCPUMCTX pCtx)
{
    /*
     * When forcing raw-mode execution, things are simple.
     */
    if (pVM->em.s.fForceRAW)
        return EMSTATE_RAW;

    /* !!! THIS MUST BE IN SYNC WITH remR3CanExecuteRaw !!! */
    /* !!! THIS MUST BE IN SYNC WITH remR3CanExecuteRaw !!! */
    /* !!! THIS MUST BE IN SYNC WITH remR3CanExecuteRaw !!! */

    X86EFLAGS EFlags = pCtx->eflags;
    if (HWACCMIsEnabled(pVM))
    {
        /* Hardware accelerated raw-mode:
         *
         * Typically only 32-bits protected mode, with paging enabled, code is allowed here.
         */
        if (HWACCMR3CanExecuteGuest(pVM, pCtx) == true)
            return EMSTATE_HWACC;

        /* Note: Raw mode and hw accelerated mode are incompatible. The latter turns
         * off monitoring features essential for raw mode! */
        return EMSTATE_REM;
    }

    /*
     * Standard raw-mode:
     *
     * Here we only support 16 & 32 bits protected mode ring 3 code that has no IO privileges
     * or 32 bits protected mode ring 0 code
     *
     * The tests are ordered by the likelyhood of being true during normal execution.
     */
    if (EFlags.u32 & (X86_EFL_TF /* | HF_INHIBIT_IRQ_MASK*/))
    {
        Log2(("raw mode refused: EFlags=%#x\n", EFlags.u32));
        return EMSTATE_REM;
    }

#ifndef VBOX_RAW_V86
    if (EFlags.u32 & X86_EFL_VM) {
        Log2(("raw mode refused: VM_MASK\n"));
        return EMSTATE_REM;
    }
#endif

    /** @todo check up the X86_CR0_AM flag in respect to raw mode!!! We're probably not emulating it right! */
    uint32_t u32CR0 = pCtx->cr0;
    if ((u32CR0 & (X86_CR0_PG | X86_CR0_PE)) != (X86_CR0_PG | X86_CR0_PE))
    {
        //Log2(("raw mode refused: %s%s%s\n", (u32CR0 & X86_CR0_PG) ? "" : " !PG", (u32CR0 & X86_CR0_PE) ? "" : " !PE", (u32CR0 & X86_CR0_AM) ? "" : " !AM"));
        return EMSTATE_REM;
    }

    if (pCtx->cr4 & X86_CR4_PAE)
    {
        uint32_t u32Dummy, u32Features;

        CPUMGetGuestCpuId(pVM, 1, &u32Dummy, &u32Dummy, &u32Dummy, &u32Features);
        if (!(u32Features & X86_CPUID_FEATURE_EDX_PAE))
            return EMSTATE_REM;
    }

    unsigned uSS = pCtx->ss;
    if (    pCtx->eflags.Bits.u1VM
        ||  (uSS & X86_SEL_RPL) == 3)
    {
        if (!EMIsRawRing3Enabled(pVM))
            return EMSTATE_REM;

        if (!(EFlags.u32 & X86_EFL_IF))
        {
            Log2(("raw mode refused: IF (RawR3)\n"));
            return EMSTATE_REM;
        }

        if (!(u32CR0 & X86_CR0_WP) && EMIsRawRing0Enabled(pVM))
        {
            Log2(("raw mode refused: CR0.WP + RawR0\n"));
            return EMSTATE_REM;
        }
    }
    else
    {
        if (!EMIsRawRing0Enabled(pVM))
            return EMSTATE_REM;

        /* Only ring 0 supervisor code. */
        if ((uSS & X86_SEL_RPL) != 0)
        {
            Log2(("raw r0 mode refused: CPL %d\n", uSS & X86_SEL_RPL));
            return EMSTATE_REM;
        }

        // Let's start with pure 32 bits ring 0 code first
        /** @todo What's pure 32-bit mode? flat? */
        if (    !(pCtx->ssHid.Attr.n.u1DefBig)
            ||  !(pCtx->csHid.Attr.n.u1DefBig))
        {
            Log2(("raw r0 mode refused: SS/CS not 32bit\n"));
            return EMSTATE_REM;
        }

        /* Write protection must be turned on, or else the guest can overwrite our hypervisor code and data. */
        if (!(u32CR0 & X86_CR0_WP))
        {
            Log2(("raw r0 mode refused: CR0.WP=0!\n"));
            return EMSTATE_REM;
        }

        if (PATMShouldUseRawMode(pVM, (RTGCPTR)pCtx->eip))
        {
            Log2(("raw r0 mode forced: patch code\n"));
            return EMSTATE_RAW;
        }

#if !defined(VBOX_ALLOW_IF0) && !defined(VBOX_RUN_INTERRUPT_GATE_HANDLERS)
        if (!(EFlags.u32 & X86_EFL_IF))
        {
            ////Log2(("R0: IF=0 VIF=%d %08X\n", eip, pVMeflags));
            //Log2(("RR0: Interrupts turned off; fall back to emulation\n"));
            return EMSTATE_REM;
        }
#endif

        /** @todo still necessary??? */
        if (EFlags.Bits.u2IOPL != 0)
        {
            Log2(("raw r0 mode refused: IOPL %d\n", EFlags.Bits.u2IOPL));
            return EMSTATE_REM;
        }
    }

    Assert(PGMPhysIsA20Enabled(pVM));
    return EMSTATE_RAW;
}


/**
 * Executes all high priority post execution force actions.
 *
 * @returns rc or a fatal status code.
 *
 * @param   pVM         VM handle.
 * @param   rc          The current rc.
 */
static int emR3HighPriorityPostForcedActions(PVM pVM, int rc)
{
    if (VM_FF_ISSET(pVM, VM_FF_PDM_CRITSECT))
        PDMR3CritSectFF(pVM);

    if (VM_FF_ISSET(pVM, VM_FF_CSAM_PENDING_ACTION))
        CSAMR3DoPendingAction(pVM);

    return rc;
}


/**
 * Executes all pending forced actions.
 *
 * Forced actions can cause execution delays and execution
 * rescheduling. The first we deal with using action priority, so
 * that for instance pending timers aren't scheduled and ran until
 * right before execution. The rescheduling we deal with using
 * return codes. The same goes for VM termination, only in that case
 * we exit everything.
 *
 * @returns VBox status code of equal or greater importance/severity than rc.
 *          The most important ones are: VINF_EM_RESCHEDULE,
 *          VINF_EM_SUSPEND, VINF_EM_RESET and VINF_EM_TERMINATE.
 *
 * @param   pVM         VM handle.
 * @param   rc          The current rc.
 *
 */
static int emR3ForcedActions(PVM pVM, int rc)
{
    STAM_REL_PROFILE_START(&pVM->em.s.StatForcedActions, a);
#ifdef VBOX_STRICT
    int rcIrq = VINF_SUCCESS;
#endif
    int rc2;
#define UPDATE_RC() \
        do { \
            AssertMsg(rc2 <= 0 || (rc2 >= VINF_EM_FIRST && rc2 <= VINF_EM_LAST), ("Invalid FF return code: %Rra\n", rc2)); \
            if (rc2 == VINF_SUCCESS || rc < VINF_SUCCESS) \
                break; \
            if (!rc || rc2 < rc) \
                rc = rc2; \
        } while (0)

    /*
     * Post execution chunk first.
     */
    if (VM_FF_ISPENDING(pVM, VM_FF_NORMAL_PRIORITY_POST_MASK))
    {
        /*
         * Termination request.
         */
        if (VM_FF_ISSET(pVM, VM_FF_TERMINATE))
        {
            Log2(("emR3ForcedActions: returns VINF_EM_TERMINATE\n"));
            STAM_REL_PROFILE_STOP(&pVM->em.s.StatForcedActions, a);
            return VINF_EM_TERMINATE;
        }

        /*
         * Debugger Facility polling.
         */
        if (VM_FF_ISSET(pVM, VM_FF_DBGF))
        {
            rc2 = DBGFR3VMMForcedAction(pVM);
            UPDATE_RC();
        }

        /*
         * Postponed reset request.
         */
        if (VM_FF_ISSET(pVM, VM_FF_RESET))
        {
            rc2 = VMR3Reset(pVM);
            UPDATE_RC();
            VM_FF_CLEAR(pVM, VM_FF_RESET);
        }

        /*
         * CSAM page scanning.
         */
        if (VM_FF_ISSET(pVM, VM_FF_CSAM_SCAN_PAGE))
        {
            PCPUMCTX pCtx = pVM->em.s.pCtx;

            /** @todo: check for 16 or 32 bits code! (D bit in the code selector) */
            Log(("Forced action VM_FF_CSAM_SCAN_PAGE\n"));

            CSAMR3CheckCodeEx(pVM, CPUMCTX2CORE(pCtx), pCtx->eip);
            VM_FF_CLEAR(pVM, VM_FF_CSAM_SCAN_PAGE);
        }

        /* check that we got them all  */
        Assert(!(VM_FF_NORMAL_PRIORITY_POST_MASK & ~(VM_FF_TERMINATE | VM_FF_DBGF | VM_FF_RESET | VM_FF_CSAM_SCAN_PAGE)));
    }

    /*
     * Normal priority then.
     * (Executed in no particular order.)
     */
    if (VM_FF_ISPENDING(pVM, VM_FF_NORMAL_PRIORITY_MASK))
    {
        /*
         * PDM Queues are pending.
         */
        if (VM_FF_ISSET(pVM, VM_FF_PDM_QUEUES))
            PDMR3QueueFlushAll(pVM);

        /*
         * PDM DMA transfers are pending.
         */
        if (VM_FF_ISSET(pVM, VM_FF_PDM_DMA))
            PDMR3DmaRun(pVM);

        /*
         * Requests from other threads.
         */
        if (VM_FF_ISSET(pVM, VM_FF_REQUEST))
        {
            rc2 = VMR3ReqProcessU(pVM->pUVM, VMREQDEST_ANY);
            if (rc2 == VINF_EM_OFF || rc2 == VINF_EM_TERMINATE)
            {
                Log2(("emR3ForcedActions: returns %Rrc\n", rc2));
                STAM_REL_PROFILE_STOP(&pVM->em.s.StatForcedActions, a);
                return rc2;
            }
            UPDATE_RC();
        }

        /* Replay the handler notification changes. */
        if (VM_FF_ISSET(pVM, VM_FF_REM_HANDLER_NOTIFY))
            REMR3ReplayHandlerNotifications(pVM);

        /* check that we got them all  */
        Assert(!(VM_FF_NORMAL_PRIORITY_MASK & ~(VM_FF_REQUEST | VM_FF_PDM_QUEUES | VM_FF_PDM_DMA | VM_FF_REM_HANDLER_NOTIFY)));
    }

    /*
     * Execute polling function ever so often.
     * THIS IS A HACK, IT WILL BE *REPLACED* BY PROPER ASYNC NETWORKING "SOON"!
     */
    static unsigned cLast = 0;
    if (!((++cLast) % 4))
        PDMR3Poll(pVM);

    /*
     * High priority pre execution chunk last.
     * (Executed in ascending priority order.)
     */
    if (VM_FF_ISPENDING(pVM, VM_FF_HIGH_PRIORITY_PRE_MASK))
    {
        /*
         * Timers before interrupts.
         */
        if (VM_FF_ISSET(pVM, VM_FF_TIMER))
            TMR3TimerQueuesDo(pVM);

        /*
         * The instruction following an emulated STI should *always* be executed!
         */
        if (VM_FF_ISSET(pVM, VM_FF_INHIBIT_INTERRUPTS))
        {
            Log(("VM_FF_EMULATED_STI at %RGv successor %RGv\n", (RTGCPTR)CPUMGetGuestRIP(pVM), EMGetInhibitInterruptsPC(pVM)));
            if (CPUMGetGuestEIP(pVM) != EMGetInhibitInterruptsPC(pVM))
            {
                /* Note: we intentionally don't clear VM_FF_INHIBIT_INTERRUPTS here if the eip is the same as the inhibited instr address.
                 *  Before we are able to execute this instruction in raw mode (iret to guest code) an external interrupt might
                 *  force a world switch again. Possibly allowing a guest interrupt to be dispatched in the process. This could
                 *  break the guest. Sounds very unlikely, but such timing sensitive problem are not as rare as you might think.
                 */
                VM_FF_CLEAR(pVM, VM_FF_INHIBIT_INTERRUPTS);
            }
            if (HWACCMR3IsActive(pVM))
                rc2 = VINF_EM_RESCHEDULE_HWACC;
            else
                rc2 = PATMAreInterruptsEnabled(pVM) ? VINF_EM_RESCHEDULE_RAW : VINF_EM_RESCHEDULE_REM;

            UPDATE_RC();
        }

        /*
         * Interrupts.
         */
        if (    !VM_FF_ISSET(pVM, VM_FF_INHIBIT_INTERRUPTS)
            &&  (!rc || rc >= VINF_EM_RESCHEDULE_RAW)
            &&  !TRPMHasTrap(pVM) /* an interrupt could already be scheduled for dispatching in the recompiler. */
            &&  PATMAreInterruptsEnabled(pVM)
            &&  !HWACCMR3IsEventPending(pVM))
        {
            if (VM_FF_ISPENDING(pVM, VM_FF_INTERRUPT_APIC | VM_FF_INTERRUPT_PIC))
            {
                /* Note: it's important to make sure the return code from TRPMR3InjectEvent isn't ignored! */
                /** @todo this really isn't nice, should properly handle this */
                rc2 = TRPMR3InjectEvent(pVM, TRPM_HARDWARE_INT);
#ifdef VBOX_STRICT
                rcIrq = rc2;
#endif
                UPDATE_RC();
            }
            /** @todo really ugly; if we entered the hlt state when exiting the recompiler and an interrupt was pending, we previously got stuck in the halted state. */
            else if (REMR3QueryPendingInterrupt(pVM) != REM_NO_PENDING_IRQ)
            {
                rc2 = VINF_EM_RESCHEDULE_REM;
                UPDATE_RC();
            }
        }

        /*
         * Allocate handy pages.
         */
        if (VM_FF_ISSET(pVM, VM_FF_PGM_NEED_HANDY_PAGES))
        {
            rc2 = PGMR3PhysAllocateHandyPages(pVM);
            UPDATE_RC();
        }

        /*
         * Debugger Facility request.
         */
        if (VM_FF_ISSET(pVM, VM_FF_DBGF))
        {
            rc2 = DBGFR3VMMForcedAction(pVM);
            UPDATE_RC();
        }

        /*
         * Termination request.
         */
        if (VM_FF_ISSET(pVM, VM_FF_TERMINATE))
        {
            Log2(("emR3ForcedActions: returns VINF_EM_TERMINATE\n"));
            STAM_REL_PROFILE_STOP(&pVM->em.s.StatForcedActions, a);
            return VINF_EM_TERMINATE;
        }

#ifdef DEBUG
        /*
         * Debug, pause the VM.
         */
        if (VM_FF_ISSET(pVM, VM_FF_DEBUG_SUSPEND))
        {
            VM_FF_CLEAR(pVM, VM_FF_DEBUG_SUSPEND);
            Log(("emR3ForcedActions: returns VINF_EM_SUSPEND\n"));
            return VINF_EM_SUSPEND;
        }

#endif
        /* check that we got them all  */
        Assert(!(VM_FF_HIGH_PRIORITY_PRE_MASK & ~(VM_FF_TIMER | VM_FF_INTERRUPT_APIC | VM_FF_INTERRUPT_PIC | VM_FF_DBGF | VM_FF_PGM_SYNC_CR3 | VM_FF_PGM_SYNC_CR3_NON_GLOBAL | VM_FF_SELM_SYNC_TSS | VM_FF_TRPM_SYNC_IDT | VM_FF_SELM_SYNC_GDT | VM_FF_SELM_SYNC_LDT | VM_FF_TERMINATE | VM_FF_DEBUG_SUSPEND | VM_FF_INHIBIT_INTERRUPTS | VM_FF_PGM_NEED_HANDY_PAGES)));
    }

#undef UPDATE_RC
    Log2(("emR3ForcedActions: returns %Rrc\n", rc));
    STAM_REL_PROFILE_STOP(&pVM->em.s.StatForcedActions, a);
    Assert(rcIrq == VINF_SUCCESS || rcIrq == rc);
    return rc;
}


/**
 * Execute VM.
 *
 * This function is the main loop of the VM. The emulation thread
 * calls this function when the VM has been successfully constructed
 * and we're ready for executing the VM.
 *
 * Returning from this function means that the VM is turned off or
 * suspended (state already saved) and deconstruction in next in line.
 *
 * All interaction from other thread are done using forced actions
 * and signaling of the wait object.
 *
 * @returns VBox status code, informational status codes may indicate failure.
 * @param   pVM         The VM to operate on.
 * @param   idCpu       VMCPU id.
 */
VMMR3DECL(int) EMR3ExecuteVM(PVM pVM, RTCPUID idCpu)
{
    LogFlow(("EMR3ExecuteVM: pVM=%p enmVMState=%d  enmState=%d (%s) fForceRAW=%d\n", pVM, pVM->enmVMState,
             pVM->em.s.enmState, EMR3GetStateName(pVM->em.s.enmState), pVM->em.s.fForceRAW));
    VM_ASSERT_EMT(pVM);
    Assert(pVM->em.s.enmState == EMSTATE_NONE || pVM->em.s.enmState == EMSTATE_SUSPENDED);

    VMMR3Lock(pVM);

    int rc = setjmp(pVM->em.s.u.FatalLongJump);
    if (rc == 0)
    {
        /*
         * Start the virtual time.
         */
        rc = TMVirtualResume(pVM);
        Assert(rc == VINF_SUCCESS);
        rc = TMCpuTickResume(pVM);
        Assert(rc == VINF_SUCCESS);

        /*
         * The Outer Main Loop.
         */
        bool fFFDone = false;

        /* Reschedule right away to start in the right state. */
        rc = VINF_SUCCESS;
        pVM->em.s.enmState = emR3Reschedule(pVM, pVM->em.s.pCtx);

        STAM_REL_PROFILE_ADV_START(&pVM->em.s.StatTotal, x);
        for (;;)
        {
            /*
             * Before we can schedule anything (we're here because
             * scheduling is required) we must service any pending
             * forced actions to avoid any pending action causing
             * immediate rescheduling upon entering an inner loop
             *
             * Do forced actions.
             */
            if (   !fFFDone
                && rc != VINF_EM_TERMINATE
                && rc != VINF_EM_OFF
                && VM_FF_ISPENDING(pVM, VM_FF_ALL_BUT_RAW_MASK))
            {
                rc = emR3ForcedActions(pVM, rc);
                if (    (   rc == VINF_EM_RESCHEDULE_REM
                         || rc == VINF_EM_RESCHEDULE_HWACC)
                    &&  pVM->em.s.fForceRAW)
                    rc = VINF_EM_RESCHEDULE_RAW;
            }
            else if (fFFDone)
                fFFDone = false;

            /*
             * Now what to do?
             */
            Log2(("EMR3ExecuteVM: rc=%Rrc\n", rc));
            switch (rc)
            {
                /*
                 * Keep doing what we're currently doing.
                 */
                case VINF_SUCCESS:
                    break;

                /*
                 * Reschedule - to raw-mode execution.
                 */
                case VINF_EM_RESCHEDULE_RAW:
                    Log2(("EMR3ExecuteVM: VINF_EM_RESCHEDULE_RAW: %d -> %d (EMSTATE_RAW)\n", pVM->em.s.enmState, EMSTATE_RAW));
                    pVM->em.s.enmState = EMSTATE_RAW;
                    break;

                /*
                 * Reschedule - to hardware accelerated raw-mode execution.
                 */
                case VINF_EM_RESCHEDULE_HWACC:
                    Log2(("EMR3ExecuteVM: VINF_EM_RESCHEDULE_HWACC: %d -> %d (EMSTATE_HWACC)\n", pVM->em.s.enmState, EMSTATE_HWACC));
                    Assert(!pVM->em.s.fForceRAW);
                    pVM->em.s.enmState = EMSTATE_HWACC;
                    break;

                /*
                 * Reschedule - to recompiled execution.
                 */
                case VINF_EM_RESCHEDULE_REM:
                    Log2(("EMR3ExecuteVM: VINF_EM_RESCHEDULE_REM: %d -> %d (EMSTATE_REM)\n", pVM->em.s.enmState, EMSTATE_REM));
                    pVM->em.s.enmState = EMSTATE_REM;
                    break;

#ifdef VBOX_WITH_VMI
                /*
                 * Reschedule - parav call.
                 */
                case VINF_EM_RESCHEDULE_PARAV:
                    Log2(("EMR3ExecuteVM: VINF_EM_RESCHEDULE_PARAV: %d -> %d (EMSTATE_PARAV)\n", pVM->em.s.enmState, EMSTATE_PARAV));
                    pVM->em.s.enmState = EMSTATE_PARAV;
                    break;
#endif

                /*
                 * Resume.
                 */
                case VINF_EM_RESUME:
                    Log2(("EMR3ExecuteVM: VINF_EM_RESUME: %d -> VINF_EM_RESCHEDULE\n", pVM->em.s.enmState));
                    /* fall through and get scheduled. */

                /*
                 * Reschedule.
                 */
                case VINF_EM_RESCHEDULE:
                {
                    EMSTATE enmState = emR3Reschedule(pVM, pVM->em.s.pCtx);
                    Log2(("EMR3ExecuteVM: VINF_EM_RESCHEDULE: %d -> %d (%s)\n", pVM->em.s.enmState, enmState, EMR3GetStateName(enmState)));
                    pVM->em.s.enmState = enmState;
                    break;
                }

                /*
                 * Halted.
                 */
                case VINF_EM_HALT:
                    Log2(("EMR3ExecuteVM: VINF_EM_HALT: %d -> %d\n", pVM->em.s.enmState, EMSTATE_HALTED));
                    pVM->em.s.enmState = EMSTATE_HALTED;
                    break;

                /*
                 * Suspend.
                 */
                case VINF_EM_SUSPEND:
                    Log2(("EMR3ExecuteVM: VINF_EM_SUSPEND: %d -> %d\n", pVM->em.s.enmState, EMSTATE_SUSPENDED));
                    pVM->em.s.enmState = EMSTATE_SUSPENDED;
                    break;

                /*
                 * Reset.
                 * We might end up doing a double reset for now, we'll have to clean up the mess later.
                 */
                case VINF_EM_RESET:
                {
                    EMSTATE enmState = emR3Reschedule(pVM, pVM->em.s.pCtx);
                    Log2(("EMR3ExecuteVM: VINF_EM_RESET: %d -> %d (%s)\n", pVM->em.s.enmState, enmState, EMR3GetStateName(enmState)));
                    pVM->em.s.enmState = enmState;
                    break;
                }

                /*
                 * Power Off.
                 */
                case VINF_EM_OFF:
                    pVM->em.s.enmState = EMSTATE_TERMINATING;
                    Log2(("EMR3ExecuteVM: returns VINF_EM_OFF (%d -> %d)\n", pVM->em.s.enmState, EMSTATE_TERMINATING));
                    TMVirtualPause(pVM);
                    TMCpuTickPause(pVM);
                    VMMR3Unlock(pVM);
                    STAM_REL_PROFILE_ADV_STOP(&pVM->em.s.StatTotal, x);
                    return rc;

                /*
                 * Terminate the VM.
                 */
                case VINF_EM_TERMINATE:
                    pVM->em.s.enmState = EMSTATE_TERMINATING;
                    Log(("EMR3ExecuteVM returns VINF_EM_TERMINATE (%d -> %d)\n", pVM->em.s.enmState, EMSTATE_TERMINATING));
                    TMVirtualPause(pVM);
                    TMCpuTickPause(pVM);
                    STAM_REL_PROFILE_ADV_STOP(&pVM->em.s.StatTotal, x);
                    return rc;

                /*
                 * Guest debug events.
                 */
                case VINF_EM_DBG_STEPPED:
                    AssertMsgFailed(("VINF_EM_DBG_STEPPED cannot be here!"));
                case VINF_EM_DBG_STOP:
                case VINF_EM_DBG_BREAKPOINT:
                case VINF_EM_DBG_STEP:
                    if (pVM->em.s.enmState == EMSTATE_RAW)
                    {
                        Log2(("EMR3ExecuteVM: %Rrc: %d -> %d\n", rc, pVM->em.s.enmState, EMSTATE_DEBUG_GUEST_RAW));
                        pVM->em.s.enmState = EMSTATE_DEBUG_GUEST_RAW;
                    }
                    else
                    {
                        Log2(("EMR3ExecuteVM: %Rrc: %d -> %d\n", rc, pVM->em.s.enmState, EMSTATE_DEBUG_GUEST_REM));
                        pVM->em.s.enmState = EMSTATE_DEBUG_GUEST_REM;
                    }
                    break;

                /*
                 * Hypervisor debug events.
                 */
                case VINF_EM_DBG_HYPER_STEPPED:
                case VINF_EM_DBG_HYPER_BREAKPOINT:
                case VINF_EM_DBG_HYPER_ASSERTION:
                    Log2(("EMR3ExecuteVM: %Rrc: %d -> %d\n", rc, pVM->em.s.enmState, EMSTATE_DEBUG_HYPER));
                    pVM->em.s.enmState = EMSTATE_DEBUG_HYPER;
                    break;

                /*
                 * Guru mediations.
                 */
                case VERR_VMM_RING0_ASSERTION:
                    Log(("EMR3ExecuteVM: %Rrc: %d -> %d (EMSTATE_GURU_MEDITATION)\n", rc, pVM->em.s.enmState, EMSTATE_GURU_MEDITATION));
                    pVM->em.s.enmState = EMSTATE_GURU_MEDITATION;
                    break;

                /*
                 * Any error code showing up here other than the ones we
                 * know and process above are considered to be FATAL.
                 *
                 * Unknown warnings and informational status codes are also
                 * included in this.
                 */
                default:
                    if (RT_SUCCESS(rc))
                    {
                        AssertMsgFailed(("Unexpected warning or informational status code %Rra!\n", rc));
                        rc = VERR_EM_INTERNAL_ERROR;
                    }
                    pVM->em.s.enmState = EMSTATE_GURU_MEDITATION;
                    Log(("EMR3ExecuteVM returns %d\n", rc));
                    break;
            }


            /*
             * Any waiters can now be woken up
             */
            VMMR3Unlock(pVM);
            VMMR3Lock(pVM);

            STAM_PROFILE_ADV_STOP(&pVM->em.s.StatTotal, x); /* (skip this in release) */
            STAM_PROFILE_ADV_START(&pVM->em.s.StatTotal, x);

            /*
             * Act on the state.
             */
            switch (pVM->em.s.enmState)
            {
                /*
                 * Execute raw.
                 */
                case EMSTATE_RAW:
                    rc = emR3RawExecute(pVM, &fFFDone);
                    break;

                /*
                 * Execute hardware accelerated raw.
                 */
                case EMSTATE_HWACC:
                    rc = emR3HwAccExecute(pVM, idCpu, &fFFDone);
                    break;

                /*
                 * Execute recompiled.
                 */
                case EMSTATE_REM:
                    rc = emR3RemExecute(pVM, &fFFDone);
                    Log2(("EMR3ExecuteVM: emR3RemExecute -> %Rrc\n", rc));
                    break;

#ifdef VBOX_WITH_VMI
                /*
                 * Execute PARAV function.
                 */
                case EMSTATE_PARAV:
                    rc = PARAVCallFunction(pVM);
                    pVM->em.s.enmState = EMSTATE_REM;
                    break;
#endif

                /*
                 * hlt - execution halted until interrupt.
                 */
                case EMSTATE_HALTED:
                {
                    STAM_REL_PROFILE_START(&pVM->em.s.StatHalted, y);
                    rc = VMR3WaitHalted(pVM, !(CPUMGetGuestEFlags(pVM) & X86_EFL_IF));
                    STAM_REL_PROFILE_STOP(&pVM->em.s.StatHalted, y);
                    break;
                }

                /*
                 * Suspended - return to VM.cpp.
                 */
                case EMSTATE_SUSPENDED:
                    TMVirtualPause(pVM);
                    TMCpuTickPause(pVM);
                    VMMR3Unlock(pVM);
                    STAM_REL_PROFILE_ADV_STOP(&pVM->em.s.StatTotal, x);
                    return VINF_EM_SUSPEND;

                /*
                 * Debugging in the guest.
                 */
                case EMSTATE_DEBUG_GUEST_REM:
                case EMSTATE_DEBUG_GUEST_RAW:
                    TMVirtualPause(pVM);
                    TMCpuTickPause(pVM);
                    rc = emR3Debug(pVM, rc);
                    TMVirtualResume(pVM);
                    TMCpuTickResume(pVM);
                    Log2(("EMR3ExecuteVM: enmr3Debug -> %Rrc (state %d)\n", rc, pVM->em.s.enmState));
                    break;

                /*
                 * Debugging in the hypervisor.
                 */
                case EMSTATE_DEBUG_HYPER:
                {
                    TMVirtualPause(pVM);
                    TMCpuTickPause(pVM);
                    STAM_REL_PROFILE_ADV_STOP(&pVM->em.s.StatTotal, x);

                    rc = emR3Debug(pVM, rc);
                    Log2(("EMR3ExecuteVM: enmr3Debug -> %Rrc (state %d)\n", rc, pVM->em.s.enmState));
                    if (rc != VINF_SUCCESS)
                    {
                        /* switch to guru meditation mode */
                        pVM->em.s.enmState = EMSTATE_GURU_MEDITATION;
                        VMMR3FatalDump(pVM, rc);
                        return rc;
                    }

                    STAM_REL_PROFILE_ADV_START(&pVM->em.s.StatTotal, x);
                    TMVirtualResume(pVM);
                    TMCpuTickResume(pVM);
                    break;
                }

                /*
                 * Guru meditation takes place in the debugger.
                 */
                case EMSTATE_GURU_MEDITATION:
                {
                    TMVirtualPause(pVM);
                    TMCpuTickPause(pVM);
                    VMMR3FatalDump(pVM, rc);
                    emR3Debug(pVM, rc);
                    VMMR3Unlock(pVM);
                    STAM_REL_PROFILE_ADV_STOP(&pVM->em.s.StatTotal, x);
                    return rc;
                }

                /*
                 * The states we don't expect here.
                 */
                case EMSTATE_NONE:
                case EMSTATE_TERMINATING:
                default:
                    AssertMsgFailed(("EMR3ExecuteVM: Invalid state %d!\n", pVM->em.s.enmState));
                    pVM->em.s.enmState = EMSTATE_GURU_MEDITATION;
                    TMVirtualPause(pVM);
                    TMCpuTickPause(pVM);
                    VMMR3Unlock(pVM);
                    STAM_REL_PROFILE_ADV_STOP(&pVM->em.s.StatTotal, x);
                    return VERR_EM_INTERNAL_ERROR;
            }
        } /* The Outer Main Loop */
    }
    else
    {
        /*
         * Fatal error.
         */
        LogFlow(("EMR3ExecuteVM: returns %Rrc (longjmp / fatal error)\n", rc));
        TMVirtualPause(pVM);
        TMCpuTickPause(pVM);
        VMMR3FatalDump(pVM, rc);
        emR3Debug(pVM, rc);
        VMMR3Unlock(pVM);
        STAM_REL_PROFILE_ADV_STOP(&pVM->em.s.StatTotal, x);
        /** @todo change the VM state! */
        return rc;
    }

    /* (won't ever get here). */
    AssertFailed();
}

