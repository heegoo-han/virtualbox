/* $Id$ */
/** @file
 * VBoxDnD.cpp - Windows-specific bits of the drag'n drop service.
 */

/*
 * Copyright (C) 2013-2014 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */
#include <windows.h>
#include "VBoxTray.h"
#include "VBoxHelpers.h"
#include "VBoxDnD.h"

#include <VBox/VBoxGuestLib.h>
#include "VBox/HostServices/DragAndDropSvc.h"

#include <iprt/asm.h>
#include <iprt/assert.h>
#include <iprt/err.h>
#include <iprt/ldr.h>
#include <iprt/list.h>
#include <iprt/mem.h>

#include <iprt/cpp/mtlist.h>
#include <iprt/cpp/ministring.h>

#include <iprt/cpp/mtlist.h>

#include <VBoxGuestInternal.h>

/* Enable this define to see the proxy window(s) when debugging
 * their behavior. Don't have this enabled in release builds! */
#ifdef DEBUG
//# define VBOX_DND_DEBUG_WND
#endif

/** @todo Merge this with messages from VBoxTray.h. */
#define WM_VBOXTRAY_DND_MESSAGE       WM_APP + 401

static LRESULT CALLBACK vboxDnDWndProcInstance(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
static LRESULT CALLBACK vboxDnDWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

VBoxDnDWnd::VBoxDnDWnd(void)
    : hWnd(NULL),
      mfMouseButtonDown(false),
#ifdef VBOX_WITH_DRAG_AND_DROP_GH
      pDropTarget(NULL),
#endif
      mClientID(UINT32_MAX),
      mMode(Unknown),
      mState(Uninitialized)
{
    RT_ZERO(startupInfo);

    reset();
}

VBoxDnDWnd::~VBoxDnDWnd(void)
{
    /** @todo Shutdown crit sect / event etc! */

    reset();
}

int VBoxDnDWnd::Initialize(PVBOXDNDCONTEXT pContext)
{
    AssertPtrReturn(pContext, VERR_INVALID_POINTER);

    /* Save the context. */
    this->pContext = pContext;

    int rc = RTSemEventCreate(&mEventSem);
    if (RT_SUCCESS(rc))
        rc = RTCritSectInit(&mCritSect);

    if (RT_SUCCESS(rc))
    {
        /* Message pump thread for our proxy window. */
        rc = RTThreadCreate(&gCtx.hEvtQueue, VBoxDnDWnd::Thread, this,
                            0, RTTHREADTYPE_MSG_PUMP, RTTHREADFLAGS_WAITABLE,
                            "VBoxTrayDnDWnd");
        if (RT_FAILURE(rc))
            LogRel(("DnD: Failed to start proxy window thread, rc=%Rrc\n", rc));
        /** @todo Wait for thread to be started! */
    }

    LogFlowThisFunc(("Returning rc=%Rrc\n", rc));
    return rc;
}

/**
 * Thread for handling the window's message pump.
 *
 * @return  IPRT status code.
 * @param   hThread
 * @param   pvUser
 */
/* static */
int VBoxDnDWnd::Thread(RTTHREAD hThread, void *pvUser)
{
    AssertPtrReturn(pvUser, VERR_INVALID_POINTER);

    VBoxDnDWnd *pThis = (VBoxDnDWnd*)pvUser;
    AssertPtr(pThis);

    PVBOXDNDCONTEXT pContext = pThis->pContext;
    AssertPtr(pContext);
    AssertPtr(pContext->pEnv);

    HINSTANCE hInstance = pContext->pEnv->hInstance;
    Assert(hInstance != 0);

    /* Create our proxy window. */
    WNDCLASSEX wndClass;
    RT_ZERO(wndClass);

    wndClass.cbSize        = sizeof(WNDCLASSEX);
    wndClass.lpfnWndProc   = vboxDnDWndProc;
    wndClass.lpszClassName = "VBoxTrayDnDWnd";
    wndClass.hInstance     = hInstance;
    wndClass.style         = CS_NOCLOSE;
#ifdef VBOX_DND_DEBUG_WND
    wndClass.style        |= CS_HREDRAW | CS_VREDRAW;
    wndClass.hbrBackground = (HBRUSH)(CreateSolidBrush(RGB(255, 0, 0)));
#else
    wndClass.hbrBackground = (HBRUSH)(COLOR_BACKGROUND + 1);
#endif

    int rc = VINF_SUCCESS;
    if (!RegisterClassEx(&wndClass))
    {
        DWORD dwErr = GetLastError();
        LogFlowFunc(("Unable to register proxy window class, error=%ld\n", dwErr));
        rc = RTErrConvertFromWin32(dwErr);
    }

    if (RT_SUCCESS(rc))
    {
        DWORD dwExStyle = WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE;
        DWORD dwStyle = WS_POPUP;
#ifdef VBOX_DND_DEBUG_WND
        dwExStyle &= ~WS_EX_TRANSPARENT; /* Remove transparency bit. */
        dwStyle |= WS_VISIBLE; /* Make the window visible. */
#endif
        pThis->hWnd =
            CreateWindowEx(dwExStyle,
                           "VBoxTrayDnDWnd", "VBoxTrayDnDWnd",
                           dwStyle,
#ifdef VBOX_DND_DEBUG_WND
                           CW_USEDEFAULT, CW_USEDEFAULT, 200, 200, NULL, NULL,
#else
                           -200, -200, 100, 100, NULL, NULL,
#endif
                           hInstance, pThis /* lParm */);
        if (!pThis->hWnd)
        {
            DWORD dwErr = GetLastError();
            LogFlowFunc(("Unable to create proxy window, error=%ld\n", dwErr));
            rc = RTErrConvertFromWin32(dwErr);
        }
        else
        {
#ifndef VBOX_DND_DEBUG_WND
            SetWindowPos(pThis->hWnd, HWND_TOPMOST, -200, -200, 0, 0,
                           SWP_NOACTIVATE | SWP_HIDEWINDOW
                         | SWP_NOCOPYBITS | SWP_NOREDRAW | SWP_NOSIZE);
            LogFlowFunc(("Proxy window created, hWnd=0x%x\n", pThis->hWnd));
#else
            LogFlowFunc(("Debug proxy window created, hWnd=0x%x\n", pThis->hWnd));

            /*
             * Install some mouse tracking.
             */
            TRACKMOUSEEVENT me;
            RT_ZERO(me);
            me.cbSize    = sizeof(TRACKMOUSEEVENT);
            me.dwFlags   = TME_HOVER | TME_LEAVE | TME_NONCLIENT;
            me.hwndTrack = pThis->hWnd;
            BOOL fRc = TrackMouseEvent(&me);
            Assert(fRc);
#endif
        }
    }

    if (RT_SUCCESS(rc))
    {
        OleInitialize(NULL);

        pThis->RegisterAsDropTarget();

        bool fShutdown = false;

        do
        {
            MSG uMsg;
            while (GetMessage(&uMsg, 0, 0, 0))
            {
                TranslateMessage(&uMsg);
                DispatchMessage(&uMsg);
            }

            if (ASMAtomicReadBool(&pContext->fShutdown))
                fShutdown = true;

            if (fShutdown)
            {
                LogFlowFunc(("Cancelling ...\n"));
                break;
            }

            /** @todo Immediately drop on failure? */

        } while (RT_SUCCESS(rc));

        OleUninitialize();
    }

    LogFlowFuncLeaveRC(rc);
    return rc;
}

/* static */
BOOL CALLBACK VBoxDnDWnd::MonitorEnumProc(HMONITOR hMonitor, HDC hdcMonitor,
                                          LPRECT lprcMonitor, LPARAM lParam)
{
    LPRECT pRect = (LPRECT)lParam;
    AssertPtrReturn(pRect, FALSE);

    AssertPtr(lprcMonitor);
    LogFlowFunc(("Monitor is %ld,%ld,%ld,%ld\n",
                 lprcMonitor->left, lprcMonitor->top,
                 lprcMonitor->right, lprcMonitor->bottom));

    /* Build up a simple bounding box to hold the entire (virtual) screen. */
    if (pRect->left > lprcMonitor->left)
        pRect->left = lprcMonitor->left;
    if (pRect->right < lprcMonitor->right)
        pRect->right = lprcMonitor->right;
    if (pRect->top > lprcMonitor->top)
        pRect->top = lprcMonitor->top;
    if (pRect->bottom < lprcMonitor->bottom)
        pRect->bottom = lprcMonitor->bottom;

    return TRUE;
}

LRESULT CALLBACK VBoxDnDWnd::WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
        case WM_CREATE:
        {
            int rc = OnCreate();
            if (RT_FAILURE(rc))
                return FALSE;
            return TRUE;
        }

        case WM_CLOSE:
        {
            OnDestroy();

            DestroyWindow(hWnd);
            PostQuitMessage(0);
            return 0;
        }

        case WM_LBUTTONDOWN:
            LogFlowThisFunc(("WM_LBUTTONDOWN\n"));
            mfMouseButtonDown = true;
            return 0;

        case WM_LBUTTONUP:
            LogFlowThisFunc(("WM_LBUTTONUP\n"));
            mfMouseButtonDown = false;
            return 0;

        case WM_MOUSELEAVE:
            LogFlowThisFunc(("WM_MOUSELEAVE\n"));
            return 0;

        /* Will only be called once; after the first mouse move, this
         * window will be hidden! */
        case WM_MOUSEMOVE:
        {
            LogFlowThisFunc(("WM_MOUSEMOVE: mfMouseButtonDown=%RTbool, mMode=%ld, mState=%ld\n",
                             mfMouseButtonDown, mMode, mState));
#ifdef DEBUG_andy
            POINT p;
            GetCursorPos(&p);
            LogFlowThisFunc(("WM_MOUSEMOVE: curX=%ld, curY=%ld\n", p.x, p.y));
#endif
            int rc = VINF_SUCCESS;
            if (mMode == HG) /* Host to guest. */
            {
                /* Dragging not started yet? Kick it off ... */
                if (   mfMouseButtonDown
                    && (mState != Dragging))
                {
                    mState = Dragging;
#if 0
                    /* Delay hiding the proxy window a bit when debugging, to see
                     * whether the desired range is covered correctly. */
                    RTThreadSleep(5000);
#endif
                    hide();

                    LogFlowThisFunc(("Starting drag'n drop: uAllActions=0x%x, dwOKEffects=0x%x ...\n",
                                     uAllActions, startupInfo.dwOKEffects));

                    AssertPtr(startupInfo.pDataObject);
                    AssertPtr(startupInfo.pDropSource);
                    DWORD dwEffect;
                    HRESULT hr = DoDragDrop(startupInfo.pDataObject, startupInfo.pDropSource,
                                            startupInfo.dwOKEffects, &dwEffect);
                    LogFlowThisFunc(("rc=%Rhrc, dwEffect=%RI32\n", hr, dwEffect));
                    switch (hr)
                    {
                        case DRAGDROP_S_DROP:
                            mState = Dropped;
                            break;

                        case DRAGDROP_S_CANCEL:
                            mState = Canceled;
                            break;

                        default:
                            LogFlowThisFunc(("Drag'n drop failed with %Rhrc\n", hr));
                            mState = Canceled;
                            rc = VERR_GENERAL_FAILURE; /** @todo Find a better status code. */
                            break;
                    }

                    int rc2 = RTCritSectEnter(&mCritSect);
                    if (RT_SUCCESS(rc2))
                    {
                        startupInfo.pDropSource->Release();
                        startupInfo.pDataObject->Release();

                        RT_ZERO(startupInfo);

                        rc2 = RTCritSectLeave(&mCritSect);
                        if (RT_SUCCESS(rc))
                            rc = rc2;
                    }

                    mMode = Unknown;
                }
            }
            else if (mMode == GH) /* Guest to host. */
            {
                //hide();

                /* Starting here VBoxDnDDropTarget should
                 * take over; was instantiated when registering
                 * this proxy window as a (valid) drop target. */
            }
            else
                rc = VERR_NOT_SUPPORTED;

            LogFlowThisFunc(("WM_MOUSEMOVE: mMode=%ld, mState=%ld, rc=%Rrc\n",
                             mMode, mState, rc));
            return 0;
        }

        case WM_NCMOUSEHOVER:
            LogFlowThisFunc(("WM_NCMOUSEHOVER\n"));
            return 0;

        case WM_NCMOUSELEAVE:
            LogFlowThisFunc(("WM_NCMOUSELEAVE\n"));
            return 0;

        case WM_VBOXTRAY_DND_MESSAGE:
        {
            VBOXDNDEVENT *pEvent = (PVBOXDNDEVENT)lParam;
            AssertPtr(pEvent);

            LogFlowThisFunc(("Received uType=%RU32, uScreenID=%RU32\n",
                         pEvent->Event.uType, pEvent->Event.uScreenId));

            int rc;
            switch (pEvent->Event.uType)
            {
                case DragAndDropSvc::HOST_DND_HG_EVT_ENTER:
                {
                    LogFlowThisFunc(("HOST_DND_HG_EVT_ENTER\n"));

                    reset();

                    Assert(mMode == Unknown);
                    mMode = HG;

                    if (pEvent->Event.cbFormats)
                    {
                        RTCList<RTCString> lstFormats =
                            RTCString(pEvent->Event.pszFormats, pEvent->Event.cbFormats - 1).split("\r\n");
                        rc = OnHgEnter(lstFormats, pEvent->Event.u.a.uAllActions);
                    }
                    else
                    {
                        AssertMsgFailed(("cbFormats is 0\n"));
                        rc = VERR_INVALID_PARAMETER;
                    }

                    /* Note: After HOST_DND_HG_EVT_ENTER there immediately is a move
                     *       event, so fall through is intentional here. */
                }

                case DragAndDropSvc::HOST_DND_HG_EVT_MOVE:
                {
                    LogFlowThisFunc(("HOST_DND_HG_EVT_MOVE: %d,%d\n",
                                     pEvent->Event.u.a.uXpos, pEvent->Event.u.a.uYpos));

                    rc = OnHgMove(pEvent->Event.u.a.uXpos, pEvent->Event.u.a.uYpos,
                                  pEvent->Event.u.a.uDefAction);
                    break;
                }

                case DragAndDropSvc::HOST_DND_HG_EVT_LEAVE:
                {
                    LogFlowThisFunc(("HOST_DND_HG_EVT_LEAVE\n"));

                    rc = OnHgLeave();
                    break;
                }

                case DragAndDropSvc::HOST_DND_HG_EVT_DROPPED:
                {
                    LogFlowThisFunc(("HOST_DND_HG_EVT_DROPPED\n"));

                    rc = OnHgDrop();
                    break;
                }

                case DragAndDropSvc::HOST_DND_HG_SND_DATA:
                {
                    LogFlowThisFunc(("HOST_DND_HG_SND_DATA\n"));

                    rc = OnHgDataReceived(pEvent->Event.u.b.pvData,
                                          pEvent->Event.u.b.cbData);
                    break;
                }

                case DragAndDropSvc::HOST_DND_HG_EVT_CANCEL:
                {
                    LogFlowThisFunc(("HOST_DND_HG_EVT_CANCEL\n"));

                    rc = OnHgCancel();

                    reset();
                    break;
                }

                case DragAndDropSvc::HOST_DND_GH_REQ_PENDING:
                {
                    LogFlowThisFunc(("HOST_DND_GH_REQ_PENDING\n"));
#ifdef VBOX_WITH_DRAG_AND_DROP_GH
                    Assert(   mMode == Unknown
                           || mMode == GH);
                    mMode = GH;
                    rc = OnGhIsDnDPending(pEvent->Event.uScreenId);
#else
                    rc = VERR_NOT_SUPPORTED;
#endif
                    break;
                }

                case DragAndDropSvc::HOST_DND_GH_EVT_DROPPED:
                {
                    LogFlowThisFunc(("HOST_DND_GH_EVT_DROPPED\n"));
#ifdef VBOX_WITH_DRAG_AND_DROP_GH
                    Assert(mMode == GH);
                    rc = OnGhDropped(pEvent->Event.pszFormats,
                                     pEvent->Event.cbFormats,
                                     pEvent->Event.u.a.uDefAction);
                    mMode = Unknown;
#else
                    rc = VERR_NOT_SUPPORTED;
#endif
                    break;
                }

                case DragAndDropSvc::GUEST_DND_GH_EVT_ERROR:
                {
                    LogFlowThisFunc(("GUEST_DND_GH_EVT_ERROR\n"));
#ifdef VBOX_WITH_DRAG_AND_DROP_GH
                    reset();
                    rc = VINF_SUCCESS; /** @todo GUEST_DND_GH_EVT_ERROR */
#else
                    rc = VERR_NOT_SUPPORTED;
#endif
                    break;
                }

                default:
                    rc = VERR_NOT_SUPPORTED;
                    break;
            }

            /* Some messages require cleanup. */
            switch (pEvent->Event.uType)
            {
                case DragAndDropSvc::HOST_DND_HG_EVT_ENTER:
                case DragAndDropSvc::HOST_DND_HG_EVT_MOVE:
                case DragAndDropSvc::HOST_DND_HG_EVT_DROPPED:
#ifdef VBOX_WITH_DRAG_AND_DROP_GH
                case DragAndDropSvc::HOST_DND_GH_EVT_DROPPED:
#endif
                {
                    if (pEvent->Event.pszFormats)
                        RTMemFree(pEvent->Event.pszFormats);
                    break;
                }

                case DragAndDropSvc::HOST_DND_HG_SND_DATA:
                {
                    if (pEvent->Event.pszFormats)
                        RTMemFree(pEvent->Event.pszFormats);
                    if (pEvent->Event.u.b.pvData)
                        RTMemFree(pEvent->Event.u.b.pvData);
                    break;
                }

                default:
                    /* Ignore. */
                    break;
            }

            LogFlowThisFunc(("Processing event %RU32 resulted in rc=%Rrc\n",
                             pEvent->Event.uType, rc));
            if (pEvent)
                RTMemFree(pEvent);
            return 0;
        }

        default:
            break;
    }

    return DefWindowProc(hWnd, uMsg, wParam, lParam);
}

