
/* $Id$ */
/** @file
 * VirtualBox Main - XXX.
 */

/*
 * Copyright (C) 2012 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */


/*******************************************************************************
*   Header Files                                                               *
*******************************************************************************/
#include "GuestImpl.h"
#include "GuestSessionImpl.h"
#include "GuestCtrlImplPrivate.h"

#include "Global.h"
#include "AutoCaller.h"
#include "ConsoleImpl.h"
#include "MachineImpl.h"
#include "ProgressImpl.h"

#include <memory> /* For auto_ptr. */

#include <iprt/env.h>
#include <iprt/file.h> /* For CopyTo/From. */

#ifdef LOG_GROUP
 #undef LOG_GROUP
#endif
#define LOG_GROUP LOG_GROUP_GUEST_CONTROL
#include <VBox/log.h>


/*******************************************************************************
*   Defines                                                                    *
*******************************************************************************/

/**
 * Update file flags.
 */
#define UPDATEFILE_FLAG_NONE                RT_BIT(0)
/** Copy over the file from host to the
 *  guest. */
#define UPDATEFILE_FLAG_COPY_FROM_ISO       RT_BIT(1)
/** Execute file on the guest after it has
 *  been successfully transfered. */
#define UPDATEFILE_FLAG_EXECUTE             RT_BIT(7)
/** File is optional, does not have to be
 *  existent on the .ISO. */
#define UPDATEFILE_FLAG_OPTIONAL            RT_BIT(8)


// session task classes
/////////////////////////////////////////////////////////////////////////////

GuestSessionTask::GuestSessionTask(GuestSession *pSession)
{
    mSession = pSession;
}

GuestSessionTask::~GuestSessionTask(void)
{
}

int GuestSessionTask::getGuestProperty(const ComObjPtr<Guest> &pGuest,
                                       const Utf8Str &strPath, Utf8Str &strValue)
{
    ComObjPtr<Console> pConsole = pGuest->getConsole();
    const ComPtr<IMachine> pMachine = pConsole->machine();

    Assert(!pMachine.isNull());
    Bstr strTemp, strFlags;
    LONG64 i64Timestamp;
    HRESULT hr = pMachine->GetGuestProperty(Bstr(strPath).raw(),
                                            strTemp.asOutParam(),
                                            &i64Timestamp, strFlags.asOutParam());
    if (SUCCEEDED(hr))
    {
        strValue = strTemp;
        return VINF_SUCCESS;
    }
    return VERR_NOT_FOUND;
}

int GuestSessionTask::setProgress(ULONG uPercent)
{
    if (mProgress.isNull()) /* Progress is optional. */
        return VINF_SUCCESS;

    BOOL fCanceled;
    if (   SUCCEEDED(mProgress->COMGETTER(Canceled(&fCanceled)))
        && fCanceled)
        return VERR_CANCELLED;
    BOOL fCompleted;
    if (   SUCCEEDED(mProgress->COMGETTER(Completed(&fCompleted)))
        && fCompleted)
    {
        AssertMsgFailed(("Setting value of an already completed progress\n"));
        return VINF_SUCCESS;
    }
    HRESULT hr = mProgress->SetCurrentOperationProgress(uPercent);
    if (FAILED(hr))
        return VERR_COM_UNEXPECTED;

    return VINF_SUCCESS;
}

int GuestSessionTask::setProgressSuccess(void)
{
    if (mProgress.isNull()) /* Progress is optional. */
        return VINF_SUCCESS;

    BOOL fCanceled;
    BOOL fCompleted;
    if (   SUCCEEDED(mProgress->COMGETTER(Canceled(&fCanceled)))
        && !fCanceled
        && SUCCEEDED(mProgress->COMGETTER(Completed(&fCompleted)))
        && !fCompleted)
    {
        HRESULT hr = mProgress->notifyComplete(S_OK);
        if (FAILED(hr))
            return VERR_COM_UNEXPECTED; /** @todo Find a better rc. */
    }

    return VINF_SUCCESS;
}

HRESULT GuestSessionTask::setProgressErrorMsg(HRESULT hr, const Utf8Str &strMsg)
{
    if (mProgress.isNull()) /* Progress is optional. */
        return hr; /* Return original rc. */

    BOOL fCanceled;
    BOOL fCompleted;
    if (   SUCCEEDED(mProgress->COMGETTER(Canceled(&fCanceled)))
        && !fCanceled
        && SUCCEEDED(mProgress->COMGETTER(Completed(&fCompleted)))
        && !fCompleted)
    {
        HRESULT hr2 = mProgress->notifyComplete(hr,
                                               COM_IIDOF(IGuestSession),
                                               GuestSession::getStaticComponentName(),
                                               strMsg.c_str());
        if (FAILED(hr2))
            return hr2;
    }
    return hr; /* Return original rc. */
}

SessionTaskCopyTo::SessionTaskCopyTo(GuestSession *pSession,
                                     const Utf8Str &strSource, const Utf8Str &strDest, uint32_t uFlags)
                                     : GuestSessionTask(pSession),
                                       mSource(strSource),
                                       mSourceFile(NULL),
                                       mSourceOffset(0),
                                       mSourceSize(0),
                                       mDest(strDest)
{
    mCopyFileFlags = uFlags;
}

/** @todo Merge this and the above call and let the above call do the open/close file handling so that the
 *        inner code only has to deal with file handles. No time now ... */
SessionTaskCopyTo::SessionTaskCopyTo(GuestSession *pSession,
                                     PRTFILE pSourceFile, size_t cbSourceOffset, uint64_t cbSourceSize,
                                     const Utf8Str &strDest, uint32_t uFlags)
                                     : GuestSessionTask(pSession)
{
    mSourceFile    = pSourceFile;
    mSourceOffset  = cbSourceOffset;
    mSourceSize    = cbSourceSize;
    mDest          = strDest;
    mCopyFileFlags = uFlags;
}

SessionTaskCopyTo::~SessionTaskCopyTo(void)
{

}

