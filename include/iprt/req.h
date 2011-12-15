/** @file
 * IPRT - Request Queue & Pool.
 */

/*
 * Copyright (C) 2006-2011 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 *
 * The contents of this file may alternatively be used under the terms
 * of the Common Development and Distribution License Version 1.0
 * (CDDL) only, as it comes in the "COPYING.CDDL" file of the
 * VirtualBox OSE distribution, in which case the provisions of the
 * CDDL are applicable instead of those of the GPL.
 *
 * You may elect to license modified versions of this file under the
 * terms and conditions of either the GPL or the CDDL or both.
 */

#ifndef ___iprt_req_h
#define ___iprt_req_h

#include <iprt/cdefs.h>
#include <iprt/types.h>

#include <iprt/stdarg.h>

RT_C_DECLS_BEGIN

/** @defgroup grp_rt_req    RTReq - Request Queue & Pool.
 * @ingroup grp_rt
 * @{
 */

/** Request queue handle. */
typedef struct RTREQQUEUEINT *RTREQQUEUE;
/** Pointer to a request queue handle. */
typedef RTREQQUEUE *PRTREQQUEUE;
/** NIL request queue handle. */
#define NIL_RTREQQUEUE      ((RTREQQUEUE)0)

/** Request thread pool handle. */
typedef struct RTREQPOOLINT *RTREQPOOL;
/** Poiner to a request thread pool handle. */
typedef RTREQPOOL *PRTREQPOOL;
/** NIL request pool handle. */
#define NIL_RTREQPOOL       ((RTREQPOOL)0)


/**
 * Request type.
 */
typedef enum RTREQTYPE
{
    /** Invalid request. */
    RTREQTYPE_INVALID = 0,
    /** RT: Internal. */
    RTREQTYPE_INTERNAL,
    /** Maximum request type (exclusive). Used for validation. */
    RTREQTYPE_MAX
} RTREQTYPE;

/**
 * Request flags.
 */
typedef enum RTREQFLAGS
{
    /** The request returns a iprt status code. */
    RTREQFLAGS_IPRT_STATUS  = 0,
    /** The request is a void request and have no status code. */
    RTREQFLAGS_VOID         = 1,
    /** Return type mask. */
    RTREQFLAGS_RETURN_MASK  = 1,
    /** Caller does not wait on the packet, Queue process thread will free it. */
    RTREQFLAGS_NO_WAIT      = 2
} RTREQFLAGS;


/** A request packet. */
typedef struct RTREQ RTREQ;
/** Pointer to an RT request packet. */
typedef RTREQ *PRTREQ;


#ifdef IN_RING3

/**
 * Create a request packet queue
 *
 * @returns iprt status code.
 * @param   phQueue         Where to store the request queue handle.
 */
RTDECL(int) RTReqQueueCreate(PRTREQQUEUE phQueue);

/**
 * Destroy a request packet queue
 *
 * @returns iprt status code.
 * @param   hQueue          The request queue.
 */
RTDECL(int) RTReqQueueDestroy(RTREQQUEUE hQueue);

/**
 * Process one or more request packets
 *
 * @returns iprt status code.
 * @returns VERR_TIMEOUT if cMillies was reached without the packet being added.
 *
 * @param   hQueue          The request queue.
 * @param   cMillies        Number of milliseconds to wait for a pending request.
 *                          Use RT_INDEFINITE_WAIT to only wait till one is added.
 */
RTDECL(int) RTReqQueueProcess(RTREQQUEUE hQueue, RTMSINTERVAL cMillies);

/**
 * Allocate and queue a call request.
 *
 * If it's desired to poll on the completion of the request set cMillies
 * to 0 and use RTReqWait() to check for completion. In the other case
 * use RT_INDEFINITE_WAIT.
 * The returned request packet must be freed using RTReqRelease().
 *
 * @returns iprt statuscode.
 *          Will not return VERR_INTERRUPTED.
 * @returns VERR_TIMEOUT if cMillies was reached without the packet being completed.
 *
 * @param   hQueue          The request queue.
 * @param   ppReq           Where to store the pointer to the request.
 *                          This will be NULL or a valid request pointer not matter what happens.
 * @param   cMillies        Number of milliseconds to wait for the request to
 *                          be completed. Use RT_INDEFINITE_WAIT to only
 *                          wait till it's completed.
 * @param   pfnFunction     Pointer to the function to call.
 * @param   cArgs           Number of arguments following in the ellipsis.
 * @param   ...             Function arguments.
 *
 * @remarks See remarks on RTReqQueueCallV.
 */
