/*-
 * Copyright (c) 2011-2013 Robert N. M. Watson
 * All rights reserved.
 *
 * This software was developed by SRI International and the University of
 * Cambridge Computer Laboratory under DARPA/AFRL contract (FA8750-10-C-0237)
 * ("CTSRD"), as part of the DARPA CRASH research programme.
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

#include "opt_ddb.h"

#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/proc.h>
#include <sys/syscall.h>
#include <sys/sysctl.h>

#include <ddb/ddb.h>
#include <sys/kdb.h>

#include <machine/atomic.h>
#include <machine/cheri.h>
#include <machine/pcb.h>
#include <machine/sysarch.h>

/*
 * Beginnings of a programming interface for explicitly managing capability
 * registers.  Convert back and forth between capability registers and
 * general-purpose registers/memory so that we can program the context,
 * save/restore application contexts, etc.
 *
 * In the future, we'd like the compiler to do this sort of stuff for us
 * based on language-level properties and annotations, but in the mean
 * time...
 *
 * XXXRW: Any manipulation of c0 should include a "memory" clobber for inline
 * assembler, so that the compiler will write back memory contents before the
 * call, and reload them afterwards.
 */

static SYSCTL_NODE(_security, OID_AUTO, cheri, CTLFLAG_RD, 0,
    "CHERI parameters and statistics");

/* XXXRW: Should possibly be u_long. */
static u_int	security_cheri_syscall_violations;
SYSCTL_UINT(_security_cheri, OID_AUTO, syscall_violations, CTLFLAG_RD,
    &security_cheri_syscall_violations, 0, "Number of system calls blocked");

static u_int	security_cheri_debugger_on_exception;
SYSCTL_UINT(_security_cheri, OID_AUTO, debugger_on_exception, CTLFLAG_RW,
    &security_cheri_debugger_on_exception, 0,
    "Run debugger on CHERI exception");

/*
 * XXXRW: Temporary hack -- single global trusted stack used by any instance
 * of CCall or CReturn in any thread of any process.  This won't last long,
 * but is enough to test things a little.
 */
struct cheri_stack_frame	cheri_tsc_hack;

/*
 * Capability memcpy() routine -- not a general-purpose memcpy() as it has
 * much stronger alignment and size requirements.
 *
 * XXXRW: Eventually, true memcpy() will support capabilities, and this will
 * go away.  We hope.
 */
void *
cheri_memcpy(void *dst, void *src, size_t len)
{
	register_t s;
	u_int i;

	/* NB: Assumes CHERICAP_SIZE is a power of two. */
	KASSERT(((uintptr_t)dst & (CHERICAP_SIZE - 1)) == 0,
	    ("%s: unaligned dst", __func__));
	KASSERT(((uintptr_t)src & (CHERICAP_SIZE - 1)) == 0,
	    ("%s: unaligned src", __func__));
	KASSERT((len % CHERICAP_SIZE) == 0,
	    ("%s: copy size not a multiple of capability size", __func__));

	/*
	 * XXXRW: Prevent preemption during memory copy, as we're using an
	 * exception handling temporary register.
	 */
	s = intr_disable();
	for (i = 0; i < (len / CHERICAP_SIZE); i++) {
		cheri_capability_load(CHERI_CR_CTEMP,
		    (struct chericap *)src + i);
		cheri_capability_store(CHERI_CR_CTEMP,
		    (struct chericap *)dst + i);
	}
	intr_restore(s);
	return (dst);
}

/*
 * Given an existing more privileged capability (fromcrn), build a new
 * capability in tocrn with the contents of the passed flattened
 * representation.
 *
 * XXXRW: It's not yet clear how important ordering is here -- try to do the
 * privilege downgrade in a way that will work when doing an "in place"
 * downgrade, with permissions last.
 *
 * XXXRW: How about the unsealed bit?
 */

void
cheri_capability_set(struct chericap *cp, uint32_t perms,
    void *otypep /* eaddr */, void *basep, uint64_t length)
{
	register_t s;

	/*
	 * XXXRW: For now, we're using an exception handling temporary
	 * register to construct capabilities to store.  Disable interrupts so
	 * that this is safe.  In the future, we'd like to use a general
	 * temporary preserved during kernel execution to avoid this.
	 */
	s = intr_disable();
	CHERI_CINCBASE(CHERI_CR_CTEMP, CHERI_CR_KDC, (register_t)basep);
	CHERI_CSETLEN(CHERI_CR_CTEMP, CHERI_CR_CTEMP, (register_t)length);
	CHERI_CANDPERM(CHERI_CR_CTEMP, CHERI_CR_CTEMP, (register_t)perms);
	CHERI_CSETTYPE(CHERI_CR_CTEMP, CHERI_CR_CTEMP, (register_t)otypep);
	CHERI_CSC(CHERI_CR_CTEMP, CHERI_CR_KDC, (register_t)cp, 0);
	intr_restore(s);
}