int SessionTaskCopyTo::Run(void)
{
    LogFlowThisFuncEnter();

    ComObjPtr<GuestSession> pSession = mSession;
    Assert(!pSession.isNull());

    AutoCaller autoCaller(pSession);
    if (FAILED(autoCaller.rc())) return autoCaller.rc();

    if (mCopyFileFlags)
    {
        setProgressErrorMsg(VBOX_E_IPRT_ERROR,
                            Utf8StrFmt(GuestSession::tr("Copy flags (%#x) not implemented yet"),
                            mCopyFileFlags));
        return VERR_INVALID_PARAMETER;
    }

    int rc;

    RTFILE fileLocal;
    PRTFILE pFile = &fileLocal;

    if (!mSourceFile)
    {
        /* Does our source file exist? */
        if (!RTFileExists(mSource.c_str()))
        {
            rc = setProgressErrorMsg(VBOX_E_IPRT_ERROR,
                                     Utf8StrFmt(GuestSession::tr("Source file \"%s\" does not exist or is not a file"),
                                                mSource.c_str()));
        }
        else
        {
            rc = RTFileOpen(pFile, mSource.c_str(),
                            RTFILE_O_OPEN | RTFILE_O_READ | RTFILE_O_DENY_WRITE);
            if (RT_FAILURE(rc))
            {
                rc = setProgressErrorMsg(VBOX_E_IPRT_ERROR,
                                         Utf8StrFmt(GuestSession::tr("Could not open source file \"%s\" for reading: %Rrc"),
                                                    mSource.c_str(), rc));
            }
            else
            {
                rc = RTFileGetSize(*pFile, &mSourceSize);
                if (RT_FAILURE(rc))
                {
                    setProgressErrorMsg(VBOX_E_IPRT_ERROR,
                                        Utf8StrFmt(GuestSession::tr("Could not query file size of \"%s\": %Rrc"),
                                                   mSource.c_str(), rc));
                }
            }
        }
    }
    else
    {
        pFile = mSourceFile;
        /* Size + offset are optional. */
    }

    GuestProcessStartupInfo procInfo;
    procInfo.mName    = Utf8StrFmt(GuestSession::tr("Copying file \"%s\" to the guest to \"%s\" (%RU64 bytes)"),
                                   mSource.c_str(), mDest.c_str(), mSourceSize);
    procInfo.mCommand = Utf8Str(VBOXSERVICE_TOOL_CAT);
    procInfo.mFlags   = ProcessCreateFlag_Hidden;

    /* Set arguments.*/
    procInfo.mArguments.push_back(Utf8StrFmt("--output=%s", mDest.c_str())); /** @todo Do we need path conversion? */

    /* Startup process. */
    ComObjPtr<GuestProcess> pProcess;
    rc = pSession->processCreateExInteral(procInfo, pProcess);
    if (RT_SUCCESS(rc))
        rc = pProcess->startProcess();
    if (RT_FAILURE(rc))
    {
        setProgressErrorMsg(VBOX_E_IPRT_ERROR,
                            Utf8StrFmt(GuestSession::tr("Unable to start guest process: %Rrc"), rc));
    }
    else
    {
        GuestProcessWaitResult waitRes;
        BYTE byBuf[_64K];

        BOOL fCanceled = FALSE;
        uint64_t cbWrittenTotal = 0;
        uint64_t cbToRead = mSourceSize;

        for (;;)
        {
            rc = pProcess->waitFor(ProcessWaitForFlag_StdIn,
                                   30 * 1000 /* Timeout */, waitRes);
            if (   RT_FAILURE(rc)
                || (   waitRes.mResult != ProcessWaitResult_StdIn
                    && waitRes.mResult != ProcessWaitResult_WaitFlagNotSupported))
            {
                break;
            }

            /* If the guest does not support waiting for stdin, we now yield in
             * order to reduce the CPU load due to busy waiting. */
            if (waitRes.mResult == ProcessWaitResult_WaitFlagNotSupported)
                RTThreadYield(); /* Optional, don't check rc. */

            size_t cbRead = 0;
            if (mSourceSize) /* If we have nothing to write, take a shortcut. */
            {
                /** @todo Not very efficient, but works for now. */
                rc = RTFileSeek(*pFile, mSourceOffset + cbWrittenTotal,
                                RTFILE_SEEK_BEGIN, NULL /* poffActual */);
                if (RT_SUCCESS(rc))
                {
                    rc = RTFileRead(*pFile, (uint8_t*)byBuf,
                                    RT_MIN(cbToRead, sizeof(byBuf)), &cbRead);
                    /*
                     * Some other error occured? There might be a chance that RTFileRead
                     * could not resolve/map the native error code to an IPRT code, so just
                     * print a generic error.
                     */
                    if (RT_FAILURE(rc))
                    {
                        setProgressErrorMsg(VBOX_E_IPRT_ERROR,
                                            Utf8StrFmt(GuestSession::tr("Could not read from file \"%s\" (%Rrc)"),
                                                       mSource.c_str(), rc));
                        break;
                    }
                }
                else
                {
                    setProgressErrorMsg(VBOX_E_IPRT_ERROR,
                                        Utf8StrFmt(GuestSession::tr("Seeking file \"%s\" offset %RU64 failed: %Rrc"),
                                                   mSource.c_str(), cbWrittenTotal, rc));
                    break;
                }
            }

            uint32_t fFlags = ProcessInputFlag_None;

            /* Did we reach the end of the content we want to transfer (last chunk)? */
            if (   (cbRead < sizeof(byBuf))
                /* Did we reach the last block which is exactly _64K? */
                || (cbToRead - cbRead == 0)
                /* ... or does the user want to cancel? */
                || (   SUCCEEDED(mProgress->COMGETTER(Canceled(&fCanceled)))
                    && fCanceled)
               )
            {
                fFlags |= ProcessInputFlag_EndOfFile;
            }

            uint32_t cbWritten;
            Assert(sizeof(byBuf) >= cbRead);
            rc = pProcess->writeData(0 /* StdIn */, fFlags,
                                     byBuf, cbRead,
                                     30 * 1000 /* Timeout */, &cbWritten);
            if (RT_FAILURE(rc))
            {
                setProgressErrorMsg(VBOX_E_IPRT_ERROR,
                                    Utf8StrFmt(GuestSession::tr("Writing to file \"%s\" (offset %RU64) failed: %Rrc"),
                                               mDest.c_str(), cbWrittenTotal, rc));
                break;
            }

            LogFlowThisFunc(("cbWritten=%RU32, cbToRead=%RU64, cbWrittenTotal=%RU64, cbFileSize=%RU64\n",
                             cbWritten, cbToRead - cbWritten, cbWrittenTotal + cbWritten, mSourceSize));

            /* Only subtract bytes reported written by the guest. */
            Assert(cbToRead >= cbWritten);
            cbToRead -= cbWritten;

            /* Update total bytes written to the guest. */
            cbWrittenTotal += cbWritten;
            Assert(cbWrittenTotal <= mSourceSize);

            /* Did the user cancel the operation above? */
            if (fCanceled)
                break;

            /* Update the progress.
             * Watch out for division by zero. */
            mSourceSize > 0
                ? rc = setProgress((ULONG)(cbWrittenTotal * 100 / mSourceSize))
                : rc = setProgress(100);
            if (RT_FAILURE(rc))
                break;

            /* End of file reached? */
            if (!cbToRead)
                break;
        } /* for */

        if (   !fCanceled
            || RT_SUCCESS(rc))
        {
            /*
             * Even if we succeeded until here make sure to check whether we really transfered
             * everything.
             */
            if (   mSourceSize > 0
                && cbWrittenTotal == 0)
            {
                /* If nothing was transfered but the file size was > 0 then "vbox_cat" wasn't able to write
                 * to the destination -> access denied. */
                rc = setProgressErrorMsg(VBOX_E_IPRT_ERROR,
                                         Utf8StrFmt(GuestSession::tr("Access denied when copying file \"%s\" to \"%s\""),
                                                    mSource.c_str(), mDest.c_str()));
            }
            else if (cbWrittenTotal < mSourceSize)
            {
                /* If we did not copy all let the user know. */
                rc = setProgressErrorMsg(VBOX_E_IPRT_ERROR,
                                         Utf8StrFmt(GuestSession::tr("Copying file \"%s\" failed (%RU64/%RU64 bytes transfered)"),
                                                    mSource.c_str(), cbWrittenTotal, mSourceSize));
            }
            else
            {
                rc = pProcess->waitFor(ProcessWaitForFlag_Terminate,
                                       30 * 1000 /* Timeout */, waitRes);
                if (   RT_FAILURE(rc)
                    || waitRes.mResult != ProcessWaitResult_Terminate)
                {
                    if (RT_FAILURE(rc))
                        rc = setProgressErrorMsg(VBOX_E_IPRT_ERROR,
                                                 Utf8StrFmt(GuestSession::tr("Waiting on termination for copying file \"%s\" failed: %Rrc"),
                                                            mSource.c_str(), rc));
                    else
                        rc = setProgressErrorMsg(VBOX_E_IPRT_ERROR,
                                                 Utf8StrFmt(GuestSession::tr("Waiting on termination for copying file \"%s\" failed with wait result %ld"),
                                                            mSource.c_str(), waitRes.mResult));
                }

                if (RT_SUCCESS(rc))
                {
                    ProcessStatus_T procStatus;
                    LONG exitCode;
                    if (   (   SUCCEEDED(pProcess->COMGETTER(Status(&procStatus)))
                            && procStatus != ProcessStatus_TerminatedNormally)
                        || (   SUCCEEDED(pProcess->COMGETTER(ExitCode(&exitCode)))
                            && exitCode != 0)
                       )
                    {
                        rc = setProgressErrorMsg(VBOX_E_IPRT_ERROR,
                                                 Utf8StrFmt(GuestSession::tr("Copying file \"%s\" failed with status %ld, exit code %ld"),
                                                            mSource.c_str(), procStatus, exitCode)); /**@todo Add stringify methods! */
                    }
                }

                if (RT_SUCCESS(rc))
                    rc = setProgressSuccess();
            }
        }

        if (!pProcess.isNull())
            pProcess->uninit();
    } /* processCreateExInteral */

    if (!mSourceFile) /* Only close locally opened files. */
        RTFileClose(*pFile);

    LogFlowFuncLeaveRC(rc);
    return rc;
}

