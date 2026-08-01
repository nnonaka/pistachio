/*********************************************************************
 *                
 * Copyright (C) 2003, University of New South Wales
 *                
 * File path:    elf-loader/include/kip.h
 * Description:  KIP helper headers.
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
 * $Id: kip.h,v 1.3 2003/09/24 19:06:12 skoglund Exp $
 *                
 ********************************************************************/

#ifndef __ELF_LOADER__INCLUDE__KIP_H__
#define __ELF_LOADER__INCLUDE__KIP_H__

#include <l4/kip.h>
#include <l4/kcp.h>
#include <l4/types.h>

#define MAX_MEMDESC 32 

#define OFWMemorySubType_Reserved   0xE
#define OFWMemorySubType_DeviceTree 0xF

/* Two classes; the nested enum moves to file scope, and the members become
   kip_manager_* / kip_server_* entry points.  Notes §174. */
typedef enum {
  server_sigma0 = 0,
  server_sigma1,
  server_root,
  server_total,       // Total number of servers.
  server_invalid = -1 // Invalid server.
} kip_server_e;

struct kip_server_t
{
  L4_Word_t sp;         // Initial stack pointer.
  L4_Word_t ip;         // Initial instruction pointer.
  L4_Word_t low;        // Start address of server.
  L4_Word_t high;       // End address of server.
}; // kip_server_t
typedef struct kip_server_t kip_server_t;

L4_INLINE void kip_server_clear (kip_server_t *self)
{
  self->sp = self->ip = self->low = self->high = 0;
}

struct kip_manager_t
{
  /* were protected */
  L4_KernelConfigurationPage_t * kip; // Where the KIP gets loaded.

  L4_MemoryDesc_t memdesc[MAX_MEMDESC];
  kip_server_t    servers[server_total];

  L4_Word_t boot_info;
  L4_Word_t total_memdesc;
}; // kip_manager_t
typedef struct kip_manager_t kip_manager_t;

bool kip_manager_add_server (kip_manager_t *self, kip_server_e server,
			     L4_Word_t sp, L4_Word_t ip,
			     L4_Word_t low, L4_Word_t high);
int kip_manager_add_memdesc (kip_manager_t *self, bool is_virt,
			     L4_Word_t low, L4_Word_t high,
			     L4_Word_t type, L4_Word_t sub_type);

L4_Word_t kip_manager_mem_base (kip_manager_t *self);
L4_Word_t kip_manager_first_avail_page (kip_manager_t *self);

bool kip_manager_init (kip_manager_t *self, void * kip_addr);
bool kip_manager_update (kip_manager_t *self);

L4_INLINE void kip_manager_set_boot_info (kip_manager_t *self, L4_Word_t value)
{
  self->boot_info = value;
}

extern kip_manager_t kip_manager;


#endif /* !__ELF_LOADER__INCLUDE__KIP_H__ */
