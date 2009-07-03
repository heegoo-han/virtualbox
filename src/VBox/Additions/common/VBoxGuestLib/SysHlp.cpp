/* $Revision$ */
/** @file
 * VBoxGuestLibR0 - IDC with VBoxGuest and HGCM helpers.
 */

/*
 * Copyright (C) 2006-2009 Sun Microsystems, Inc.
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

#define LOG_GROUP LOG_GROUP_HGCM
#include <VBox/log.h>

#include <VBox/VBoxGuestLib.h>
#include "SysHlp.h"

#include <iprt/assert.h>
#if !defined (RT_OS_WINDOWS) \
 && (!defined (RT_OS_LINUX) || defined (VBOX_WITH_COMMON_VBOXGUEST_ON_LINUX))
# include <iprt/memobj.h>
#endif


int vbglLockLinear (void **ppvCtx, void *pv, uint32_t u32Size, bool fWriteAccess)
{
    int rc = VINF_SUCCESS;

    /* Zero size buffers shouldn't be locked. */
    if (u32Size == 0)
    {
        Assert(pv == NULL);
#ifdef RT_OS_WINDOWS
        *ppvCtx = NULL;
#else
        *ppvCtx = NIL_RTR0MEMOBJ;
#endif
        return VINF_SUCCESS;
    }

#ifdef RT_OS_WINDOWS
    PMDL pMdl = IoAllocateMdl (pv, u32Size, FALSE, FALSE, NULL);

    if (pMdl == NULL)
    {
        rc = VERR_NOT_SUPPORTED;
        AssertMsgFailed(("IoAllocateMdl %p %x failed!!\n", pv, u32Size));
    }
    else
    {
        __try {
            /* Calls to MmProbeAndLockPages must be enclosed in a try/except block. */
            MmProbeAndLockPages (pMdl,
                                 KernelMode,
                                 (fWriteAccess) ? IoModifyAccess : IoReadAccess);

            *ppvCtx = pMdl;

        } __except(EXCEPTION_EXECUTE_HANDLER) {

            IoFreeMdl (pMdl);
            rc = VERR_INVALID_PARAMETER;
            AssertMsgFailed(("MmProbeAndLockPages %p %x failed!!\n", pv, u32Size));
        }
    }

#elif defined (RT_OS_LINUX) && !defined (VBOX_WITH_COMMON_VBOXGUEST_ON_LINUX)
    /** @todo r=frank: Linux: pv is at least in some cases, e.g. with VBoxMapFolder,
     *  an R0 address -- the memory was allocated with kmalloc(). I don't know
     *  if this is true in any case.
     * r=michael: on Linux, we sometimes have R3 addresses (e.g. shared
     *  clipboard) and sometimes R0 (e.g. shared folders).  We really ought
     *  to have two separate paths here - at any rate, Linux R0 shouldn't
     *  end up calling this API.  In practice, Linux R3 does it's own thing
     *  before winding up in the R0 path - which calls this stub API.
     */
    NOREF(ppvCtx);
    NOREF(pv);
    NOREF(u32Size);

#else
    /* Default to IPRT - this ASSUMES that it is USER addresses we're locking. */
    RTR0MEMOBJ MemObj;
    rc = RTR0MemObjLockUser(&MemObj, (RTR3PTR)pv, u32Size, NIL_RTR0PROCESS);
    if (RT_SUCCESS(rc))
        *ppvCtx = MemObj;
    else
        *ppvCtx = NIL_RTR0MEMOBJ;

#endif

    return rc;
}

void vbglUnlockLinear (void *pvCtx, void *pv, uint32_t u32Size)
{
    NOREF(pv);
    NOREF(u32Size);

#ifdef RT_OS_WINDOWS
    PMDL pMdl = (PMDL)pvCtx;

    Assert(pMdl);
    if (pMdl != NULL)
    {
        MmUnlockPages (pMdl);
        IoFreeMdl (pMdl);
    }

#elif defined (RT_OS_LINUX) && !defined (VBOX_WITH_COMMON_VBOXGUEST_ON_LINUX)
    NOREF(pvCtx);

#else
    /* default to IPRT */
    RTR0MEMOBJ MemObj = (RTR0MEMOBJ)pvCtx;
    int rc = RTR0MemObjFree(MemObj, false);
    AssertRC(rc);

#endif
}

#ifndef VBGL_VBOXGUEST

# if defined (RT_OS_LINUX) && !defined (__KERNEL__) /** @todo r=bird: What is this for?????? */
#  include <unistd.h>
#  include <errno.h>
#  include <sys/fcntl.h>
#  include <sys/ioctl.h>
# endif

