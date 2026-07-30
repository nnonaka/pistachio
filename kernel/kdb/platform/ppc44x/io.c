/*********************************************************************
 *                
 * Copyright (C) 2010,  Karlsruhe Institute of Technology
 * Copyright (C) 2008-2009,  Volkmar Uhlig, Jan Stoess, IBM Corporation
 *                
 * Filename:      io.cc
 * Author:        Volkmar Uhlig, Jan Stoess <stoess@kit.edu>
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
 ********************************************************************/

#include <debug.h>
#include <lib.h>
#include <kdb/console.h>
#include <sync.h>
#include INC_ARCH(io.h)
#include INC_ARCH(cache.h)
#include INC_ARCH(ppc_registers.h)
#include INC_ARCH(ppc44x.h)
#include INC_PLAT(fdt.h)

#define SEC_PPC44X_IO		".kdebug"
extern addr_t setup_console_mapping(paddr_t paddr, int log2size);
bool getc_blocked = false;


/* Section assignements */
#if defined(CONFIG_KDB_CONS_BGP_JTAG)
static void putc_jtag (char) SECTION (SEC_PPC44X_IO);
static char getc_jtag (bool) SECTION (SEC_PPC44X_IO);
static void init_jtag (void) SECTION (SEC_PPC44X_IO);
#endif

#if defined(CONFIG_KDB_CONS_BGP_TREE)
static void putc_bgtree (char) SECTION (SEC_PPC44X_IO);
static char getc_bgtree (bool) SECTION (SEC_PPC44X_IO);
static void init_bgtree (void) SECTION (SEC_PPC44X_IO);
#endif

#if defined(CONFIG_KDB_CONS_COM)
static void putc_serial (char) SECTION (SEC_PPC44X_IO);
static char getc_serial (bool) SECTION (SEC_PPC44X_IO);
static void init_serial (void) SECTION (SEC_PPC44X_IO);
#endif 

#if defined(CONFIG_KDB_BREAKIN)
void kdebug_check_breakin (void) SECTION (SEC_PPC44X_IO);
#endif

kdb_console_t kdb_consoles[] = {
#if defined(CONFIG_KDB_CONS_BGP_JTAG)
    { "jtag", init_jtag, putc_jtag, getc_jtag },
#endif
#if defined(CONFIG_KDB_CONS_BGP_TREE)
    { "tree", init_bgtree, putc_bgtree, getc_bgtree },
#endif
#if defined(CONFIG_KDB_CONS_COM)
    { "serial", &init_serial, &putc_serial, &getc_serial },
#endif 
    KDB_NULL_CONSOLE
};

/*
**
** Serial port I/O functions.
**
*/
#if defined(CONFIG_KDB_CONS_COM)

#if !defined(CONFIG_KDB_COMPORT)
#define CONFIG_KDB_COMPORT 0
#endif
#if !defined(CONFIG_KDB_COMSPEED)
#define CONFIG_KDB_COMSPEED 115200
#endif
static u8_t *comport = CONFIG_KDB_COMPORT;


static void init_serial (void)
{
#define IER	(comport+1)
#define EIR	(comport+2)
#define LCR	(comport+3)
#define MCR	(comport+4)
#define LSR	(comport+5)
#define MSR	(comport+6)
#define DLLO	(comport+0)
#define DLHI	(comport+1)

#if CONFIG_COMPORT == 1
    UNIMPLEMENTED();
#endif
    
#if CONFIG_COMPORT == 0
    /*  FDT  */
    fdt_property_t *prop;
    fdt_node_t *node;
    fdt_t *fdt;    

    if (!(fdt = get_dtree()))
        return;
    
    if (!(node = fdt_find_subtree (fdt, "/aliases")))
        return;

    if (! (prop = fdt_find_property_node_in (fdt, node, "serial0")) )
        return;

    if (!(node = fdt_find_subtree (fdt, fdt_property_get_string (prop))))
        return;
    
    if (! (prop = fdt_find_property_node_in (fdt, node, "reg")) )
        return;
    
    // Serial bus is beyond 4GB
    u64_t comport_phys = 0x100000000ULL | (u64_t) fdt_property_get_word (prop, 0);
    comport = (u8_t*)setup_console_mapping(comport_phys, 12);
    
#endif /* CONFIG_COMPORT == 0 */

    if (comport)
    {
        out_8(LCR, 0x80);          /* select bank 1        */
        for (volatile int i = 10000000; i--; );
        out_8(DLLO, (((115200/CONFIG_KDB_COMSPEED) >> 0) & 0x00FF));
        out_8(DLHI, (((115200/CONFIG_KDB_COMSPEED) >> 8) & 0x00FF));
        out_8(LCR, 0x03);          /* set 8,N,1            */
        out_8(IER, 0x00);          /* disable interrupts   */
        out_8(EIR, 0x07);          /* enable FIFOs */
        in_8(IER);
        in_8(EIR);
        in_8(LCR);
        in_8(MCR);
        in_8(LSR);
        in_8(MSR);
        
    }

}


