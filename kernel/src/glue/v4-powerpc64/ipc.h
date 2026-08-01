/*********************************************************************
 *
 * Copyright (C) 1999-2010,  Karlsruhe University
 *
 * File path:     glue/v4-powerpc64/ipc.h
 * Description:
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
 * $Id$
 *
 ********************************************************************/
#ifndef __GLUE__V4_POWERPC64__IPC_H__
#define __GLUE__V4_POWERPC64__IPC_H__

/* New file, and deliberately empty.  api/v4/sched-rr/ktcb.h and
   api/v4/sched-hs/ktcb.h both include INC_GLUE(ipc.h) unconditionally, but
   upstream never wrote one for powerpc64: the hierarchical/round-robin
   schedulers and the ctrlxfer message protocol that gave the header its
   contents (the arch_ctrlxfer_id_e enum, see glue/v4-powerpc/ipc.h) both
   postdate the powerpc64 port, so no shipped powerpc64 configuration ever
   reached the include.  Nothing in it is powerpc64-specific to convert --
   CONFIG_X_CTRLXFER_MSG is x86-only -- so the header exists only to satisfy
   the include.  Notes §165. */

#endif /* !__GLUE__V4_POWERPC64__IPC_H__ */