#ifdef VBOX_WITH_DRAG_AND_DROP_GH
/**
 * Registers this proxy window as a local drop target.
 *
 * @return  IPRT status code.
 */
int VBoxDnDWnd::RegisterAsDropTarget(void)
{
    if (pDropTarget) /* Already registered as drop target? */
        return VINF_SUCCESS;

    int rc;
    try
    {
        pDropTarget = new VBoxDnDDropTarget(this /* pParent */);
        AssertPtr(pDropTarget);
        HRESULT hr = CoLockObjectExternal(pDropTarget, TRUE /* fLock */,
                                          FALSE /* fLastUnlockReleases */);
        if (SUCCEEDED(hr))
            hr = RegisterDragDrop(hWnd, pDropTarget);

        if (FAILED(hr))
        {
            LogRel(("DnD: Creating drop target failed with hr=%Rhrc\n", hr));
            rc = VERR_GENERAL_FAILURE; /** @todo Find a better rc. */
        }
        else
            rc = VINF_SUCCESS;
    }
    catch (std::bad_alloc)
    {
        rc = VERR_NO_MEMORY;
    }

    LogFlowFuncLeaveRC(rc);
    return rc;
}

int VBoxDnDWnd::UnregisterAsDropTarget(void)
{
    if (!pDropTarget) /* No drop target? Bail out. */
        return VINF_SUCCESS;

    HRESULT hr = RevokeDragDrop(hWnd);
    if (SUCCEEDED(hr))
        hr = CoLockObjectExternal(pDropTarget, FALSE /* fLock */,
                                  TRUE /* fLastUnlockReleases */);
    if (SUCCEEDED(hr))
        pDropTarget->Release();

    int rc = SUCCEEDED(hr)
           ? VINF_SUCCESS : VERR_GENERAL_FAILURE; /** @todo Fix this. */

    LogFlowFuncLeaveRC(rc);
    return rc;
}
#endif /* VBOX_WITH_DRAG_AND_DROP_GH */

