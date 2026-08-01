/****************************************************************************
 *
 * Copyright (C) 2002-2003, Karlsruhe University
 *
 * File path:	include/piggybacker/kip.h
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
 * $Id: kip.h,v 1.3 2003/10/27 00:26:15 cvansch Exp $
 *
 ***************************************************************************/
#ifndef __PIGGYBACKER__INCLUDE__KIP_H__
#define __PIGGYBACKER__INCLUDE__KIP_H__

#include <l4/types.h>
#include <l4/kcp.h>
#include <l4/kip.h>

/* Two classes; the enum moves to file scope and the members become
   kip_manager_* / kip_server_* entry points.  Notes §173. */
struct kip_server_t
{
    L4_Word_t ip;
    L4_Word_t start;
    L4_Word_t end;
};
typedef struct kip_server_t kip_server_t;

L4_INLINE void kip_server_clear (kip_server_t *self)
{ self->ip = self->start = self->end = 0; }

enum server_e {
    sigma0 = 0,
    root_task,
    kernel,
    tot,
};

struct kip_manager_t
{
    /* were protected */
    L4_KernelConfigurationPage_t *kip_src;
    L4_KernelConfigurationPage_t *kip_dst;

    kip_server_t servers[tot];

    L4_Word_t boot_info;
    L4_Word_t mem_desc_cnt;
};
typedef struct kip_manager_t kip_manager_t;

/* was protected */
void kip_manager_install_module (kip_manager_t *self, L4_Word_t mod_start,
				 L4_Word_t mod_end, kip_server_t *server);

bool kip_manager_find_kip (kip_manager_t *self, L4_Word_t kernel_start);
void kip_manager_install_sigma0 (kip_manager_t *self, L4_Word_t mod_start, L4_Word_t mod_end);
void kip_manager_install_root_task (kip_manager_t *self, L4_Word_t mod_start, L4_Word_t mod_end);
void kip_manager_install_kernel (kip_manager_t *self, L4_Word_t mod_start, L4_Word_t mod_end);
L4_Word_t kip_manager_first_avail_page (kip_manager_t *self);

bool kip_manager_virt_to_phys (kip_manager_t *self, L4_Word_t virt,
			       L4_Word_t elf_start, L4_Word_t *phys);

void kip_manager_update_kip (kip_manager_t *self);
L4_INLINE void kip_manager_set_boot_info (kip_manager_t *self, L4_Word_t val)
{ self->boot_info = val; }
void kip_manager_setup_main_memory (kip_manager_t *self, L4_Word_t start, L4_Word_t end);
void kip_manager_dedicate_memory (kip_manager_t *self, L4_Word_t start, L4_Word_t end,
				  L4_Word_t type, L4_Word_t sub_type);

void kip_manager_init (kip_manager_t *self);

#endif	/* __PIGGYBACKER__INCLUDE__KIP_H__ */