int SessionTaskCopyTo::RunAsync(const Utf8Str &strDesc, ComObjPtr<Progress> &pProgress)
{
    LogFlowThisFunc(("strDesc=%s, strSource=%s, strDest=%s, mCopyFileFlags=%x\n",
                     strDesc.c_str(), mSource.c_str(), mDest.c_str(), mCopyFileFlags));

    mDesc = strDesc;
    mProgress = pProgress;

    int rc = RTThreadCreate(NULL, SessionTaskCopyTo::taskThread, this,
                            0, RTTHREADTYPE_MAIN_HEAVY_WORKER, 0,
                            "gctlCpyTo");
    LogFlowFuncLeaveRC(rc);
    return rc;
}

/* static */
int SessionTaskCopyTo::taskThread(RTTHREAD Thread, void *pvUser)
{
    std::auto_ptr<SessionTaskCopyTo> task(static_cast<SessionTaskCopyTo*>(pvUser));
    AssertReturn(task.get(), VERR_GENERAL_FAILURE);

    LogFlowFunc(("pTask=%p\n", task.get()));
    return task->Run();
}

SessionTaskCopyFrom::SessionTaskCopyFrom(GuestSession *pSession,
                                         const Utf8Str &strSource, const Utf8Str &strDest, uint32_t uFlags)
                                         : GuestSessionTask(pSession)
{
    mSource = strSource;
    mDest   = strDest;
    mFlags  = uFlags;
}

SessionTaskCopyFrom::~SessionTaskCopyFrom(void)
{

}