RTDECL(int) RTReqQueueCall(RTREQQUEUE hQueue, PRTREQ *ppReq, RTMSINTERVAL cMillies, PFNRT pfnFunction, unsigned cArgs, ...);

/**
 * Allocate and queue a call request to a void function.
 *
 * If it's desired to poll on the completion of the request set cMillies
 * to 0 and use RTReqWait() to check for completion. In the other case
 * use RT_INDEFINITE_WAIT.
 * The returned request packet must be freed using RTReqRelease().
 *
 * @returns iprt status code.
 *          Will not return VERR_INTERRUPTED.
 * @returns VERR_TIMEOUT if cMillies was reached without the packet being completed.
 *
 * @param   hQueue          The request queue.
 * @param   ppReq           Where to store the pointer to the request.
 *                          This will be NULL or a valid request pointer not matter what happens.
 * @param   cMillies        Number of milliseconds to wait for the request to
 *                          be completed. Use RT_INDEFINITE_WAIT to only
 *                          wait till it's completed.
 * @param   pfnFunction     Pointer to the function to call.
 * @param   cArgs           Number of arguments following in the ellipsis.
 * @param   ...             Function arguments.
 *
 * @remarks See remarks on RTReqQueueCallV.
 */
RTDECL(int) RTReqQueueCallVoid(RTREQQUEUE hQueue, PRTREQ *ppReq, RTMSINTERVAL cMillies, PFNRT pfnFunction, unsigned cArgs, ...);

/**
 * Allocate and queue a call request to a void function.
 *
 * If it's desired to poll on the completion of the request set cMillies
 * to 0 and use RTReqWait() to check for completion. In the other case
 * use RT_INDEFINITE_WAIT.
 * The returned request packet must be freed using RTReqRelease().
 *
 * @returns iprt status code.
 *          Will not return VERR_INTERRUPTED.
 * @returns VERR_TIMEOUT if cMillies was reached without the packet being completed.
 *
 * @param   hQueue          The request queue.
 * @param   ppReq           Where to store the pointer to the request.
 *                          This will be NULL or a valid request pointer not matter what happens, unless fFlags
 *                          contains RTREQFLAGS_NO_WAIT when it will be optional and always NULL.
 * @param   cMillies        Number of milliseconds to wait for the request to
 *                          be completed. Use RT_INDEFINITE_WAIT to only
 *                          wait till it's completed.
 * @param   fFlags          A combination of the RTREQFLAGS values.
 * @param   pfnFunction     Pointer to the function to call.
 * @param   cArgs           Number of arguments following in the ellipsis.
 * @param   ...             Function arguments.
 *
 * @remarks See remarks on RTReqQueueCallV.
 */
RTDECL(int) RTReqQueueCallEx(RTREQQUEUE hQueue, PRTREQ *ppReq, RTMSINTERVAL cMillies, unsigned fFlags, PFNRT pfnFunction, unsigned cArgs, ...);

/**
 * Allocate and queue a call request.
 *
 * If it's desired to poll on the completion of the request set cMillies
 * to 0 and use RTReqWait() to check for completion. In the other case
 * use RT_INDEFINITE_WAIT.
 * The returned request packet must be freed using RTReqRelease().
 *
 * @returns iprt status code.
 *          Will not return VERR_INTERRUPTED.
 * @returns VERR_TIMEOUT if cMillies was reached without the packet being completed.
 *
 * @param   hQueue          The request queue.
 * @param   ppReq           Where to store the pointer to the request.
 *                          This will be NULL or a valid request pointer not matter what happens, unless fFlags
 *                          contains RTREQFLAGS_NO_WAIT when it will be optional and always NULL.
 * @param   cMillies        Number of milliseconds to wait for the request to
 *                          be completed. Use RT_INDEFINITE_WAIT to only
 *                          wait till it's completed.
 * @param   fFlags          A combination of the RTREQFLAGS values.
 * @param   pfnFunction     Pointer to the function to call.
 * @param   cArgs           Number of arguments following in the ellipsis.
 * @param   Args            Variable argument vector.
 *
 * @remarks Caveats:
 *              - Do not pass anything which is larger than an uintptr_t.
 *              - 64-bit integers are larger than uintptr_t on 32-bit hosts.
 *                Pass integers > 32-bit by reference (pointers).
 *              - Don't use NULL since it should be the integer 0 in C++ and may
 *                therefore end up with garbage in the bits 63:32 on 64-bit
 *                hosts because 'int' is 32-bit.
 *                Use (void *)NULL or (uintptr_t)0 instead of NULL.
 */
RTDECL(int) RTReqQueueCallV(RTREQQUEUE hQueue, PRTREQ *ppReq, RTMSINTERVAL cMillies, unsigned fFlags, PFNRT pfnFunction, unsigned cArgs, va_list Args);