int VBoxDnDWnd::OnCreate(void)
{
    int rc = VbglR3DnDConnect(&mClientID);
    if (RT_FAILURE(rc))
    {
        LogFlowThisFunc(("Connection to host service failed, rc=%Rrc\n", rc));
        return rc;
    }

    LogFlowThisFunc(("Client ID=%RU32, rc=%Rrc\n", mClientID, rc));
    return rc;
}

void VBoxDnDWnd::OnDestroy(void)
{
    VbglR3DnDDisconnect(mClientID);
    LogFlowThisFuncLeave();
}

int VBoxDnDWnd::OnHgEnter(const RTCList<RTCString> &lstFormats, uint32_t uAllActions)
{
#ifdef DEBUG
    LogFlowThisFunc(("uActions=0x%x, lstFormats=%zu: ", uAllActions, lstFormats.size()));
    for (size_t i = 0; i < lstFormats.size(); i++)
        LogFlow(("'%s' ", lstFormats.at(i).c_str()));
    LogFlow(("\n"));
#endif

    /* Save all allowed actions. */
    this->uAllActions = uAllActions;

    /*
     * Install our allowed MIME types.
     ** @todo See todo for m_sstrAllowedMimeTypes in GuestDnDImpl.cpp.
     */
    const RTCList<RTCString> lstAllowedMimeTypes = RTCList<RTCString>()
        /* Uri's */
        << "text/uri-list"
        /* Text */
        << "text/plain;charset=utf-8"
        << "UTF8_STRING"
        << "text/plain"
        << "COMPOUND_TEXT"
        << "TEXT"
        << "STRING"
        /* OpenOffice formates */
        << "application/x-openoffice-embed-source-xml;windows_formatname=\"Star Embed Source (XML)\""
        << "application/x-openoffice-drawing;windows_formatname=\"Drawing Format\"";
    this->lstAllowedFormats = lstAllowedMimeTypes;

    /*
     * Check MIME compatibility with this client.
     */
    LogFlowThisFunc(("Supported MIME types:\n"));
    for (size_t i = 0; i < lstFormats.size(); i++)
    {
        bool fSupported = lstAllowedFormats.contains(lstFormats.at(i));
        if (fSupported)
            this->lstFormats.append(lstFormats.at(i));
        LogFlowThisFunc(("\t%s: %RTbool\n", lstFormats.at(i).c_str(), fSupported));
    }

    /*
     * Prepare the startup info for DoDragDrop().
     */
    int rc = VINF_SUCCESS;
    try
    {
        /* Translate our drop actions into
         * allowed Windows drop effects. */
        startupInfo.dwOKEffects = DROPEFFECT_NONE;
        if (uAllActions)
        {
            if (uAllActions & DND_COPY_ACTION)
                startupInfo.dwOKEffects |= DROPEFFECT_COPY;
            if (uAllActions & DND_MOVE_ACTION)
                startupInfo.dwOKEffects |= DROPEFFECT_MOVE;
            if (uAllActions & DND_LINK_ACTION)
                startupInfo.dwOKEffects |= DROPEFFECT_LINK;
        }

        startupInfo.pDropSource = new VBoxDnDDropSource(this);
        startupInfo.pDataObject = new VBoxDnDDataObject();
    }
    catch (std::bad_alloc)
    {
        rc = VERR_NO_MEMORY;
    }

    if (RT_SUCCESS(rc))
        rc = makeFullscreen();

    LogFlowFuncLeaveRC(rc);
    return rc;
}