int SessionTaskCopyFrom::Run(void)
{
    LogFlowThisFuncEnter();

    ComObjPtr<GuestSession> pSession = mSession;
    Assert(!pSession.isNull());

    AutoCaller autoCaller(pSession);
    if (FAILED(autoCaller.rc())) return autoCaller.rc();

    /*
     * Note: There will be races between querying file size + reading the guest file's
     *       content because we currently *do not* lock down the guest file when doing the
     *       actual operations.
     ** @todo Implement guest file locking!
     */
    GuestFsObjData objData;
    int rc = pSession->fileQueryInfoInternal(Utf8Str(mSource), objData);
    if (RT_FAILURE(rc))
    {
        setProgressErrorMsg(VBOX_E_IPRT_ERROR,
                            Utf8StrFmt(GuestSession::tr("Querying guest file information for \"%s\" failed: %Rrc"),
                            mSource.c_str(), rc));
    }
    else if (objData.mType != FsObjType_File) /* Only single files are supported at the moment. */
    {
        rc = setProgressErrorMsg(VBOX_E_IPRT_ERROR,
                                 Utf8StrFmt(GuestSession::tr("Object \"%s\" on the guest is not a file"), mSource.c_str()));
    }

    if (RT_SUCCESS(rc))
    {
        RTFILE fileDest;
        rc = RTFileOpen(&fileDest, mDest.c_str(),
                        RTFILE_O_WRITE | RTFILE_O_OPEN_CREATE | RTFILE_O_DENY_WRITE); /** @todo Use the correct open modes! */
        if (RT_FAILURE(rc))
        {
            setProgressErrorMsg(VBOX_E_IPRT_ERROR,
                                Utf8StrFmt(GuestSession::tr("Error opening destination file \"%s\": %Rrc"),
                                           mDest.c_str(), rc));
        }
        else
        {
            GuestProcessStartupInfo procInfo;
            procInfo.mName    = Utf8StrFmt(GuestSession::tr("Copying file \"%s\" from guest to the host to \"%s\" (%RI64 bytes)"),
                                                            mSource.c_str(), mDest.c_str(), objData.mObjectSize);
            procInfo.mCommand   = Utf8Str(VBOXSERVICE_TOOL_CAT);
            procInfo.mFlags     = ProcessCreateFlag_Hidden | ProcessCreateFlag_WaitForStdOut;

            /* Set arguments.*/
            procInfo.mArguments.push_back(mSource); /* Which file to output? */

            /* Startup process. */
            ComObjPtr<GuestProcess> pProcess;
            rc = pSession->processCreateExInteral(procInfo, pProcess);
            if (RT_SUCCESS(rc))
                rc = pProcess->startProcess();
            if (RT_FAILURE(rc))
            {
                setProgressErrorMsg(VBOX_E_IPRT_ERROR,
                                    Utf8StrFmt(GuestSession::tr("Unable to start guest process for copying data from guest to host: %Rrc"), rc));
            }
            else
            {
                GuestProcessWaitResult waitRes;
                BYTE byBuf[_64K];

                BOOL fCanceled = FALSE;
                uint64_t cbWrittenTotal = 0;
                uint64_t cbToRead = objData.mObjectSize;

                for (;;)
                {
                    rc = pProcess->waitFor(ProcessWaitForFlag_StdOut,
                                           30 * 1000 /* Timeout */, waitRes);
                    if (   waitRes.mResult == ProcessWaitResult_StdOut
                        || waitRes.mResult == ProcessWaitResult_WaitFlagNotSupported)
                    {
                        /* If the guest does not support waiting for stdin, we now yield in
                         * order to reduce the CPU load due to busy waiting. */
                        if (waitRes.mResult == ProcessWaitResult_WaitFlagNotSupported)
                            RTThreadYield(); /* Optional, don't check rc. */

                        size_t cbRead;
                        rc = pProcess->readData(OUTPUT_HANDLE_ID_STDOUT, sizeof(byBuf),
                                                30 * 1000 /* Timeout */, byBuf, sizeof(byBuf),
                                                &cbRead);
                        if (RT_FAILURE(rc))
                        {
                            setProgressErrorMsg(VBOX_E_IPRT_ERROR,
                                                Utf8StrFmt(GuestSession::tr("Reading from file \"%s\" (offset %RU64) failed: %Rrc"),
                                                mSource.c_str(), cbWrittenTotal, rc));
                            break;
                        }

                        if (cbRead)
                        {
                            rc = RTFileWrite(fileDest, byBuf, cbRead, NULL /* No partial writes */);
                            if (RT_FAILURE(rc))
                            {
                                setProgressErrorMsg(VBOX_E_IPRT_ERROR,
                                                    Utf8StrFmt(GuestSession::tr("Error writing to file \"%s\" (%RU64 bytes left): %Rrc"),
                                                    mDest.c_str(), cbToRead, rc));
                                break;
                            }

                            /* Only subtract bytes reported written by the guest. */
                            Assert(cbToRead >= cbRead);
                            cbToRead -= cbRead;

                            /* Update total bytes written to the guest. */
                            cbWrittenTotal += cbRead;
                            Assert(cbWrittenTotal <= (uint64_t)objData.mObjectSize);

                            /* Did the user cancel the operation above? */
                            if (   SUCCEEDED(mProgress->COMGETTER(Canceled(&fCanceled)))
                                && fCanceled)
                                break;

                            rc = setProgress((ULONG)(cbWrittenTotal / ((uint64_t)objData.mObjectSize / 100.0)));
                            if (RT_FAILURE(rc))
                                break;
                        }
                    }
                    else if (   RT_FAILURE(rc)
                             || waitRes.mResult == ProcessWaitResult_Terminate
                             || waitRes.mResult == ProcessWaitResult_Error
                             || waitRes.mResult == ProcessWaitResult_Timeout)
                    {
                        if (RT_FAILURE(waitRes.mRC))
                            rc = waitRes.mRC;
                        break;
                    }
                } /* for */

                LogFlowThisFunc(("rc=%Rrc, cbWrittenTotal=%RU64, cbSize=%RI64, cbToRead=%RU64\n",
                                 rc, cbWrittenTotal, objData.mObjectSize, cbToRead));

                if (   !fCanceled
                    || RT_SUCCESS(rc))
                {
                    /*
                     * Even if we succeeded until here make sure to check whether we really transfered
                     * everything.
                     */
                    if (   objData.mObjectSize > 0
                        && cbWrittenTotal == 0)
                    {
                        /* If nothing was transfered but the file size was > 0 then "vbox_cat" wasn't able to write
                         * to the destination -> access denied. */
                        setProgressErrorMsg(VBOX_E_IPRT_ERROR,
                                            Utf8StrFmt(GuestSession::tr("Access denied when copying file \"%s\" to \"%s\""),
                                            mSource.c_str(), mDest.c_str()));
                        rc = VERR_GENERAL_FAILURE; /* Fudge. */
                    }
                    else if (cbWrittenTotal < (uint64_t)objData.mObjectSize)
                    {
                        /* If we did not copy all let the user know. */
                        setProgressErrorMsg(VBOX_E_IPRT_ERROR,
                                            Utf8StrFmt(GuestSession::tr("Copying file \"%s\" failed (%RU64/%RI64 bytes transfered)"),
                                            mSource.c_str(), cbWrittenTotal, objData.mObjectSize));
                        rc = VERR_GENERAL_FAILURE; /* Fudge. */
                    }
                    else
                    {
                        ProcessStatus_T procStatus;
                        LONG exitCode;
                        if (   (   SUCCEEDED(pProcess->COMGETTER(Status(&procStatus)))
                                && procStatus != ProcessStatus_TerminatedNormally)
                            || (   SUCCEEDED(pProcess->COMGETTER(ExitCode(&exitCode)))
                                && exitCode != 0)
                           )
                        {
                            setProgressErrorMsg(VBOX_E_IPRT_ERROR,
                                                Utf8StrFmt(GuestSession::tr("Copying file \"%s\" failed with status %ld, exit code %d"),
                                                mSource.c_str(), procStatus, exitCode)); /**@todo Add stringify methods! */
                            rc = VERR_GENERAL_FAILURE; /* Fudge. */
                        }
                        else /* Yay, success! */
                            rc = setProgressSuccess();
                    }
                }

                if (!pProcess.isNull())
                    pProcess->uninit();
            }

            RTFileClose(fileDest);
        }
    }

    LogFlowFuncLeaveRC(rc);
    return rc;
}