# if defined (RT_OS_LINUX) && !defined (VBOX_WITH_COMMON_VBOXGUEST_ON_LINUX)
RT_C_DECLS_BEGIN
extern DECLVBGL(void *) vboxadd_cmc_open (void);
extern DECLVBGL(void) vboxadd_cmc_close (void *);
extern DECLVBGL(int) vboxadd_cmc_call (void *opaque, uint32_t func, void *data);
RT_C_DECLS_END
# endif /* RT_OS_LINUX */

# ifdef RT_OS_OS2
RT_C_DECLS_BEGIN
/*
 * On OS/2 we'll do the connecting in the assembly code of the
 * client driver, exporting a g_VBoxGuestIDC symbol containing
 * the connection information obtained from the 16-bit IDC.
 */
extern VBOXGUESTOS2IDCCONNECT g_VBoxGuestIDC;
RT_C_DECLS_END
# endif

# if !defined(RT_OS_OS2) \
  && !defined(RT_OS_WINDOWS) \
  && (!defined (RT_OS_LINUX) || defined (VBOX_WITH_COMMON_VBOXGUEST_ON_LINUX))
RT_C_DECLS_BEGIN
extern DECLVBGL(void *) VBoxGuestIDCOpen (uint32_t *pu32Version);
extern DECLVBGL(void)   VBoxGuestIDCClose (void *pvOpaque);
extern DECLVBGL(int)    VBoxGuestIDCCall (void *pvOpaque, unsigned int iCmd, void *pvData, size_t cbSize, size_t *pcbReturn);
RT_C_DECLS_END
# endif

int vbglDriverOpen (VBGLDRIVER *pDriver)
{
# ifdef RT_OS_WINDOWS
    UNICODE_STRING uszDeviceName;
    RtlInitUnicodeString (&uszDeviceName, L"\\Device\\VBoxGuest");

    PDEVICE_OBJECT pDeviceObject = NULL;
    PFILE_OBJECT pFileObject = NULL;

    NTSTATUS rc = IoGetDeviceObjectPointer (&uszDeviceName, FILE_ALL_ACCESS,
                                            &pFileObject, &pDeviceObject);

    if (NT_SUCCESS (rc))
    {
        Log(("vbglDriverOpen VBoxGuest successful pDeviceObject=%x\n", pDeviceObject));
        pDriver->pDeviceObject = pDeviceObject;
        pDriver->pFileObject = pFileObject;
        return VINF_SUCCESS;
    }
    /** @todo return RTErrConvertFromNtStatus(rc)! */
    Log(("vbglDriverOpen VBoxGuest failed with ntstatus=%x\n", rc));
    return rc;

# elif defined (RT_OS_LINUX) && !defined (VBOX_WITH_COMMON_VBOXGUEST_ON_LINUX)

    void *opaque;

    opaque = (void *) vboxadd_cmc_open ();
    if (!opaque)
    {
        return VERR_NOT_IMPLEMENTED;
    }
    pDriver->opaque = opaque;
    return VINF_SUCCESS;

# elif defined (RT_OS_OS2)
    /*
     * Just check whether the connection was made or not.
     */
    if (    g_VBoxGuestIDC.u32Version == VMMDEV_VERSION
        &&  VALID_PTR(g_VBoxGuestIDC.u32Session)
        &&  VALID_PTR(g_VBoxGuestIDC.pfnServiceEP))
    {
        pDriver->u32Session = g_VBoxGuestIDC.u32Session;
        return VINF_SUCCESS;
    }
    pDriver->u32Session = UINT32_MAX;
    Log(("vbglDriverOpen: failed\n"));
    return VERR_FILE_NOT_FOUND;

# else
    uint32_t u32VMMDevVersion;
    pDriver->pvOpaque = VBoxGuestIDCOpen (&u32VMMDevVersion);
    if (    pDriver->pvOpaque
        &&  u32VMMDevVersion == VMMDEV_VERSION)
        return VINF_SUCCESS;

    Log(("vbglDriverOpen: failed\n"));
    return VERR_FILE_NOT_FOUND;
# endif
}

# ifdef RT_OS_WINDOWS
static NTSTATUS vbglDriverIOCtlCompletion (IN PDEVICE_OBJECT DeviceObject,
                                           IN PIRP Irp,
                                           IN PVOID Context)
{
    Log(("VBGL completion %x\n", Irp));

    KEVENT *pEvent = (KEVENT *)Context;
    KeSetEvent (pEvent, IO_NO_INCREMENT, FALSE);

    return STATUS_MORE_PROCESSING_REQUIRED;
}
# endif