int VBoxDnDWnd::OnHgMove(uint32_t u32xPos, uint32_t u32yPos, uint32_t uAction)
{
    LogFlowThisFunc(("u32xPos=%RU32, u32yPos=%RU32, uAction=0x%x\n",
                     u32xPos, u32yPos, uAction));

    /** @todo Put this block into a function! */
    /** @todo Multi-monitor setups? */
    int iScreenX = GetSystemMetrics(SM_CXSCREEN) - 1;
    int iScreenY = GetSystemMetrics(SM_CYSCREEN) - 1;

    INPUT Input[1] = { 0 };
    Input[0].type       = INPUT_MOUSE;
    Input[0].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_LEFTDOWN | MOUSEEVENTF_ABSOLUTE;
    Input[0].mi.dx      = u32xPos * (65535 / iScreenX);
    Input[0].mi.dy      = u32yPos * (65535 / iScreenY);
    SendInput(1, Input, sizeof(INPUT));

#ifdef DEBUG_andy
    POINT p;
    GetCursorPos(&p);
    LogFlowThisFunc(("curX=%d, curY=%d\n", p.x, p.y));
#endif

    uint32_t uActionNotify = DND_IGNORE_ACTION;
    int rc = RTCritSectEnter(&mCritSect);
    if (RT_SUCCESS(rc))
    {
        if (   (Dragging == mState)
            && startupInfo.pDropSource)
            uActionNotify = startupInfo.pDropSource->GetCurrentAction();

        RTCritSectLeave(&mCritSect);
    }

    if (RT_SUCCESS(rc))
    {
        rc = VbglR3DnDHGAcknowledgeOperation(mClientID, uActionNotify);
        if (RT_FAILURE(rc))
            LogFlowThisFunc(("Acknowledging operation failed with rc=%Rrc\n", rc));
    }

    LogFlowThisFunc(("Returning uActionNotify=0x%x, rc=%Rrc\n", uActionNotify, rc));
    return rc;
}