int SessionTaskCopyFrom::RunAsync(const Utf8Str &strDesc, ComObjPtr<Progress> &pProgress)
{
    LogFlowThisFunc(("strDesc=%s, strSource=%s, strDest=%s, uFlags=%x\n",
                     strDesc.c_str(), mSource.c_str(), mDest.c_str(), mFlags));

    mDesc = strDesc;
    mProgress = pProgress;

    int rc = RTThreadCreate(NULL, SessionTaskCopyFrom::taskThread, this,
                            0, RTTHREADTYPE_MAIN_HEAVY_WORKER, 0,
                            "gctlCpyFrom");
    LogFlowFuncLeaveRC(rc);
    return rc;
}

/* static */
int SessionTaskCopyFrom::taskThread(RTTHREAD Thread, void *pvUser)
{
    std::auto_ptr<SessionTaskCopyFrom> task(static_cast<SessionTaskCopyFrom*>(pvUser));
    AssertReturn(task.get(), VERR_GENERAL_FAILURE);

    LogFlowFunc(("pTask=%p\n", task.get()));
    return task->Run();
}

SessionTaskUpdateAdditions::SessionTaskUpdateAdditions(GuestSession *pSession,
                                                       const Utf8Str &strSource, uint32_t uFlags)
                                                       : GuestSessionTask(pSession)
{
    mSource = strSource;
    mFlags  = uFlags;
}

SessionTaskUpdateAdditions::~SessionTaskUpdateAdditions(void)
{

}

int SessionTaskUpdateAdditions::copyFileToGuest(GuestSession *pSession, PRTISOFSFILE pISO,
                                                Utf8Str const &strFileSource, const Utf8Str &strFileDest,
                                                bool fOptional, uint32_t *pcbSize)
{
    AssertPtrReturn(pSession, VERR_INVALID_POINTER);
    AssertPtrReturn(pISO, VERR_INVALID_POINTER);
    /* pcbSize is optional. */

    uint32_t cbOffset;
    size_t cbSize;

    int rc = RTIsoFsGetFileInfo(pISO, strFileSource.c_str(), &cbOffset, &cbSize);
    if (RT_FAILURE(rc))
    {
        if (fOptional)
            return VINF_SUCCESS;

        return rc;
    }

    Assert(cbOffset);
    Assert(cbSize);
    rc = RTFileSeek(pISO->file, cbOffset, RTFILE_SEEK_BEGIN, NULL);

    /* Copy over the Guest Additions file to the guest. */
    if (RT_SUCCESS(rc))
    {
        LogFlowThisFunc(("Copying Guest Additions installer file \"%s\" to \"%s\" on guest ...\n",
                         strFileSource.c_str(), strFileDest.c_str()));

        if (RT_SUCCESS(rc))
        {
            SessionTaskCopyTo *pTask = new SessionTaskCopyTo(pSession /* GuestSession */,
                                                             &pISO->file, cbOffset, cbSize,
                                                             strFileDest, CopyFileFlag_None);
            AssertPtrReturn(pTask, VERR_NO_MEMORY);

            ComObjPtr<Progress> pProgressCopyTo;
            rc = pSession->startTaskAsync(Utf8StrFmt(GuestSession::tr("Copying Guest Additions installer file \"%s\" to \"%s\" on guest"),
                                                     mSource.c_str(), strFileDest.c_str()),
                                                     pTask, pProgressCopyTo);
            if (RT_SUCCESS(rc))
            {
                BOOL fCanceled = FALSE;
                HRESULT hr = pProgressCopyTo->WaitForCompletion(-1);
                if (   SUCCEEDED(pProgressCopyTo->COMGETTER(Canceled)(&fCanceled))
                    && fCanceled)
                {
                    rc = VERR_GENERAL_FAILURE; /* Fudge. */
                }
                else if (FAILED(hr))
                {
                    Assert(FAILED(hr));
                    rc = VERR_GENERAL_FAILURE; /* Fudge. */
                }
            }
        }
    }

    /** @todo Note: Since there is no file locking involved at the moment, there can be modifications
     *              between finished copying, the verification and the actual execution. */

    /* Determine where the installer image ended up and if it has the correct size. */
    if (RT_SUCCESS(rc))
    {
        LogFlowThisFunc(("Verifying Guest Additions installer file \"%s\" ...\n",
                         strFileDest.c_str()));

        GuestFsObjData objData;
        int64_t cbSizeOnGuest;
        rc = pSession->fileQuerySizeInternal(strFileDest, &cbSizeOnGuest);
        if (   RT_SUCCESS(rc)
            && cbSize == (uint64_t)cbSizeOnGuest)
        {
            LogFlowThisFunc(("Guest Additions installer file \"%s\" successfully verified\n",
                             strFileDest.c_str()));
        }
        else
        {
            if (RT_SUCCESS(rc)) /* Size does not match. */
            {
                LogFlowThisFunc(("Size of Guest Additions installer file \"%s\" does not match: %RI64bytes copied, %RU64bytes expected\n",
                                 strFileDest.c_str(), cbSizeOnGuest, cbSize));
                rc = VERR_BROKEN_PIPE; /** @todo Find a better error. */
            }
            else
                LogFlowThisFunc(("Error copying Guest Additions installer file \"%s\": %Rrc\n",
                                 strFileDest.c_str(), rc));
        }

        if (RT_SUCCESS(rc))
        {
            if (pcbSize)
                *pcbSize = cbSizeOnGuest;
        }
    }

    return rc;
}