/**
 * Checks if the queue is busy or not.
 *
 * The caller is responsible for dealing with any concurrent submitts.
 *
 * @returns true if busy, false if idle.
 * @param   hQueue              The queue.
 */
RTDECL(bool) RTReqQueueIsBusy(RTREQQUEUE hQueue);

/**
 * Allocates a request packet.
 *
 * The caller allocates a request packet, fills in the request data
 * union and queues the request.
 *
 * @returns iprt status code.
 *
 * @param   hQueue          The request queue.
 * @param   enmType         Package type.
 * @param   phReq           Where to store the handle to the new request.
 */
RTDECL(int) RTReqQueueAlloc(RTREQQUEUE hQueue, RTREQTYPE enmType, PRTREQ *phReq);


/**
 * Retainsa reference to a request thread pool.
 *
 * @returns The new reference count, UINT32_MAX on invalid handle (asserted).
 * @param   hPool           The request thread pool handle.
 */
RTDECL(uint32_t) RTReqPoolRetain(RTREQPOOL hPool);

/**
 * Releases a reference to the request thread pool.
 *
 * When the reference count reaches zero, the request will be pooled for reuse.
 *
 * @returns The new reference count, UINT32_MAX on invalid handle (asserted).
 * @param   hPool           The request thread pool handle.
 */
RTDECL(uint32_t) RTReqPoolRelease(RTREQPOOL hPool);

/**
 * Request thread pool configuration variable.
 */
typedef enum RTREQPOOLCFGVAR
{
    /** Invalid zero value. */
    RTREQPOOLCFGVAR_INVALID = 0,
    /** The desired RTTHREADTYPE of the worker threads. */
    RTREQPOOLCFGVAR_THREAD_TYPE,
    /** The minimum number of threads to keep handy once spawned. */
    RTREQPOOLCFGVAR_MIN_THREADS,
    /** The maximum number of thread to start. */
    RTREQPOOLCFGVAR_MAX_THREADS,
    /** The minimum number of milliseconds a worker thread needs to be idle
     * before we consider shutting it down.  The other shutdown criteria
     * being set by RTREQPOOLCFGVAR_MIN_THREADS.  The value
     * RT_INDEFINITE_WAIT can be used to disable shutting down idle threads. */
    RTREQPOOLCFGVAR_MS_MIN_IDLE,
    /** The sleep period, in milliseoncds, to employ when idling. The value
     * RT_INDEFINITE_WAIT can be used to disable shutting down idle threads. */
    RTREQPOOLCFGVAR_MS_IDLE_SLEEP,
    /** The number of threads at which to start pushing back. The value
     *  UINT64_MAX is an alias for the current upper thread count limit, i.e.
     *  disabling push back.  The value 0 (zero) is an alias for the current
     *  lower thread count, a good value to start pushing back at.  The value
     *  must otherwise be within  */
    RTREQPOOLCFGVAR_PUSH_BACK_THRESHOLD,
    /** The minimum push back time in milliseconds. */
    RTREQPOOLCFGVAR_PUSH_BACK_MIN_MS,
    /** The maximum push back time in milliseconds. */
    RTREQPOOLCFGVAR_PUSH_BACK_MAX_MS,
    /** The maximum number of free requests to keep handy for recycling. */
    RTREQPOOLCFGVAR_MAX_FREE_REQUESTS,
    /** The end of the range of valid config variables. */
    RTREQPOOLCFGVAR_END,
    /** Blow the type up to 32-bits. */
    RTREQPOOLCFGVAR_32BIT_HACK = 0x7fffffff
} RTREQPOOLCFGVAR;


/**
 * Sets a config variable for a request thread pool.
 *
 * @returns IPRT status code.
 * @param   hPool           The pool handle.
 * @param   enmVar          The variable to set.
 * @param   uValue          The new value.
 */
RTDECL(int) RTReqPoolSetCfgVar(RTREQPOOL hPool, RTREQPOOLCFGVAR enmVar, uint64_t uValue);

/**
 * Gets a config variable for a request thread pool.
 *
 * @returns IPRT status code.
 * @param   hPool           The pool handle.
 * @param   enmVar          The variable to query.
 * @param   puValue         Where to return the value.
 */
RTDECL(int) RTReqPoolQueryCfgVar(RTREQPOOL hPool, RTREQPOOLCFGVAR enmVar, uint64_t *puValue);

/**
 * Request thread pool statistics value names.
 */