static void putc_serial (const char c)
{
    while ((in_8(LSR) & 0x20) == 0);
    out_8(comport,c);
    while ((in_8(LSR) & 0x40) == 0);
    if (c == '\n')
	putc_serial('\r');
}

static char getc_serial (bool block)
{
    if ((in_8(LSR) & 0x01) == 0)
    {
	if (!block)
	    return (char) -1;
	
        getc_blocked = true;
	while ((in_8(LSR) & 0x01) == 0);
        getc_blocked = false;
	
    }
    return in_8(comport);
}

static bool check_breakin_serial ()
{
#if defined(CONFIG_KDB_BREAKIN_BREAK) || defined(CONFIG_KDB_BREAKIN_ESCAPE)
    u8_t c = in_8(LSR);
#endif

#if defined(CONFIG_KDB_BREAKIN_ESCAPE)
    if ((c & 0x01) && (in_8(comport) == 0x1b))
        return true;
#endif
    return false;
}

#endif /* defined(CONFIG_KDB_CONS_COM) */


#if defined(CONFIG_KDB_CONS_BGP_JTAG)
/*
**
** Bluegene JTAG I/O functions.
**
*/

enum mb_commands_e {
    cmd_print = 2,
};

struct bgp_mailbox_t
{
    volatile unsigned short command;	// comand; upper bit=ack
    unsigned short len;			// length (does not include header)
    unsigned short result;		// return code from reader
    unsigned short crc;			// 0=no CRC
    char data[0];
};
typedef struct bgp_mailbox_t bgp_mailbox_t;

typedef struct jtag_console_t 
{
    u64_t mb_phys;
    bgp_mailbox_t *mb;
    word_t size;
    word_t dcr_set;
    word_t dcr_clear;
    word_t dcr_mask;

    void send_command(int command)
	{ 
	    mb->command = command;
	    asm volatile("sync");
	    ppc_set_dcr(dcr_set, dcr_mask);
	    
	    do {
		ppc_cache_invalidate_block((word_t)&mb->command);
	    } while(!(mb->command & 0x8000));
	}

    void putc(char c)
	{
	    if (!mb) return;

	    mb->data[mb->len++] = c;
	
	    if (mb->len >= size || c == '\n')
	    {
		send_command(cmd_print);
		mb->len = 0;
	    }
	}

    bool init(fdt_t *fdt)
	{
	    /* initialize only once */
	    if (mb)
		return true;
	    
	    fdt_property_t *prop;
	    fdt_node_t *node = fdt_find_subtree (fdt, "/jtag/console0");

	    if (! (prop = fdt_find_property_node_in (fdt, node, "reg")) )
		return false;

	    size = fdt_property_get_word (prop, 2);
	    mb_phys = fdt_property_get_u64 (prop, 0);
#warning fix uboot
	    mb_phys |= 0x700000000ULL;

	    if (! (prop = fdt_find_property_node_in (fdt, node, "dcr-reg")) )
		return false;

	    dcr_set = fdt_property_get_word (prop, 0);
	    dcr_clear = fdt_property_get_word (prop, 1);
	    
	    if (! (prop = fdt_find_property_node_in (fdt, node, "dcr-mask")) )
		return false;
	    
	    dcr_mask = fdt_property_get_word (prop, 0);

	    mb = (bgp_mailbox_t*)setup_console_mapping(mb_phys, 14);
	    return true;
	}
};

void init_bgtree();
static jtag_console_t cons;

static void init_jtag()
{
    cons.init(get_dtree());
    init_bgtree();
}

const char kbd_ret[] = "1q2q";
static char getc_jtag(bool block) 
{
    static int cnt = 0;

    if (cnt < 1)
	return kbd_ret[cnt++];

    if (block)
    {
        getc_blocked = true;
	while(1);
        getc_blocked = false;
    }
    return 0; 
}

static void putc_jtag(char c) 
{
    cons.putc(c);
}
#endif