int VBoxDnDWnd::OnHgLeave(void)
{
    LogFlowThisFunc(("mMode=%ld, mState=%RU32\n", mMode, mState));
    LogRel(("DnD: Drag'n drop operation aborted\n"));

    reset();

    int rc = VINF_SUCCESS;

    /* Post ESC to our window to officially abort the
     * drag'n drop operation. */
    PostMessage(hWnd, WM_KEYDOWN, VK_ESCAPE, 0 /* lParam */);

    LogFlowFuncLeaveRC(rc);
    return rc;
}

int VBoxDnDWnd::OnHgDrop(void)
{
    LogFlowThisFunc(("mMode=%ld, mState=%RU32\n", mMode, mState));

    int rc = VINF_SUCCESS;
    if (mState == Dragging)
    {
        Assert(lstFormats.size() >= 1);

        /** @todo What to do when multiple formats are available? */
        mFormatRequested = lstFormats.at(0);

        rc = RTCritSectEnter(&mCritSect);
        if (RT_SUCCESS(rc))
        {
            if (startupInfo.pDataObject)
                startupInfo.pDataObject->SetStatus(VBoxDnDDataObject::Dropping);
            else
                rc = VERR_NOT_FOUND;

            RTCritSectLeave(&mCritSect);
        }

        if (RT_SUCCESS(rc))
        {
            LogRel(("DnD: Requesting data as '%s' ...\n", mFormatRequested.c_str()));
            rc = VbglR3DnDHGRequestData(mClientID, mFormatRequested.c_str());
            if (RT_FAILURE(rc))
                LogFlowThisFunc(("Requesting data failed with rc=%Rrc\n", rc));
        }
    }

    LogFlowFuncLeaveRC(rc);
    return rc;
}

int VBoxDnDWnd::OnHgDataReceived(const void *pvData, uint32_t cbData)
{
    LogFlowThisFunc(("mState=%ld, pvData=%p, cbData=%RU32\n",
                     mState, pvData, cbData));

    mState = Dropped;

    int rc = VINF_SUCCESS;
    if (pvData)
    {
        Assert(cbData);
        rc = RTCritSectEnter(&mCritSect);
        if (RT_SUCCESS(rc))
        {
            if (startupInfo.pDataObject)
                rc = startupInfo.pDataObject->Signal(mFormatRequested, pvData, cbData);
            else
                rc = VERR_NOT_FOUND;

            RTCritSectLeave(&mCritSect);
        }
    }

    int rc2 = dragRelease();
    if (RT_SUCCESS(rc))
        rc = rc2;

    LogFlowFuncLeaveRC(rc);
    return rc;
}