static void
cheri_capability_clear(struct chericap *cp)
{

	/*
	 * While we could construct a non-capability and write it out, simply
	 * bzero'ing memory is sufficient to clear the tag bit, and easier to
	 * spell.
	 */
	bzero(cp, sizeof(*cp));

}

/*
 * Functions to store a common set of capability values to in-memory
 * capabilities: full privilege, userspace privilege, and null privilege.
 * These are used to initialise capability registers when creating new
 * contexts.
 */
void
cheri_capability_set_priv(struct chericap *cp)
{

	cheri_capability_set(cp, CHERI_CAP_PRIV_PERMS, CHERI_CAP_PRIV_OTYPE,
	    CHERI_CAP_PRIV_BASE, CHERI_CAP_PRIV_LENGTH);
}

void
cheri_capability_set_user(struct chericap *cp)
{

	cheri_capability_set(cp, CHERI_CAP_USER_PERMS, CHERI_CAP_USER_OTYPE,
	    CHERI_CAP_USER_BASE, CHERI_CAP_USER_LENGTH);
}

void
cheri_capability_set_null(struct chericap *cp)
{

	cheri_capability_clear(cp);
}

/*
 * Because contexts contain tagged capabilities, we can't just use memcpy()
 * on the data structure.  Once the C compiler knows about capabilities, then
 * direct structure assignment should be plausible.  In the mean time, an
 * explicit capability context copy routine is required.
 *
 * XXXRW: Compiler should know how to do copies of tagged capabilities.
 *
 * XXXRW: Compiler should be providing us with the temporary register.
 */
void
cheri_capability_copy(struct chericap *cp_to, struct chericap *cp_from)
{
	register_t s;

	/*
	 * XXXRW: For now, we're using an exception handling temporary
	 * register to construct capabilities to store.  Disable interrupts so
	 * that this is safe.  In the future, we'd like to use a general
	 * temporary preserved during kernel execution to avoid this.
	 */
	s = intr_disable();
	cheri_capability_load(CHERI_CR_CTEMP, cp_from);
	cheri_capability_store(CHERI_CR_CTEMP, cp_to);
	intr_restore(s);
}

void
cheri_context_copy(struct pcb *dst, struct pcb *src)
{

	cheri_memcpy(&dst->pcb_cheriframe, &src->pcb_cheriframe,
	    sizeof(dst->pcb_cheriframe));
}

void
cheri_exec_setregs(struct thread *td)
{
	struct cheri_frame *cfp;

	/*
	 * XXXRW: Experimental CHERI ABI initialises $c0 with full user
	 * privilege, and all other user-accessible capability registers with
	 * no rights at all.  The runtime linker/compiler/application can
	 * propagate around rights as required.
	 */
	cfp = &td->td_pcb->pcb_cheriframe;
	bzero(cfp, sizeof(*cfp));
	cheri_capability_set_user(&cfp->cf_c0);
	cheri_capability_set_user(&cfp->cf_pcc);

	/* XXXRW: Trusted stack initialisation here? */
}

#define	CHERI_REG_PRINT(c, ctag, num) do {				\
	printf("C%u t: %u u: %u perms %04jx otype %016jx\n", num,	\
	    ctag, c.c_unsealed, (uintmax_t)c.c_perms,			\
	    (uintmax_t)c.c_otype);					\
	printf("\tbase %016jx length %016jx\n", (uintmax_t)c.c_base,	\
	    (uintmax_t)c.c_length);					\
} while (0)