/*
**
** Bluegene Tree I/O functions.
**
*/

#if defined(CONFIG_KDB_CONS_BGP_TREE)

// tree ifc memory offsets
#define BGP_TRx_DI		(0x00U)
#define BGP_TRx_HI		(0x10U)
#define BGP_TRx_DR		(0x20U)
#define BGP_TRx_HR		(0x30U)
#define BGP_TRx_Sx		(0x40U)

#define BGP_NUM_CHANNEL		2

/* hardware header */
struct bgtree_header_t
{
    union {
	word_t raw;
	struct {
	    word_t pclass	: 4;
	    word_t p2p		: 1;
	    word_t irq		: 1;
	    word_t vector	: 24;
	    word_t csum_mode	: 2;
	} p2p;
	struct {
	    word_t pclass	: 4;
	    word_t p2p		: 1;
	    word_t irq		: 1;
	    word_t op		: 3;
	    word_t opsize	: 7;
	    word_t tag		: 14;
	    word_t csum_mode	: 2;
	} bcast;
    };

} __attribute__((packed));
typedef struct bgtree_header_t bgtree_header_t;

/* irq defaulted to false, tag to 0. */
INLINE void bgtree_header_set_p2p (bgtree_header_t *self, word_t pclass, word_t vector, bool irq)
{
    self->raw = 0;
    self->p2p.pclass = pclass;
    self->p2p.irq = irq;
    self->p2p.vector = vector;
    self->p2p.p2p = 1;
}

INLINE void bgtree_header_set_broadcast (bgtree_header_t *self, word_t pclass, word_t tag, bool irq)
{
    self->raw = 0;
    self->bcast.pclass = pclass;
    self->bcast.irq = irq;
    self->bcast.tag = tag;
}


struct bgtree_status_t 
{
    union {
	word_t raw;
	struct {
	    word_t inj_pkt	: 4;
	    word_t inj_qwords	: 4;
	    word_t		: 4;      
	    word_t inj_hdr	: 4;
	    word_t rcv_pkt	: 4;
	    word_t rcv_qwords	: 4;
	    word_t		: 3;
	    word_t irq		: 1;
	    word_t rcv_hdr	: 4;
	};
    };
} __attribute__((packed));
typedef struct bgtree_status_t bgtree_status_t;

/* link layer */
struct bglink_hdr_t
{
    word_t dst_key; 
    word_t src_key; 
    u16_t conn_id; 
    u8_t this_pkt; 
    u8_t total_pkt;
    u16_t lnk_proto;	// 1 eth, 2 con, 3...
    u16_t optional;	// for encapsulated protocol use
} __attribute__((packed));
typedef struct bglink_hdr_t bglink_hdr_t;

/* Was bgtree_t's nested channel_t plus its static helpers.  C has no nested
   types or class-scoped statics, so both move out. */
INLINE void bgtree_fpu_memcpy_16 (void *dst, void *src)
{
    asm volatile("lfpdx 0,0,%0\n"
		 "stfpdx 0,0,%1\n"
		 :
		 : "b"(src), "b"(dst)
		 : "fr0", "memory");
}

INLINE void in128 (addr_t reg, void *ptr)  { bgtree_fpu_memcpy_16(ptr, reg); }
INLINE void out128 (addr_t reg, void *ptr) { bgtree_fpu_memcpy_16(reg, ptr); }

struct bgtree_channel_t
{
    addr_t base;		// virtual base address of tree
    paddr_t base_phys;		// phys location
};
typedef struct bgtree_channel_t bgtree_channel_t;

INLINE void bgtree_channel_send_header (bgtree_channel_t *self, bgtree_header_t *hdr)
{ out_be32(addr_offset(self->base, BGP_TRx_HI), hdr->raw); }

INLINE void bgtree_channel_send_payload_block (bgtree_channel_t *self, void *payload)
{ out128(addr_offset(self->base, BGP_TRx_DI), payload); }

INLINE void bgtree_channel_rcv_payload_block (bgtree_channel_t *self, void *payload)
{ in128(addr_offset(self->base, BGP_TRx_DR), payload); }

INLINE bgtree_header_t bgtree_channel_get_header (bgtree_channel_t *self)
{
    bgtree_header_t hdr;
    hdr.raw = in_be32(addr_offset(self->base, BGP_TRx_HR));
    return hdr;
}

INLINE bgtree_status_t bgtree_channel_get_status (bgtree_channel_t *self)
{
    bgtree_status_t status;
    status.raw = in_be32(addr_offset(self->base, BGP_TRx_Sx));
    return status;
}