int SessionTaskUpdateAdditions::runFileOnGuest(GuestSession *pSession, GuestProcessStartupInfo &procInfo)
{
    AssertPtrReturn(pSession, VERR_INVALID_POINTER);

    ComObjPtr<GuestProcess> pProcess;
    int rc = pSession->processCreateExInteral(procInfo, pProcess);
    if (RT_SUCCESS(rc))
        rc = pProcess->startProcess();

    if (RT_SUCCESS(rc))
    {
        LogRel(("Running %s ...\n", procInfo.mName.c_str()));

        GuestProcessWaitResult waitRes;
        rc = pProcess->waitFor(ProcessWaitForFlag_Terminate,
                               10 * 60 * 1000 /* 10 mins Timeout */, waitRes);
        if (waitRes.mResult == ProcessWaitResult_Terminate)
        {
            ProcessStatus_T procStatus;
            LONG exitCode;
            if (   (   SUCCEEDED(pProcess->COMGETTER(Status(&procStatus)))
                    && procStatus != ProcessStatus_TerminatedNormally)
                || (   SUCCEEDED(pProcess->COMGETTER(ExitCode(&exitCode)))
                    && exitCode != 0)
               )
            {
                setProgressErrorMsg(VBOX_E_IPRT_ERROR,
                                    Utf8StrFmt(GuestSession::tr("Running %s failed with status %ld, exit code %ld"),
                                               procInfo.mName.c_str(), procStatus, exitCode));
                rc = VERR_GENERAL_FAILURE; /* Fudge. */
            }
            else /* Yay, success! */
            {
                LogFlowThisFunc(("%s successfully completed\n", procInfo.mName.c_str()));
            }
        }
        else
        {
            if (RT_FAILURE(rc))
                setProgressErrorMsg(VBOX_E_IPRT_ERROR,
                                    Utf8StrFmt(GuestSession::tr("Error while waiting running %s: %Rrc"),
                                               procInfo.mName.c_str(), rc));
            else
            {
                setProgressErrorMsg(VBOX_E_IPRT_ERROR, pProcess->errorMsg());
                rc = VERR_GENERAL_FAILURE; /* Fudge. */
            }
        }
    }

    if (!pProcess.isNull())
        pProcess->uninit();

    return rc;
}

