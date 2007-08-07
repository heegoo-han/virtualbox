/** @file
 * innotek Portable Runtime - ctype.h wrapper.
 */

/*
 * Copyright (C) 2006-2007 innotek GmbH
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation,
 * in version 2 as it comes in the "COPYING" file of the VirtualBox OSE
 * distribution. VirtualBox OSE is distributed in the hope that it will
 * be useful, but WITHOUT ANY WARRANTY of any kind.
 */

#ifndef ___iprt_ctype_h
#define ___iprt_ctype_h

#include <ctype.h>

#if defined(_MSC_VER) && !defined(isblank)
/* isblank for MSC */
# define isblank(ch)  ( (ch) == ' ' || (ch) == '\t' )
#endif

#endif