void
cheri_log_exception(struct trapframe *frame, int trap_type)
{
	struct cheri_frame *cheriframe;
	struct chericap c;
	register_t cause;
	u_int ctag;
	uint8_t exccode, regnum;

#ifdef SMP
	printf("cpuid = %d\n", PCPU_GET(cpuid));
#endif
	CHERI_CGETCAUSE(cause);
	exccode = (cause >> 8) & 0xff;
	regnum = cause & 0x1f;
	printf("CHERI cause: ExcCode: %02x RegNum: %02x\n", exccode, regnum);

	/* XXXRW: awkward and unmaintainable pointer construction. */
	cheriframe = &(((struct pcb *)frame)->pcb_cheriframe);

	/* C0 */
	intr_disable();
	CHERI_CLC(CHERI_CR_CTEMP, CHERI_CR_KDC, &cheriframe->cf_c0, 0);
	CHERI_GETCAPREG(CHERI_CR_CTEMP, c);
	CHERI_CGETTAG(ctag, CHERI_CR_CTEMP);
	intr_enable();
	CHERI_REG_PRINT(c, ctag, 0);

	/* C1 */
	intr_disable();
	CHERI_CLC(CHERI_CR_CTEMP, CHERI_CR_KDC, &cheriframe->cf_c1, 0);
	CHERI_GETCAPREG(CHERI_CR_CTEMP, c);
	CHERI_CGETTAG(ctag, CHERI_CR_CTEMP);
	intr_enable();
	CHERI_REG_PRINT(c, ctag, 1);

	/* C2 */
	intr_disable();
	CHERI_CLC(CHERI_CR_CTEMP, CHERI_CR_KDC, &cheriframe->cf_c2, 0);
	CHERI_GETCAPREG(CHERI_CR_CTEMP, c);
	CHERI_CGETTAG(ctag, CHERI_CR_CTEMP);
	intr_enable();
	CHERI_REG_PRINT(c, ctag, 2);

	/* C3 */
	intr_disable();
	CHERI_CLC(CHERI_CR_CTEMP, CHERI_CR_KDC, &cheriframe->cf_c3, 0);
	CHERI_GETCAPREG(CHERI_CR_CTEMP, c);
	CHERI_CGETTAG(ctag, CHERI_CR_CTEMP);
	intr_enable();
	CHERI_REG_PRINT(c, ctag, 3);

	/* C24 - RCC */
	intr_disable();
	CHERI_CLC(CHERI_CR_CTEMP, CHERI_CR_KDC, &cheriframe->cf_rcc, 0);
	CHERI_GETCAPREG(CHERI_CR_CTEMP, c);
	CHERI_CGETTAG(ctag, CHERI_CR_CTEMP);
	intr_enable();
	CHERI_REG_PRINT(c, ctag, 24);

	/* C26 - IDC */
	intr_disable();
	CHERI_CLC(CHERI_CR_CTEMP, CHERI_CR_KDC, &cheriframe->cf_idc, 0);
	CHERI_GETCAPREG(CHERI_CR_CTEMP, c);
	CHERI_CGETTAG(ctag, CHERI_CR_CTEMP);
	intr_enable();
	CHERI_REG_PRINT(c, ctag, 26);

	/* C31 - saved PCC */
	intr_disable();
	CHERI_CLC(CHERI_CR_CTEMP, CHERI_CR_KDC, &cheriframe->cf_pcc, 0);
	CHERI_GETCAPREG(CHERI_CR_CTEMP, c);
	CHERI_CGETTAG(ctag, CHERI_CR_CTEMP);
	intr_enable();
	CHERI_REG_PRINT(c, ctag, 31);

#if DDB
	if (security_cheri_debugger_on_exception)
		kdb_enter(KDB_WHY_CHERI, "CHERI exception");
#endif
}

/*
 * Only allow most system calls from sandboxes that hold ambient authority in
 * userspace.
 */
int
cheri_syscall_authorize(struct thread *td, u_int code, int nargs,
    register_t *args)
{
	struct chericap c;

	/*
	 * Allow the cycle counter to be read via sysarch.
	 */
	if (code == SYS_sysarch && args[0] == MIPS_GET_COUNT)
		return (0);

	/*
	 * Allow threading primitives to be used.
	 */
	if (code == SYS__umtx_lock || code == SYS__umtx_unlock ||
	    code == SYS__umtx_op)
		return (0);

	/*
	 * Check whether userspace holds the rights defined in
	 * cheri_capability_set_user() in $C0.  Note that object type is
	 * We might also consider checking $PCC here.
	 *
	 * XXXRW: Possibly ECAPMODE should be EPROT or ESANDBOX?
	 */
	intr_disable();
	CHERI_CLC(CHERI_CR_CTEMP, CHERI_CR_KDC,
	    &td->td_pcb->pcb_cheriframe.cf_c0, 0);
	CHERI_GETCAPREG(CHERI_CR_CTEMP, c);
	intr_enable();
	if (c.c_perms != CHERI_CAP_USER_PERMS ||
	    c.c_base != CHERI_CAP_USER_BASE ||
	    c.c_length != CHERI_CAP_USER_LENGTH) {
		atomic_add_int(&security_cheri_syscall_violations, 1);
		return (ECAPMODE);
	}
	return (0);
}