int SessionTaskUpdateAdditions::Run(void)
{
    LogFlowThisFuncEnter();

    ComObjPtr<GuestSession> pSession = mSession;
    Assert(!pSession.isNull());

    AutoCaller autoCaller(pSession);
    if (FAILED(autoCaller.rc())) return autoCaller.rc();

    int rc = setProgress(10);
    if (RT_FAILURE(rc))
        return rc;

    HRESULT hr = S_OK;

    LogRel(("Automatic update of Guest Additions started, using \"%s\"\n", mSource.c_str()));

    ComObjPtr<Guest> pGuest(mSession->getParent());
#if 0
    /*
     * Wait for the guest being ready within 30 seconds.
     */
    AdditionsRunLevelType_T addsRunLevel;
    uint64_t tsStart = RTTimeSystemMilliTS();
    while (   SUCCEEDED(hr = pGuest->COMGETTER(AdditionsRunLevel)(&addsRunLevel))
           && (    addsRunLevel != AdditionsRunLevelType_Userland
                && addsRunLevel != AdditionsRunLevelType_Desktop))
    {
        if ((RTTimeSystemMilliTS() - tsStart) > 30 * 1000)
        {
            rc = VERR_TIMEOUT;
            break;
        }

        RTThreadSleep(100); /* Wait a bit. */
    }

    if (FAILED(hr)) rc = VERR_TIMEOUT;
    if (rc == VERR_TIMEOUT)
        hr = setProgressErrorMsg(VBOX_E_NOT_SUPPORTED,
                                 Utf8StrFmt(GuestSession::tr("Guest Additions were not ready within time, giving up")));
#else
    /*
     * For use with the GUI we don't want to wait, just return so that the manual .ISO mounting
     * can continue.
     */
    AdditionsRunLevelType_T addsRunLevel;
    if (   FAILED(hr = pGuest->COMGETTER(AdditionsRunLevel)(&addsRunLevel))
        || (   addsRunLevel != AdditionsRunLevelType_Userland
            && addsRunLevel != AdditionsRunLevelType_Desktop))
    {
        if (addsRunLevel == AdditionsRunLevelType_System)
            hr = setProgressErrorMsg(VBOX_E_NOT_SUPPORTED,
                                     Utf8StrFmt(GuestSession::tr("Guest Additions are installed but not fully loaded yet, aborting automatic update")));
        else
            hr = setProgressErrorMsg(VBOX_E_NOT_SUPPORTED,
                                     Utf8StrFmt(GuestSession::tr("Guest Additions not installed or ready, aborting automatic update")));
        rc = VERR_NOT_SUPPORTED;
    }
#endif

    if (RT_SUCCESS(rc))
    {
        /*
         * Determine if we are able to update automatically. This only works
         * if there are recent Guest Additions installed already.
         */
        Utf8Str strAddsVer;
        rc = getGuestProperty(pGuest, "/VirtualBox/GuestAdd/Version", strAddsVer);
        if (   RT_SUCCESS(rc)
            && RTStrVersionCompare(strAddsVer.c_str(), "4.1") < 0)
        {
            hr = setProgressErrorMsg(VBOX_E_NOT_SUPPORTED,
                                     Utf8StrFmt(GuestSession::tr("Guest has too old Guest Additions (%s) installed for automatic updating, please update manually"),
                                                strAddsVer.c_str()));
            rc = VERR_NOT_SUPPORTED;
        }
    }

    eOSType osType;
    if (RT_SUCCESS(rc))
    {
        /*
         * Determine guest OS type and the required installer image.
         */
        Utf8Str strOSType;
        rc = getGuestProperty(pGuest, "/VirtualBox/GuestInfo/OS/Product", strOSType);
        if (RT_SUCCESS(rc))
        {
            if (   strOSType.contains("Microsoft", Utf8Str::CaseInsensitive)
                || strOSType.contains("Windows", Utf8Str::CaseInsensitive))
            {
                osType = eOSType_Windows;
            }
            else if (strOSType.contains("Solaris", Utf8Str::CaseInsensitive))
            {
                osType = eOSType_Solaris;
            }
            else /* Everything else hopefully means Linux :-). */
                osType = eOSType_Linux;

#if 1 /* Only Windows is supported (and tested) at the moment. */
            if (osType != eOSType_Windows)
            {
                hr = setProgressErrorMsg(VBOX_E_NOT_SUPPORTED,
                                         Utf8StrFmt(GuestSession::tr("Detected guest OS (%s) does not support automatic Guest Additions updating, please update manually"),
                                                    strOSType.c_str()));
                rc = VERR_NOT_SUPPORTED;
            }
#endif
        }
    }

    RTISOFSFILE iso;
    if (RT_SUCCESS(rc))
    {
        /*
         * Try to open the .ISO file to extract all needed files.
         */
        rc = RTIsoFsOpen(&iso, mSource.c_str());
        if (RT_FAILURE(rc))
        {
            hr = setProgressErrorMsg(VBOX_E_IPRT_ERROR,
                                     Utf8StrFmt(GuestSession::tr("Unable to open Guest Additions .ISO file \"%s\": %Rrc"),
                                     mSource.c_str(), rc));
        }
        else
        {
            /* Set default installation directories. */
            Utf8Str strUpdateDir = "/tmp/";
            if (osType == eOSType_Windows)
                 strUpdateDir = "C:\\Temp\\";

            rc = setProgress(5);

            /* Try looking up the Guest Additions installation directory. */
            if (RT_SUCCESS(rc))
            {
                /* Try getting the installed Guest Additions version to know whether we
                 * can install our temporary Guest Addition data into the original installation
                 * directory.
                 *
                 * Because versions prior to 4.2 had bugs wrt spaces in paths we have to choose
                 * a different location then.
                 */
                bool fUseInstallDir = false;

                Utf8Str strAddsVer;
                rc = getGuestProperty(pGuest, "/VirtualBox/GuestAdd/Version", strAddsVer);
                if (   RT_SUCCESS(rc)
                    && RTStrVersionCompare(strAddsVer.c_str(), "4.2") >= 0)
                {
                    fUseInstallDir = true;
                }

                if (fUseInstallDir)
                {
                    if (RT_SUCCESS(rc))
                        rc = getGuestProperty(pGuest, "/VirtualBox/GuestAdd/InstallDir", strUpdateDir);
                    if (RT_SUCCESS(rc))
                    {
                        if (osType == eOSType_Windows)
                        {
                            strUpdateDir.findReplace('/', '\\');
                            strUpdateDir.append("\\Update\\");
                        }
                        else
                            strUpdateDir.append("/update/");
                    }
                }
            }

            if (RT_SUCCESS(rc))
                LogRel(("Guest Additions update directory is: %s\n",
                        strUpdateDir.c_str()));

            /* Create the installation directory. */
            rc = pSession->directoryCreateInternal(strUpdateDir,
                                                   755 /* Mode */, DirectoryCreateFlag_Parents);
            if (RT_FAILURE(rc))
                hr = setProgressErrorMsg(VBOX_E_IPRT_ERROR,
                                         Utf8StrFmt(GuestSession::tr("Error creating installation directory \"%s\" on the guest: %Rrc"),
                                                    strUpdateDir.c_str(), rc));
            if (RT_SUCCESS(rc))
                rc = setProgress(10);

            if (RT_SUCCESS(rc))
            {
                /* Prepare the file(s) we want to copy over to the guest and
                 * (maybe) want to run. */
                switch (osType)
                {
                    case eOSType_Windows:
                    {
                        /* Do we need to install our certificates? We do this for W2K and up. */
                        bool fInstallCert = false;

                        Utf8Str strOSVer;
                        rc = getGuestProperty(pGuest, "/VirtualBox/GuestInfo/OS/Release", strOSVer);
                        if (   RT_SUCCESS(rc)
                            && RTStrVersionCompare(strOSVer.c_str(), "5.0") >= 0) /* Only W2K an up. */
                        {
                            fInstallCert = true;
                            LogRel(("Certificates for auto updating WHQL drivers will be installed\n"));
                        }
                        else if (RT_FAILURE(rc))
                        {
                            /* Unknown (or unhandled) Windows OS. */
                            fInstallCert = true;
                            LogRel(("Unknown guest Windows version detected (%s), installing certificates for WHQL drivers\n",
                                    strOSVer.c_str()));
                        }
                        else
                            LogRel(("Skipping installation of certificates for WHQL drivers\n"));

                        if (fInstallCert)
                        {
                            /* Our certificate. */
                            mFiles.push_back(InstallerFile("CERT/ORACLE_VBOX.CER",
                                                           strUpdateDir + "oracle-vbox.cer",
                                                           UPDATEFILE_FLAG_COPY_FROM_ISO | UPDATEFILE_FLAG_OPTIONAL));
                            /* Our certificate installation utility. */
                            /* First pass: Copy over the file + execute it to remove any existing
                             *             VBox certificates. */
                            GuestProcessStartupInfo siCertUtilRem;
                            siCertUtilRem.mName = "VirtualBox Certificate Utility, removing old VirtualBox certificates";
                            siCertUtilRem.mArguments.push_back(Utf8Str("remove-trusted-publisher"));
                            siCertUtilRem.mArguments.push_back(Utf8Str("--root")); /* Add root certificate as well. */
                            siCertUtilRem.mArguments.push_back(Utf8Str(strUpdateDir + "oracle-vbox.cer"));
                            siCertUtilRem.mArguments.push_back(Utf8Str(strUpdateDir + "oracle-vbox.cer"));
                            mFiles.push_back(InstallerFile("CERT/VBOXCERTUTIL.EXE",
                                                           strUpdateDir + "VBoxCertUtil.exe",
                                                           UPDATEFILE_FLAG_COPY_FROM_ISO | UPDATEFILE_FLAG_EXECUTE | UPDATEFILE_FLAG_OPTIONAL,
                                                           siCertUtilRem));
                            /* Second pass: Only execute (but don't copy) again, this time installng the
                             *              recent certificates just copied over. */
                            GuestProcessStartupInfo siCertUtilAdd;
                            siCertUtilAdd.mName = "VirtualBox Certificate Utility, installing VirtualBox certificates";
                            siCertUtilAdd.mArguments.push_back(Utf8Str("add-trusted-publisher"));
                            siCertUtilAdd.mArguments.push_back(Utf8Str("--root")); /* Add root certificate as well. */
                            siCertUtilAdd.mArguments.push_back(Utf8Str(strUpdateDir + "oracle-vbox.cer"));
                            siCertUtilAdd.mArguments.push_back(Utf8Str(strUpdateDir + "oracle-vbox.cer"));
                            mFiles.push_back(InstallerFile("CERT/VBOXCERTUTIL.EXE",
                                                           strUpdateDir + "VBoxCertUtil.exe",
                                                           UPDATEFILE_FLAG_EXECUTE | UPDATEFILE_FLAG_OPTIONAL,
                                                           siCertUtilAdd));
                        }
                        /* The installers in different flavors, as we don't know (and can't assume)
                         * the guest's bitness. */
                        mFiles.push_back(InstallerFile("VBOXWINDOWSADDITIONS_X86.EXE",
                                                       strUpdateDir + "VBoxWindowsAdditions-x86.exe",
                                                       UPDATEFILE_FLAG_COPY_FROM_ISO));
                        mFiles.push_back(InstallerFile("VBOXWINDOWSADDITIONS_AMD64.EXE",
                                                       strUpdateDir + "VBoxWindowsAdditions-amd64.exe",
                                                       UPDATEFILE_FLAG_COPY_FROM_ISO));
                        /* The stub loader which decides which flavor to run. */
                        GuestProcessStartupInfo siInstaller;
                        siInstaller.mName = "VirtualBox Windows Guest Additions Installer";
                        siInstaller.mArguments.push_back(Utf8Str("/S")); /* We want to install in silent mode. */
                        siInstaller.mArguments.push_back(Utf8Str("/l")); /* ... and logging enabled. */
                        /* Don't quit VBoxService during upgrade because it still is used for this
                         * piece of code we're in right now (that is, here!) ... */
                        siInstaller.mArguments.push_back(Utf8Str("/no_vboxservice_exit"));
                        /* Tell the installer to report its current installation status
                         * using a running VBoxTray instance via balloon messages in the
                         * Windows taskbar. */
                        siInstaller.mArguments.push_back(Utf8Str("/post_installstatus"));
                        /* If the caller does not want to wait for out guest update process to end,
                         * complete the progress object now so that the caller can do other work. */
                        if (mFlags & AdditionsUpdateFlag_WaitForUpdateStartOnly)
                            siInstaller.mFlags |= ProcessCreateFlag_WaitForProcessStartOnly;
                        mFiles.push_back(InstallerFile("VBOXWINDOWSADDITIONS.EXE",
                                                       strUpdateDir + "VBoxWindowsAdditions.exe",
                                                       UPDATEFILE_FLAG_COPY_FROM_ISO | UPDATEFILE_FLAG_EXECUTE, siInstaller));
                        break;
                    }
                    case eOSType_Linux:
                        /** @todo Add Linux support. */
                        break;
                    case eOSType_Solaris:
                        /** @todo Add Solaris support. */
                        break;
                    default:
                        AssertReleaseMsgFailed(("Unsupported guest type: %d\n", osType));
                        break;
                }
            }

            if (RT_SUCCESS(rc))
            {
                /* We want to spend 40% total for all copying operations. So roughly
                 * calculate the specific percentage step of each copied file. */
                uint8_t uOffset = 20; /* Start at 20%. */
                uint8_t uStep = 40 / mFiles.size();

                LogRel(("Copying over Guest Additions update files to the guest ...\n"));

                std::vector<InstallerFile>::const_iterator itFiles = mFiles.begin();
                while (itFiles != mFiles.end())
                {
                    if (itFiles->fFlags & UPDATEFILE_FLAG_COPY_FROM_ISO)
                    {
                        bool fOptional = false;
                        if (itFiles->fFlags & UPDATEFILE_FLAG_OPTIONAL)
                            fOptional = true;
                        rc = copyFileToGuest(pSession, &iso, itFiles->strSource, itFiles->strDest,
                                             fOptional, NULL /* cbSize */);
                        if (RT_FAILURE(rc))
                        {
                            hr = setProgressErrorMsg(VBOX_E_IPRT_ERROR,
                                                      Utf8StrFmt(GuestSession::tr("Error while copying file \"%s\" to \"%s\" on the guest: %Rrc"),
                                                                 itFiles->strSource.c_str(), itFiles->strDest.c_str(), rc));
                            break;
                        }
                    }

                    rc = setProgress(uOffset);
                    if (RT_FAILURE(rc))
                        break;
                    uOffset += uStep;

                    itFiles++;
                }
            }

            /* Done copying, close .ISO file. */
            RTIsoFsClose(&iso);

            if (RT_SUCCESS(rc))
            {
                /* We want to spend 35% total for all copying operations. So roughly
                 * calculate the specific percentage step of each copied file. */
                uint8_t uOffset = 60; /* Start at 60%. */
                uint8_t uStep = 35 / mFiles.size();

                LogRel(("Executing Guest Additions update files ...\n"));

                std::vector<InstallerFile>::iterator itFiles = mFiles.begin();
                while (itFiles != mFiles.end())
                {
                    if (itFiles->fFlags & UPDATEFILE_FLAG_EXECUTE)
                    {
                        rc = runFileOnGuest(pSession, itFiles->mProcInfo);
                        if (RT_FAILURE(rc))
                        {
                            hr = setProgressErrorMsg(VBOX_E_IPRT_ERROR,
                                                      Utf8StrFmt(GuestSession::tr("Error while running installer file \"%s\" on the guest: %Rrc"),
                                                                 itFiles->strDest.c_str(), rc));
                            break;
                        }
                    }

                    rc = setProgress(uOffset);
                    if (RT_FAILURE(rc))
                        break;
                    uOffset += uStep;

                    itFiles++;
                }
            }

            if (RT_SUCCESS(rc))
            {
                LogRel(("Automatic update of Guest Additions succeeded\n"));
                rc = setProgressSuccess();
            }
        }
    }

    if (RT_FAILURE(rc))
    {
        if (rc == VERR_CANCELLED)
        {
            LogRel(("Automatic update of Guest Additions was canceled\n"));

            hr = setProgressErrorMsg(VBOX_E_IPRT_ERROR,
                                     Utf8StrFmt(GuestSession::tr("Installation was canceled")));
        }
        else
        {
            Utf8Str strError = Utf8StrFmt("No further error information available (%Rrc)", rc);
            if (!mProgress.isNull()) /* Progress object is optional. */
            {
                ComPtr<IVirtualBoxErrorInfo> pError;
                hr = mProgress->COMGETTER(ErrorInfo)(pError.asOutParam());
                Assert(!pError.isNull());
                if (SUCCEEDED(hr))
                {
                    Bstr strVal;
                    hr = pError->COMGETTER(Text)(strVal.asOutParam());
                    if (   SUCCEEDED(hr)
                        && strVal.isNotEmpty())
                        strError = strVal;
                }
            }

            LogRel(("Automatic update of Guest Additions failed: %s\n", strError.c_str()));
        }

        LogRel(("Please install Guest Additions manually\n"));
    }

    LogFlowFuncLeaveRC(rc);
    return rc;
}

int SessionTaskUpdateAdditions::RunAsync(const Utf8Str &strDesc, ComObjPtr<Progress> &pProgress)
{
    LogFlowThisFunc(("strDesc=%s, strSource=%s, uFlags=%x\n",
                     strDesc.c_str(), mSource.c_str(), mFlags));

    mDesc = strDesc;
    mProgress = pProgress;

    int rc = RTThreadCreate(NULL, SessionTaskUpdateAdditions::taskThread, this,
                            0, RTTHREADTYPE_MAIN_HEAVY_WORKER, 0,
                            "gctlUpGA");
    LogFlowFuncLeaveRC(rc);
    return rc;
}

/* static */
int SessionTaskUpdateAdditions::taskThread(RTTHREAD Thread, void *pvUser)
{
    std::auto_ptr<SessionTaskUpdateAdditions> task(static_cast<SessionTaskUpdateAdditions*>(pvUser));
    AssertReturn(task.get(), VERR_GENERAL_FAILURE);

    LogFlowFunc(("pTask=%p\n", task.get()));
    return task->Run();
}