bool bgtree_channel_init (bgtree_channel_t *self, int channel, paddr_t pbase, size_t size);
/* lnkhdr was a bglink_hdr_t& */
bool bgtree_channel_send (bgtree_channel_t *self, bgtree_header_t hdr,
			  bglink_hdr_t *lnkhdr, void *payload);
bool bgtree_channel_poll (bgtree_channel_t *self, bglink_hdr_t *lnkhdr, void *payload);

struct bgtree_t
{
    bgtree_channel_t channel[BGP_NUM_CHANNEL];

    word_t dcr_base;
    word_t curr_conn;
    word_t node_id;	// self
};
typedef struct bgtree_t bgtree_t;

bool bgtree_init (bgtree_t *self, fdt_t *fdt);

INLINE void bgtree_init_link_hdr (bgtree_t *self, bglink_hdr_t *hdr)
{
    hdr->src_key = self->node_id;
    hdr->conn_id = self->curr_conn++;
}

/* was the static member bgtree_t::tree */
extern bgtree_t bgtree_tree;

INLINE bgtree_t * bgtree_get_device (fdt_t *fdt, word_t handle) { return &bgtree_tree; }

bgtree_t bgtree_tree;

typedef struct {
    word_t data[4];
} fpu_reg_t __attribute__((aligned(16)));

static inline void store_fr0(fpu_reg_t *fp)
{
    asm volatile("stfpdx 0,0,%0\n" : : "b"(fp));
}

static inline void load_fr0(fpu_reg_t *fp)
{
    asm volatile("lfpdx 0,0,%0\n" : : "b"(fp));
}

bool bgtree_channel_send (bgtree_channel_t *self, bgtree_header_t hdr, bglink_hdr_t *lnkhdr, void *payload)
{
    if (bgtree_channel_get_status (self).inj_hdr >= 8)
	return false;

    // XXX: fix stack alignment
    static fpu_reg_t fp0;
    store_fr0(&fp0);

    bgtree_channel_send_header (self, &hdr);
    bgtree_channel_send_payload_block (self, (char*)&lnkhdr);
    for (int idx = 0; idx < 15; idx++) 
	bgtree_channel_send_payload_block (self, (char*)payload + idx * 16);
    load_fr0(&fp0);
    return true;
}

bool bgtree_channel_poll (bgtree_channel_t *self, bglink_hdr_t *lnkhdr, void *payload)
{
    bgtree_header_t hdr;
    if (bgtree_channel_get_status (self).rcv_hdr == 0)
	return false;

    // XXX: fix stack alignment
    static fpu_reg_t fp0;
    store_fr0(&fp0);

    hdr = bgtree_channel_get_header (self);
    bgtree_channel_rcv_payload_block (self, lnkhdr);
    for (int i = 0; i < 15; i++)
	bgtree_channel_rcv_payload_block (self, (char*)payload + i * 16);
    load_fr0(&fp0);
    return true;
}

bool bgtree_channel_init (bgtree_channel_t *self, int channel, paddr_t pbase, size_t size)
{
    self->base_phys = pbase;
    self->base = setup_console_mapping(self->base_phys, 12);
    return true;
}


bool bgtree_init (bgtree_t *self, fdt_t *fdt)
{
    fdt_property_t *prop;

    fdt_node_t *node = fdt_header_node (fdt_find_subtree (fdt, "/plb/tree"));
    if (!node)
	return false;

    if (! (prop = fdt_find_property_node_in (fdt, node, "dcr-reg")) )
	return false;
    self->dcr_base = fdt_property_get_word (prop, 0);

    if (! (prop = fdt_find_property_node_in (fdt, node, "nodeid")) )
	return false;
    self->node_id = fdt_property_get_word (prop, 0);

    if (! (prop = fdt_find_property_node_in (fdt, node, "reg")) )
	return false;

    for (int chnidx = 0; chnidx < 2; chnidx++)
	bgtree_channel_init (&self->channel[chnidx], chnidx,
			     fdt_property_get_u64 (prop, chnidx * 3),
			     fdt_property_get_word (prop, chnidx * 3 + 2));

    /* disable send and receive IRQs */
    ppc_set_dcrx(self->dcr_base + 0x45, 0);
    ppc_set_dcrx(self->dcr_base + 0x49, 0);

    /* clear anything that may be pending */
    ppc_get_dcrx(self->dcr_base + 0x44);
    ppc_get_dcrx(self->dcr_base + 0x48);

    return true;
}

