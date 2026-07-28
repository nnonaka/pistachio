/****************************************************************************
 *                
 * Copyright (C) 2002, Karlsruhe University
 *                
 * File path:	arch/powerpc/pvr.h
 * Description:	Constants and functions related to the Processor Version
 * 		Register.
 *                
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *                
 * $Id: pvr.h,v 1.4 2003/12/11 12:47:07 joshua Exp $
 *
 ***************************************************************************/

#ifndef __ARCH__POWERPC__PVR_H__
#define __ARCH__POWERPC__PVR_H__

#ifndef ASSEMBLY

/* Was a class; the ppc_pvr_e enum was protected and is file-scope in C. */
enum ppc_pvr_e {
    pvr_psim	= 0,
    pvr_601	= 1,
    pvr_603	= 3,
    pvr_604	= 4,
    pvr_603e	= 6,
    pvr_750	= 8,
    pvr_750FX	= 0x7000,
    pvr_604e	= 9,
    pvr_604ev	= 10,
    pvr_7400	= 12,
    pvr_7410	= 0x800C,
    pvr_7450	= 0x8000,
    pvr_7455	= 0x8001,
};

struct powerpc_version_t
{
    union
    {
	struct {
	    BITFIELD2( u32_t,
		revision : 16,
		version  : 16
	    );
	} x;
	u32_t raw;
    };
};
typedef struct powerpc_version_t powerpc_version_t;

INLINE powerpc_version_t powerpc_version_read (void) __attribute__ ((const));
INLINE powerpc_version_t powerpc_version_read (void)
{
    powerpc_version_t pvr;
    asm ("mfpvr %0" : "=r" (pvr.raw) );
    return pvr;
}

INLINE bool powerpc_version_is_psim (powerpc_version_t self)
{ return self.x.version == pvr_psim; }
INLINE bool powerpc_version_is_750 (powerpc_version_t self)
{ return self.x.version == pvr_750; }


#endif	/* ASSEMBLY */

#endif	/* __ARCH__POWERPC__PVR_H__ */