int VBoxDnDWnd::OnHgCancel(void)
{
    int rc = RTCritSectEnter(&mCritSect);
    if (RT_SUCCESS(rc))
    {
        if (startupInfo.pDataObject)
            startupInfo.pDataObject->Abort();

        RTCritSectLeave(&mCritSect);
    }

    int rc2 = dragRelease();
    if (RT_SUCCESS(rc))
        rc = rc2;

    return rc;
}

#ifdef VBOX_WITH_DRAG_AND_DROP_GH
int VBoxDnDWnd::OnGhIsDnDPending(uint32_t uScreenID)
{
    LogFlowThisFunc(("mMode=%ld, mState=%ld, uScreenID=%RU32\n",
                     mMode, mState, uScreenID));

    if (mState == Uninitialized)
        reset();

    int rc;
    if (mState == Initialized)
    {
        rc = makeFullscreen();
        /*if (RT_SUCCESS(rc))
            rc = RegisterAsDropTarget();*/

        if (RT_SUCCESS(rc))
        {
            /*
             * We have to release the left mouse button to
             * get into our (invisible) proxy window.
             */
            dragRelease();

            /*
             * Even if we just released the left mouse button
             * we're still in the dragging state to handle our
             * own drop target (for the host).
             */
            mState = Dragging;
        }
    }
    else
        rc = VINF_SUCCESS;

    /**
     * Some notes regarding guest cursor movement:
     * - The host only sends an HOST_DND_GH_REQ_PENDING message to the guest
     *   if the mouse cursor is outside the VM's window.
     * - The guest does not know anything about the host's cursor
     *   position / state due to security reasons.
     * - The guest *only* knows that the host currently is asking whether a
     *   guest DnD operation is in progress.
     */

    if (   RT_SUCCESS(rc)
        && (mState == Dragging))
    {
        /** @todo Put this block into a function! */
        POINT p;
        GetCursorPos(&p);
        ClientToScreen(hWnd, &p);
#ifdef DEBUG_andy
        LogFlowThisFunc(("Client to screen curX=%ld, curY=%ld\n", p.x, p.y));
#endif

#if 1
        /** @todo Multi-monitor setups? */
        int iScreenX = GetSystemMetrics(SM_CXSCREEN) - 1;
        int iScreenY = GetSystemMetrics(SM_CYSCREEN) - 1;

        static LONG px = p.x;
        if (px <= 0)
            px = 1;
        static LONG py = p.y;
        if (py <= 0)
            py = 1;
        //px++; py++;
        LogFlowThisFunc(("px=%ld, py=%ld\n", px, py));

        INPUT Input[1] = { 0 };
        Input[0].type       = INPUT_MOUSE;
        Input[0].mi.dwFlags = MOUSEEVENTF_MOVE | /*MOUSEEVENTF_LEFTDOWN |*/ MOUSEEVENTF_ABSOLUTE;
        Input[0].mi.dx      = px * (65535 / iScreenX);
        Input[0].mi.dy      = py * (65535 / iScreenY);
        UINT uiProcessed = SendInput(1, Input, sizeof(INPUT));
        if (uiProcessed)
        {
#ifdef DEBUG_andy
            LogFlowFunc(("Sent %RU16 mouse input(s), x=%ld, y=%ld, flags=0x%x\n",
                         uiProcessed, Input[0].mi.dx, Input[0].mi.dy, Input[0].mi.dwFlags));
#endif
        }
        else
            LogFlowFunc(("Unable to send input, error=0x%x\n", GetLastError()));
#else
        SetCursorPos(p.x, p.y);
#endif

#ifdef DEBUG_andy
        CURSORINFO ci;
        RT_ZERO(ci);
        ci.cbSize = sizeof(ci);
        BOOL fRc = GetCursorInfo(&ci);
        if (fRc)
            LogFlowThisFunc(("Cursor shown=%RTbool, cursor=0x%p, x=%d, y=%d\n",
                             (ci.flags & CURSOR_SHOWING) ? true : false,
                             ci.hCursor, ci.ptScreenPos.x, ci.ptScreenPos.y));
#endif
    }

    if (RT_SUCCESS(rc))
    {
        uint32_t uDefAction = DND_IGNORE_ACTION;
        RTCString strFormat = "unknown";
        if (   pDropTarget
            && pDropTarget->HasData())
        {
            uDefAction = DND_COPY_ACTION;
            uAllActions = uDefAction;
            strFormat = "text/plain;charset=utf-8";
        }

        LogFlowFunc(("Acknowledging pDropTarget=0x%p, uDefAction=0x%x, uAllActions=0x%x, strFormat=%s\n",
                     pDropTarget, uDefAction, uAllActions, strFormat.c_str()));
        rc = VbglR3DnDGHAcknowledgePending(mClientID,
                                           uDefAction, uAllActions, strFormat.c_str());
    }

    LogFlowFuncLeaveRC(rc);
    return rc;
}

int VBoxDnDWnd::OnGhDropped(const char *pszFormats, uint32_t cbFormats,
                            uint32_t uDefAction)
{
    LogFlowThisFunc(("mMode=%ld, mState=%ld, cbFormats=%RU32, uDefAction=0x%x\n",
                     mMode, mState, cbFormats, uDefAction));
    int rc;
    if (mState == Dragging)
    {
        AssertPtr(pDropTarget);
    }
    else
        rc = VERR_WRONG_ORDER;

    LogFlowFuncLeaveRC(rc);
    return rc;
}
#endif /* VBOX_WITH_DRAG_AND_DROP_GH */