#define BUF_SIZE		240

struct tree_console_t {
    // console buffers...
    char out_buf[BUF_SIZE] __attribute__((aligned(16)));
    char in_buf[BUF_SIZE] __attribute__((aligned(16)));
    int out_len;
    int in_len;
    int in_head;

    word_t send_id;	// console send id
    word_t rcv_id;	// console receive id
    int dest_node;	// destination node
    word_t proto_id;
    word_t route;
    word_t channel;
    bgtree_t *tree;
    spinlock_t lock;
};
typedef struct tree_console_t tree_console_t;

static tree_console_t tree_console;

bool tree_console_init (tree_console_t *self, fdt_t *fdt);
void tree_console_flush_outbuf (tree_console_t *self);
void tree_console_poll (tree_console_t *self);
void tree_console_inject (tree_console_t *self, except_regs_t *frame);

INLINE void tree_console_putc (tree_console_t *self, char c)
{
    spinlock_lock (&self->lock);
    self->out_buf[self->out_len++] = c;
    if (self->out_len >= BUF_SIZE || c == '\n')
	tree_console_flush_outbuf (self);
    spinlock_unlock (&self->lock);
}

INLINE char tree_console_getc (tree_console_t *self, bool block)
{
    char c = 0;

    spinlock_lock (&self->lock);

    getc_blocked = true;
    do {
	tree_console_flush_outbuf (self); // make sure the other end sees all output...
	tree_console_poll (self);
    } while (block && self->in_len == 0);
    getc_blocked = false;

    if (self->in_len)
    {
	c = self->in_buf[self->in_head];
	self->in_head = (self->in_head + 1) % BUF_SIZE;
	self->in_len--;
    }
    spinlock_unlock (&self->lock);
    return c;
}

INLINE void tree_console_enqueue_char (tree_console_t *self, char c)
{
    if (self->in_len >= BUF_SIZE)
	return; // just drop
    self->in_buf[(self->in_head + self->in_len) % BUF_SIZE] = c;
    self->in_len++;
}

INLINE void tree_console_enqueue_packet (tree_console_t *self, bglink_hdr_t *lnkhdr, char *buf)
{
    int len, i;

    if (lnkhdr->lnk_proto != self->proto_id ||
	lnkhdr->dst_key != self->rcv_id ||
	lnkhdr->src_key == self->tree->node_id)
	return;

    len = min (lnkhdr->optional - 240 * lnkhdr->this_pkt, 240);
    for (i = 0; i < len; i++)
	tree_console_enqueue_char (self, buf[i]);
}

INLINE bool tree_console_check_breakin (tree_console_t *self)
{
    bool ret = false;
#if defined(CONFIG_KDB_BREAKIN_ESCAPE)
    spinlock_lock (&self->lock);
    tree_console_poll (self);
    ret = self->in_len != 0 && self->in_buf[self->in_head] == 0x1b;
    spinlock_unlock (&self->lock);
#endif
    return ret;
}


NOINLINE void tree_console_poll (tree_console_t *self)
{
    static char buf[240] __attribute__((aligned(16)));
    static bglink_hdr_t lnkhdr __attribute__((aligned(16)));

    while (bgtree_channel_poll (&self->tree->channel[self->channel], &lnkhdr, buf))
	tree_console_enqueue_packet (self, &lnkhdr, buf);

    while (bgtree_channel_poll (&self->tree->channel[self->channel == 0 ? 1 : 0], &lnkhdr, buf)) 
    { /* deplete the other self->channel */ }
}

NOINLINE void tree_console_inject (tree_console_t *self, except_regs_t *frame)
{
    static char buf[256] __attribute__((aligned(16)));
    char *c = buf;

    asm volatile(
	"stfpdx	  0, 0, %[dest]\n"
	"stfpdux  1, %[dest], %[offset]\n"
	"stfpdux  2, %[dest], %[offset]\n"
	"stfpdux  3, %[dest], %[offset]\n"
	"stfpdux  4, %[dest], %[offset]\n"
	"stfpdux  5, %[dest], %[offset]\n"
	"stfpdux  6, %[dest], %[offset]\n"
	"stfpdux  7, %[dest], %[offset]\n"
	"stfpdux  8, %[dest], %[offset]\n"
	"stfpdux  9, %[dest], %[offset]\n"
	"stfpdux 10, %[dest], %[offset]\n"
	"stfpdux 11, %[dest], %[offset]\n"
	"stfpdux 12, %[dest], %[offset]\n"
	"stfpdux 13, %[dest], %[offset]\n"
	"stfpdux 14, %[dest], %[offset]\n"
	"stfpdux 15, %[dest], %[offset]\n"
	: [dest] "+b"(c)
	: [offset] "b" (16)
	);

    tree_console_enqueue_packet (self, (bglink_hdr_t*)(buf), &buf[16]);
}