typedef enum RTREQPOOLSTAT
{
    /** The invalid zero value, as per tradition. */
    RTREQPOOLSTAT_INVALID = 0,
    /** The current number of worker threads. */
    RTREQPOOLSTAT_THREADS,
    /** The number of threads that have been created. */
    RTREQPOOLSTAT_THREADS_CREATED,
    /** The total number of requests that have been processed. */
    RTREQPOOLSTAT_REQUESTS_PROCESSED,
    /** The total number of requests that have been submitted. */
    RTREQPOOLSTAT_REQUESTS_SUBMITTED,
    /** the current number of pending (waiting) requests. */
    RTREQPOOLSTAT_REQUESTS_PENDING,
    /** The current number of active (executing) requests. */
    RTREQPOOLSTAT_REQUESTS_ACTIVE,
    /** The current number of free (recycled) requests. */
    RTREQPOOLSTAT_REQUESTS_FREE,
    /** Total time the requests took to process. */
    RTREQPOOLSTAT_NS_TOTAL_REQ_PROCESSING,
    /** Total time the requests had to wait in the queue before being
     * scheduled. */
    RTREQPOOLSTAT_NS_TOTAL_REQ_QUEUED,
    /** Average time the requests took to process. */
    RTREQPOOLSTAT_NS_AVERAGE_REQ_PROCESSING,
    /** Average time the requests had to wait in the queue before being
     * scheduled. */
    RTREQPOOLSTAT_NS_AVERAGE_REQ_QUEUED,
    /** The end of the valid statistics value names. */
    RTREQPOOLSTAT_END,
    /** Blow the type up to 32-bit. */
    RTREQPOOLSTAT_32BIT_HACK = 0x7fffffff
} RTREQPOOLSTAT;

/**
 * Read a statistics value from the request thread pool.
 *
 * @returns The value, UINT64_MAX if an invalid parameter was given.
 * @param   hPool           The request thread pool handle.
 * @param   enmStat         The statistics value to get.
 */
RTDECL(uint64_t) RTReqPoolGetStat(RTREQPOOL hPool, RTREQPOOLSTAT enmStat);

/**
 * Allocates a request packet.
 *
 * This is mostly for internal use, please use the convenience methods.
 *
 * @returns iprt status code.
 *
 * @param   hPool           The request thread pool handle.
 * @param   enmType         Package type.
 * @param   phReq           Where to store the handle to the new request.
 */
RTDECL(int) RTReqPoolAlloc(RTREQPOOL hPool, RTREQTYPE enmType, PRTREQ *phReq);


/**
 * Retainsa reference to a request.
 *
 * @returns The new reference count, UINT32_MAX on invalid handle (asserted).
 * @param   hReq            The request handle.
 */
RTDECL(uint32_t) RTReqRetain(PRTREQ hReq);

/**
 * Releases a reference to the request.
 *
 * When the reference count reaches zero, the request will be pooled for reuse.
 *
 * @returns The new reference count, UINT32_MAX on invalid handle (asserted).
 * @param   hReq            Package to release.
 */
RTDECL(uint32_t) RTReqRelease(PRTREQ hReq);

/**
 * Queue a request.
 *
 * The quest must be allocated using RTReqQueueAlloc() or RTReqPoolAlloc() and
 * contain all the required data.
 *
 * If it's desired to poll on the completion of the request set cMillies
 * to 0 and use RTReqWait() to check for completion. In the other case
 * use RT_INDEFINITE_WAIT.
 *
 * @returns iprt status code.
 *          Will not return VERR_INTERRUPTED.
 * @returns VERR_TIMEOUT if cMillies was reached without the packet being completed.
 *
 * @param   pReq            The request to queue.
 * @param   cMillies        Number of milliseconds to wait for the request to
 *                          be completed. Use RT_INDEFINITE_WAIT to only
 *                          wait till it's completed.
 */
RTDECL(int) RTReqSubmit(PRTREQ pReq, RTMSINTERVAL cMillies);


/**
 * Wait for a request to be completed.
 *
 * @returns iprt status code.
 *          Will not return VERR_INTERRUPTED.
 * @returns VERR_TIMEOUT if cMillies was reached without the packet being completed.
 *
 * @param   pReq            The request to wait for.
 * @param   cMillies        Number of milliseconds to wait.
 *                          Use RT_INDEFINITE_WAIT to only wait till it's completed.
 */
RTDECL(int) RTReqWait(PRTREQ pReq, RTMSINTERVAL cMillies);

/**
 * Get the status of the request.
 *
 * @returns Status code in the IPRT tradition.
 *
 * @param   pReq            The request.
 */
RTDECL(int) RTReqGetStatus(PRTREQ pReq);

#endif /* IN_RING3 */


/** @} */

RT_C_DECLS_END

#endif