int VBoxDnDWnd::ProcessEvent(PVBOXDNDEVENT pEvent)
{
    AssertPtrReturn(pEvent, VERR_INVALID_POINTER);

    PostMessage(hWnd, WM_VBOXTRAY_DND_MESSAGE,
                0 /* wParm */, (LPARAM)pEvent /* lParm */);

    return VINF_SUCCESS;
}

int VBoxDnDWnd::dragRelease(void)
{
#ifdef DEBUG_andy
    LogFlowFunc(("\n"));
#endif
    /* Release mouse button in the guest to start the "drop"
     * action at the current mouse cursor position. */
    INPUT Input[1] = { 0 };
    Input[0].type       = INPUT_MOUSE;
    Input[0].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(1, Input, sizeof(INPUT));

    return VINF_SUCCESS;
}

int VBoxDnDWnd::hide(void)
{
#ifdef DEBUG_andy
    LogFlowFunc(("\n"));
#endif
    ShowWindow(hWnd, SW_HIDE);

    return VINF_SUCCESS;
}

int VBoxDnDWnd::makeFullscreen(void)
{
    int rc = VINF_SUCCESS;

    RECT r;
    RT_ZERO(r);

    BOOL fRc;
    HDC hDC = GetDC(NULL /* Entire screen */);
    if (hDC)
    {
        fRc = EnumDisplayMonitors(hDC, NULL, VBoxDnDWnd::MonitorEnumProc,
                                  (LPARAM)&r);
        if (!fRc)
            rc = VERR_NOT_FOUND;
        ReleaseDC(NULL, hDC);
    }
    else
        rc = VERR_ACCESS_DENIED;

    if (RT_FAILURE(rc))
    {
        /* If multi-monitor enumeration failed above, try getting at least the
         * primary monitor as a fallback. */
        MONITORINFO monitor_info;
        monitor_info.cbSize = sizeof(monitor_info);
        if (GetMonitorInfo(MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST),
                           &monitor_info))
        {

            r = monitor_info.rcMonitor;
            rc = VINF_SUCCESS;
        }
    }

    if (RT_SUCCESS(rc))
    {
        LONG lStyle = GetWindowLong(hWnd, GWL_STYLE);
        SetWindowLong(hWnd, GWL_STYLE,
                      lStyle & ~(WS_CAPTION | WS_THICKFRAME));
        LONG lExStyle = GetWindowLong(hWnd, GWL_EXSTYLE);
        SetWindowLong(hWnd, GWL_EXSTYLE,
                      lExStyle & ~(  WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE
                                   | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE));

        fRc = SetWindowPos(hWnd, HWND_TOPMOST,
                           r.left,
                           r.top,
                           r.right  - r.left,
                           r.bottom - r.top,
#ifdef VBOX_DND_DEBUG_WND
                           SWP_SHOWWINDOW | SWP_FRAMECHANGED);
#else
                           SWP_SHOWWINDOW | SWP_NOOWNERZORDER | SWP_NOREDRAW | SWP_NOACTIVATE);
#endif
        if (fRc)
        {
            LogFlowFunc(("Virtual screen is %ld,%ld,%ld,%ld (%ld x %ld)\n",
                         r.left, r.top, r.right, r.bottom,
                         r.right - r.left, r.bottom - r.top));
        }
        else
        {
            DWORD dwErr = GetLastError();
            LogRel(("DnD: Failed to set proxy window position, rc=%Rrc\n",
                    RTErrConvertFromWin32(dwErr)));
        }
    }
    else
        LogRel(("DnD: Failed to determine virtual screen size, rc=%Rrc\n", rc));

    LogFlowFuncLeaveRC(rc);
    return rc;
}

void VBoxDnDWnd::reset(void)
{
    LogFlowThisFunc(("Old mState=%ld\n", mState));

    lstAllowedFormats.clear();
    lstFormats.clear();
    uAllActions = DND_IGNORE_ACTION;

    mMode = Unknown;
    mState = Initialized;
}

static LRESULT CALLBACK vboxDnDWndProcInstance(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    LONG_PTR pUserData = GetWindowLongPtr(hWnd, GWLP_USERDATA);
    AssertPtrReturn(pUserData, 0);

    VBoxDnDWnd *pWnd = reinterpret_cast<VBoxDnDWnd *>(pUserData);
    if (pWnd)
        return pWnd->WndProc(hWnd, uMsg, wParam, lParam);

    return 0;
}

static LRESULT CALLBACK vboxDnDWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    /* Note: WM_NCCREATE is not the first ever message which arrives, but
     *       early enough for us. */
    if (uMsg == WM_NCCREATE)
    {
        LPCREATESTRUCT pCS = (LPCREATESTRUCT)lParam;
        AssertPtr(pCS);
        SetWindowLongPtr(hWnd, GWLP_USERDATA, (LONG_PTR)pCS->lpCreateParams);
        SetWindowLongPtr(hWnd, GWLP_WNDPROC, (LONG_PTR)vboxDnDWndProcInstance);

        return vboxDnDWndProcInstance(hWnd, uMsg, wParam, lParam);
    }

    /* No window associated yet. */
    return DefWindowProc(hWnd, uMsg, wParam, lParam);
}

/**
 * Initializes drag'n drop.
 *
 * @return  IPRT status code.
 * @param   pEnv                        The DnD service's environment.
 * @param   ppInstance                  The instance pointer which refer to this object.
 * @param   pfStartThread               Pointer to flag whether the DnD service can be started or not.
 */
