/* $Id$ */
/** @file
 * VBoxGuest - Guest Additions Driver, bits shared with the windows code.
 */

/*
 * Copyright (C) 2010 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */

#ifndef ___VBoxGuest2_h
#define ___VBoxGuest2_h

#include <VBox/ostypes.h>

RT_C_DECLS_BEGIN

int VBoxGuestReportGuestInfo(VBOXOSTYPE enmOSType);
int VBoxGuestReportDriverStatus(bool fActive);

RT_C_DECLS_END

#endif