int vbglDriverIOCtl (VBGLDRIVER *pDriver, uint32_t u32Function, void *pvData, uint32_t cbData)
{
    Log(("vbglDriverIOCtl: pDriver: %p, Func: %x, pvData: %p, cbData: %d\n", pDriver, u32Function, pvData, cbData));

# ifdef RT_OS_WINDOWS
    KEVENT Event;

    KeInitializeEvent (&Event, NotificationEvent, FALSE);

    /* Have to use the IoAllocateIRP method because this code is generic and
     * must work in any thread context.
     * The IoBuildDeviceIoControlRequest, which was used here, does not work
     * when APCs are disabled, for example.
     */
    PIRP irp = IoAllocateIrp (pDriver->pDeviceObject->StackSize, FALSE);

    Log(("vbglDriverIOCtl: irp %p, IRQL = %d\n", irp, KeGetCurrentIrql()));

    if (irp == NULL)
    {
        Log(("vbglDriverIOCtl: IRP allocation failed!\n"));
        return VERR_NO_MEMORY;
    }

    /*
     * Setup the IRP_MJ_DEVICE_CONTROL IRP.
     */

    PIO_STACK_LOCATION nextStack = IoGetNextIrpStackLocation (irp);

    nextStack->MajorFunction = IRP_MJ_DEVICE_CONTROL;
    nextStack->MinorFunction = 0;
    nextStack->DeviceObject = pDriver->pDeviceObject;
    nextStack->Parameters.DeviceIoControl.OutputBufferLength = cbData;
    nextStack->Parameters.DeviceIoControl.InputBufferLength = cbData;
    nextStack->Parameters.DeviceIoControl.IoControlCode = u32Function;
    nextStack->Parameters.DeviceIoControl.Type3InputBuffer = pvData;

    irp->AssociatedIrp.SystemBuffer = pvData; /* Output buffer. */
    irp->MdlAddress = NULL;

    /* A completion routine is required to signal the Event. */
    IoSetCompletionRoutine (irp, vbglDriverIOCtlCompletion, &Event, TRUE, TRUE, TRUE);

    NTSTATUS rc = IoCallDriver (pDriver->pDeviceObject, irp);

    if (NT_SUCCESS (rc))
    {
        /* Wait the event to be signalled by the completion routine. */
        KeWaitForSingleObject (&Event,
                               Executive,
                               KernelMode,
                               FALSE,
                               NULL);

        rc = irp->IoStatus.Status;

        Log(("vbglDriverIOCtl: wait completed IRQL = %d\n", KeGetCurrentIrql()));
    }

    IoFreeIrp (irp);

    if (rc != STATUS_SUCCESS)
        Log(("vbglDriverIOCtl: ntstatus=%x\n", rc));

    return NT_SUCCESS(rc)? VINF_SUCCESS: VERR_VBGL_IOCTL_FAILED;

# elif defined (RT_OS_LINUX) && !defined (VBOX_WITH_COMMON_VBOXGUEST_ON_LINUX)
    return vboxadd_cmc_call (pDriver->opaque, u32Function, pvData);

# elif defined (RT_OS_OS2)
    if (    pDriver->u32Session
        &&  pDriver->u32Session == g_VBoxGuestIDC.u32Session)
        return g_VBoxGuestIDC.pfnServiceEP(pDriver->u32Session, u32Function, pvData, cbData, NULL);

    Log(("vbglDriverIOCtl: No connection\n"));
    return VERR_WRONG_ORDER;

# else
    return VBoxGuestIDCCall(pDriver->pvOpaque, u32Function, pvData, cbData, NULL);
# endif
}

void vbglDriverClose (VBGLDRIVER *pDriver)
{
# ifdef RT_OS_WINDOWS
    Log(("vbglDriverClose pDeviceObject=%x\n", pDriver->pDeviceObject));
    ObDereferenceObject (pDriver->pFileObject);

# elif defined (RT_OS_LINUX) && !defined (VBOX_WITH_COMMON_VBOXGUEST_ON_LINUX)
    vboxadd_cmc_close (pDriver->opaque);

# elif defined (RT_OS_OS2)
    pDriver->u32Session = 0;

# else
    VBoxGuestIDCClose (pDriver->pvOpaque);
# endif
}

#endif /* !VBGL_VBOXGUEST */