#ifdef DDB
#define	DB_CHERI_REG_PRINT_NUM(crn, num) do {				\
	struct chericap c;						\
	u_int ctag;							\
									\
	CHERI_GETCAPREG((crn), c);					\
	CHERI_CGETTAG(ctag, (crn));					\
	db_printf("C%u t: %u u: %u perms %04jx otype %016jx\n", num,	\
	    ctag, c.c_unsealed, (uintmax_t)c.c_perms,			\
	    (uintmax_t)c.c_otype);					\
	db_printf("\tbase %016jx length %016jx\n", (uintmax_t)c.c_base,	\
	    (uintmax_t)c.c_length);					\
} while (0)

#define	DB_CHERI_REG_PRINT(crn)	 DB_CHERI_REG_PRINT_NUM(crn, crn)

/*
 * Variation that prints live register state from the capability coprocessor.
 */
DB_SHOW_COMMAND(cheri, ddb_dump_cheri)
{
	register_t cause;

	db_printf("CHERI registers\n");
	DB_CHERI_REG_PRINT(0);
	DB_CHERI_REG_PRINT(1);
	DB_CHERI_REG_PRINT(2);
	DB_CHERI_REG_PRINT(3);
	DB_CHERI_REG_PRINT(4);
	DB_CHERI_REG_PRINT(5);
	DB_CHERI_REG_PRINT(6);
	DB_CHERI_REG_PRINT(7);
	DB_CHERI_REG_PRINT(8);
	DB_CHERI_REG_PRINT(9);
	DB_CHERI_REG_PRINT(10);
	DB_CHERI_REG_PRINT(11);
	DB_CHERI_REG_PRINT(12);
	DB_CHERI_REG_PRINT(13);
	DB_CHERI_REG_PRINT(14);
	DB_CHERI_REG_PRINT(15);
	DB_CHERI_REG_PRINT(16);
	DB_CHERI_REG_PRINT(17);
	DB_CHERI_REG_PRINT(18);
	DB_CHERI_REG_PRINT(19);
	DB_CHERI_REG_PRINT(20);
	DB_CHERI_REG_PRINT(21);
	DB_CHERI_REG_PRINT(22);
	DB_CHERI_REG_PRINT(23);
	DB_CHERI_REG_PRINT(24);
	DB_CHERI_REG_PRINT(25);
	DB_CHERI_REG_PRINT(26);
	DB_CHERI_REG_PRINT(27);
	DB_CHERI_REG_PRINT(28);
	DB_CHERI_REG_PRINT(29);
	DB_CHERI_REG_PRINT(30);
	DB_CHERI_REG_PRINT(31);
	CHERI_CGETCAUSE(cause);
	db_printf("CHERI cause: ExcCode: %02x RegNum: %02x\n",
	    (uint8_t)((cause >> 8) & 0xff), (uint8_t)(cause & 0x1f));
}

/*
 * Variation that prints the saved userspace CHERI register frame for a
 * thread.
 */
DB_SHOW_COMMAND(cheriframe, ddb_dump_cheriframe)
{
	struct thread *td;
	struct cheri_frame *cfp;
	register_t s;
	u_int i;

	if (have_addr)
		td = db_lookup_thread(addr, TRUE);
	else
		td = curthread;

	cfp = &td->td_pcb->pcb_cheriframe;
	db_printf("Thread %d at %p\n", td->td_tid, td);
	db_printf("CHERI frame at %p\n", cfp);

	/* Laboriously load and print each user capability. */
	for (i = 0; i < 27; i++) {
		s = intr_disable();
		cheri_capability_load(CHERI_CR_CTEMP,
		    (struct chericap *)&cfp->cf_c0 + i);
		DB_CHERI_REG_PRINT_NUM(CHERI_CR_CTEMP, i);
		intr_restore(s);
	}
	db_printf("\nPCC:\n");
	s = intr_disable();
	cheri_capability_load(CHERI_CR_CTEMP, (struct chericap *)&cfp->cf_c0 +
	    CHERI_CR_PCC_OFF);
	DB_CHERI_REG_PRINT_NUM(CHERI_CR_CTEMP, CHERI_CR_EPCC);
	intr_restore(s);
}
#endif