int VBoxDnDInit(const VBOXSERVICEENV *pEnv, void **ppInstance, bool *pfStartThread)
{
    AssertPtrReturn(pEnv, VERR_INVALID_POINTER);
    /** ppInstance not used here. */
    AssertPtrReturn(pfStartThread, VERR_INVALID_POINTER);

    LogFlowFuncEnter();

    *pfStartThread = false;

    PVBOXDNDCONTEXT pCtx = &gCtx;

    int rc;

    /* Create the proxy window. At the moment we
     * only support one window at a time. */
    VBoxDnDWnd *pWnd = NULL;
    try
    {
        pWnd = new VBoxDnDWnd();
        rc = pWnd->Initialize(pCtx);

        /* Add proxy window to our proxy windows list. */
        if (RT_SUCCESS(rc))
            pCtx->lstWnd.append(pWnd);
    }
    catch (std::bad_alloc)
    {
        rc = VERR_NO_MEMORY;
    }

    if (RT_SUCCESS(rc))
        rc = RTSemEventCreate(&pCtx->hEvtQueueSem);
    if (RT_SUCCESS(rc))
    {
        /* Assign service environment to our context. */
        gCtx.pEnv = pEnv;
    }

    if (RT_SUCCESS(rc))
    {
        *ppInstance = pCtx;
        *pfStartThread = true;

        LogRel(("DnD: Drag'n drop service successfully started\n"));
        return VINF_SUCCESS;
    }

    LogRel(("DnD: Initializing drag'n drop service failed with rc=%Rrc\n", rc));
    return rc;
}

void VBoxDnDStop(const VBOXSERVICEENV *pEnv, void *pInstance)
{
    AssertPtrReturnVoid(pEnv);
    AssertPtrReturnVoid(pInstance);

    LogFunc(("Stopping pInstance=%p\n", pInstance));

    PVBOXDNDCONTEXT pCtx = (PVBOXDNDCONTEXT)pInstance;
    AssertPtr(pCtx);

    /* Set shutdown indicator. */
    ASMAtomicWriteBool(&pCtx->fShutdown, true);

    /** @todo Notify / wait for HGCM thread! */
}

void VBoxDnDDestroy(const VBOXSERVICEENV *pEnv, void *pInstance)
{
    AssertPtr(pEnv);
    AssertPtr(pInstance);

    LogFunc(("Destroying pInstance=%p\n", pInstance));

    PVBOXDNDCONTEXT pCtx = (PVBOXDNDCONTEXT)pInstance;
    AssertPtr(pCtx);

    int rc = VINF_SUCCESS;

    LogFunc(("Destroyed pInstance=%p, rc=%Rrc\n",
             pInstance, rc));
}

unsigned __stdcall VBoxDnDThread(void *pInstance)
{
    LogFlowFunc(("pInstance=%p\n", pInstance));

    PVBOXDNDCONTEXT pCtx = (PVBOXDNDCONTEXT)pInstance;
    AssertPtr(pCtx);

    uint32_t uClientID;
    int rc = VbglR3DnDConnect(&uClientID);
    if (RT_FAILURE(rc))
        return rc;

    /** @todo At the moment we only have one DnD proxy window. */
    Assert(pCtx->lstWnd.size() == 1);
    VBoxDnDWnd *pWnd = pCtx->lstWnd.first();
    AssertPtr(pWnd);

    /* Number of invalid messages skipped in a row. */
    int cMsgSkippedInvalid = 0;

    do
    {
        PVBOXDNDEVENT pEvent = (PVBOXDNDEVENT)RTMemAlloc(sizeof(VBOXDNDEVENT));
        if (!pEvent)
        {
            rc = VERR_NO_MEMORY;
            break;
        }
        /* Note: pEvent will be free'd by the consumer later. */

        rc = VbglR3DnDProcessNextMessage(uClientID, &pEvent->Event);
        LogFlowFunc(("VbglR3DnDProcessNextMessage returned rc=%Rrc\n", rc));

        if (ASMAtomicReadBool(&pCtx->fShutdown))
            break;

        if (RT_SUCCESS(rc))
        {
            cMsgSkippedInvalid = 0; /* Reset skipped messages count. */

            LogFlowFunc(("Received new event, type=%RU32\n", pEvent->Event.uType));

            int rc2 = pWnd->ProcessEvent(pEvent);
            if (RT_FAILURE(rc2))
                LogFlowFunc(("Processing event failed with rc=%Rrc\n", rc2));
        }
        else if (rc == VERR_CANCELLED)
        {
            int rc2 = pWnd->OnHgCancel();
            if (RT_FAILURE(rc2))
                LogFlowFunc(("Cancelling failed with rc=%Rrc\n", rc2));
        }
        else
        {
            LogFlowFunc(("Processing next message failed with rc=%Rrc\n", rc));

            /* Old(er) hosts either are broken regarding DnD support or otherwise
             * don't support the stuff we do on the guest side, so make sure we
             * don't process invalid messages forever. */
            if (rc == VERR_INVALID_PARAMETER)
                cMsgSkippedInvalid++;
            if (cMsgSkippedInvalid > 3)
            {
                LogFlowFunc(("Too many invalid/skipped messages from host, exiting ...\n"));
                break;
            }
        }

        if (ASMAtomicReadBool(&pCtx->fShutdown))
            break;

    } while (true);

    LogFlowFunc(("Shutting down ...\n"));

    VbglR3DnDDisconnect(uClientID);

    LogFlowFuncLeaveRC(rc);
    return rc;
}