NOINLINE void tree_console_flush_outbuf (tree_console_t *self)
{
    static bglink_hdr_t lnkhdr __attribute__((aligned(16)));

    if (!self->out_len)
	return;

    bgtree_init_link_hdr (self->tree, &lnkhdr);

    lnkhdr.dst_key = self->send_id;
    lnkhdr.this_pkt = 0;
    lnkhdr.total_pkt = 1;
    lnkhdr.lnk_proto = self->proto_id;
    lnkhdr.optional = self->out_len;
	
    bgtree_header_t hdr;
    /* set_broadcast's tag and both irq arguments defaulted. */
    if (self->dest_node == -1)
	bgtree_header_set_broadcast (&hdr, self->route, 0, false);
    else
	bgtree_header_set_p2p (&hdr, self->route, self->dest_node, false);

    bgtree_channel_send (&self->tree->channel[self->channel], hdr, &lnkhdr, self->out_buf);

    memset(self->out_buf, 0, BUF_SIZE);
    self->out_len = 0;
}

/* string was a char*& -- atoi advances the caller's pointer past the digits. */
int atoi(char **string)
{
    int val = 0;
    while (**string >= '0' && **string <= '9')
    {
	val = val * 10 + (**string - '0');
	(*string)++;
    }
    return val;
}

NOINLINE bool tree_console_init (tree_console_t *self, fdt_t *fdt)
{
    fdt_property_t *prop;

    fdt_node_t *tty = fdt_header_node (fdt_find_subtree (fdt, "/plb/tty"));
    if (!tty)
	return false;

    /* figure the configuration of the tty first so that we can map
     * the correct self->tree self->channel */
    if (! (prop = fdt_find_property_node_in (fdt, tty, "self->tree-self->route")) )
	return false;
    self->route = fdt_property_get_word (prop, 0);

    if (! (prop = fdt_find_property_node_in (fdt, tty, "link-protocol")) )
	return false;
    self->proto_id = fdt_property_get_word (prop, 0);
    
    if (! (prop = fdt_find_property_node_in (fdt, tty, "self->tree-self->channel")) )
	return false;
    self->channel = fdt_property_get_word (prop, 0);

    self->send_id = self->rcv_id = 2;
    self->dest_node = 0;

    fdt_node_t *l4node = fdt_header_node (fdt_find_subtree (fdt, "/l4"));
    if ( l4node && (prop = fdt_find_property_node_in (fdt, l4node, "dbgcon")) )
    {
	/* format: sndid,rcvid,dest */
	char *string = fdt_property_get_string (prop);
	self->send_id = atoi(&string);
	++string;
	self->rcv_id = atoi(&string);
	++string;
	self->dest_node = atoi(&string);
    }

    self->tree = bgtree_get_device (fdt, 0);

#warning hard coded console
    return true;
}

void init_bgtree()
{
    static bool initialized = false;
    if (!initialized) {
	bgtree_init (&bgtree_tree, get_dtree());
	tree_console_init (&tree_console, get_dtree());
	initialized = true;
    }
}

void putc_bgtree(char c)
{
#if defined(CONFIG_KDB_BREAKIN)
    tree_console_poll (&tree_console); /* first empty the fifos */
#endif
    tree_console_putc (&tree_console, c);
}

char getc_bgtree(bool block)
{
    return tree_console_getc (&tree_console, block);
}
#endif


void kdb_inject(except_regs_t* frame)
{
#if defined(CONFIG_KDB_CONS_BGP_TREE)
    tree_console_inject (&tree_console, frame);
#endif
}


#if defined(CONFIG_KDB_BREAKIN) 
void kdebug_check_breakin (void)
{
#if defined(CONFIG_KDB_CONS_BGP_TREE)
    if (tree_console_check_breakin (&tree_console))
	enter_kdebug("breakin");
#endif

#if defined(CONFIG_KDB_CONS_COM)
    if (check_breakin_serial())
	enter_kdebug("breakin");
#endif 
    
    return;
}
#endif

