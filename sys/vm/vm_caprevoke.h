/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2019 Nathaniel Filardo
 * All rights reserved.
 *
 * This software was developed by SRI International and the University of
 * Cambridge Computer Laboratory (Department of Computer Science and
 * Technology) under DARPA contract HR0011-18-C-0016 ("ECATS"), as part of the
 * DARPA SSITH research programme.
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
 */

#ifndef	_VM_CAPREVOKE_
#define _VM_CAPREVOKE_
#ifdef CHERI_CAPREVOKE

/**************************** FORWARD DECLARATIONS ***************************/

struct caprevoke_info;
struct caprevoke_info_page;
struct caprevoke_stats;
struct file;
struct vm_caprevoke_cookie;
struct vm_map;
struct vm_page;

/***************************** REVOCATION ITSELF *****************************/

  /* sys/.../cheri/cheri_otype.c */
void * __capability cheri_revoke_sealed(void * __capability r);

/*
 * The revoked image of a capability is a *tagged* quantity with zero
 * permissions.
 *
 * If the input is sealed, the revoked image is, at present, the unsealed,
 * zero-permission variant, so that software that uses sealing types for
 * tokens will notice the type mismatch and architectural usage will fail.
 * This is almost certainly a sufficiently subtle point that this is not
 * entirely the right answer, though I hope it's not entirely wrong, either.
 */
static __always_inline inline void * __capability
cheri_revoke(void * __capability c)
{
#ifndef CHERI_CAPREVOKE_CLEARTAGS
	if (__builtin_expect(cheri_gettype(c) == -1, 1)) {
		return cheri_andperm(c, 0);
	}
	return cheri_revoke_sealed(c);
#else
	/* No need to handle sealed things specially */
	return cheri_cleartag(c);
#endif
}

/***************************** KERNEL MI LAYER ******************************/

struct vm_caprevoke_cookie {
	struct vm_map * map;			/* The map itself */
	const uint8_t * __capability crshadow;	/* Access to the shadow space */
	struct caprevoke_info_page * __capability info_page;
#ifdef CHERI_CAPREVOKE_STATS
	struct caprevoke_stats *stats;		/* Statistics */
#endif

	/*
	 * To support optimization as to which bitmap(s) we look at,
	 * given revocation runs may use different predicates on
	 * capabilities under test.
	 */
	int (*caprevoke_test_int)(const uint8_t * __capability shadow,
				  const void * __capability cut);
};

int vm_caprevoke_cookie_init(struct vm_map * map,
			     struct caprevoke_stats * stats,
			     struct vm_caprevoke_cookie *baked);
void vm_caprevoke_cookie_rele(struct vm_caprevoke_cookie *cookie);

enum {
	/* Set externally */
	VM_CAPREVOKE_INCREMENTAL=0x01,
	VM_CAPREVOKE_LAST_INIT=0x02,
	VM_CAPREVOKE_LAST_FINI=0x04,
	VM_CAPREVOKE_PMAP_SYNC=0x08,

	/* Set internally */
	VM_CAPREVOKE_QUICK_SUCCESSOR=0x10,
};

int vm_caprevoke(const struct vm_caprevoke_cookie *, int);
int vm_caprevoke_one(const struct vm_caprevoke_cookie *, int, vm_offset_t);

/***************************** KERNEL MD LAYER ******************************/

int vm_caprevoke_test(const struct vm_caprevoke_cookie *,
		      const void * __capability);


enum {
	/* If no coarse bits set, VMMAP-bearing caps are imune */
	VM_CAPREVOKE_CF_NO_COARSE_MEM = 0x01,

	/* If no otype bits set, Permit_Seal and _Unseal are imune */
	VM_CAPREVOKE_CF_NO_OTYPES = 0x02,
	VM_CAPREVOKE_CF_NO_CIDS   = 0x04,
};
void vm_caprevoke_set_test(struct vm_caprevoke_cookie *, int flags);

/*  Shadow region installation into vm map */
int vm_map_install_caprevoke_shadow (struct vm_map * map);

/*  Shadow map capability constructor */
void * __capability vm_caprevoke_shadow_cap(int sel, vm_offset_t base,
					    vm_offset_t size, int perm_mask);
/*  Publish state to shared page */
void vm_caprevoke_publish(const struct vm_caprevoke_cookie *,
			  const struct caprevoke_info *);

/*  Walking a particular page */
#define VM_CAPREVOKE_PAGE_HASCAPS	0x01
#define VM_CAPREVOKE_PAGE_DIRTY		0x02
int vm_caprevoke_page(const struct vm_caprevoke_cookie * c,
		      struct vm_page * m);
int vm_caprevoke_page_ro(const struct vm_caprevoke_cookie * c,
		      struct vm_page * m);

/***************************** HOARDER CALLBACKS ******************************/

/*  sys/kern/vfs_aio.c */
void aio_caprevoke(struct proc *, const struct vm_caprevoke_cookie *);

/*  sys/kern/kern_event.c */
int kqueue_caprevoke(struct file *fp, const struct vm_caprevoke_cookie *);

/*  sys/kern/kern_sig.c */
void sigaltstack_caprevoke(struct thread *, const struct vm_caprevoke_cookie *);

/*  sys/kern/kern_time.c */
void ktimer_caprevoke(struct proc *, const struct vm_caprevoke_cookie *);

/*  MD */
void caprevoke_td_frame(struct thread *td, const struct vm_caprevoke_cookie *);

/**************************** STATISTICS COUNTING *****************************/

#ifdef CHERI_CAPREVOKE_STATS
#define CAPREVOKE_STATS_FOR(st, crc)	struct caprevoke_stats *st = crc->stats
#define CAPREVOKE_STATS_INC(st, ctr, d)	do { (st)->ctr += (d); } while(0)
#else
#define CAPREVOKE_STATS_FOR(st, crc)	do { } while(0)
#define CAPREVOKE_STATS_INC(st, ctr, d)	do { } while(0)
#endif
#define CAPREVOKE_STATS_BUMP(st, ctr)	CAPREVOKE_STATS_INC(st, ctr, 1)

#endif
#endif
