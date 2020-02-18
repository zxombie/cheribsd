/*-
 * Copyright (c) 2014 Andrew Turner
 * Copyright (c) 2015 Ruslan Bukin <br@bsdpad.com>
 * All rights reserved.
 *
 * Portions of this software were developed by SRI International and the
 * University of Cambridge Computer Laboratory under DARPA/AFRL contract
 * FA8750-10-C-0237 ("CTSRD"), as part of the DARPA CRASH research programme.
 *
 * Portions of this software were developed by the University of Cambridge
 * Computer Laboratory as part of the CTSRD Project, with support from the
 * UK Higher Education Innovation Fund (HEIF).
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
 * $FreeBSD$
 */

#include <sys/syscall.h>
#include <machine/asm.h>

#define	_SYSCALL(name)						\
	li	t0, SYS_ ## name;				\
	ecall

#ifdef __CHERI_PURE_CAPABILITY__
#define _GET_FNPTR(outreg, function)	\
	clgc CAPABILITY_REG(outreg), _C_LABEL(function)
#define _GET_LOCAL_FNPTR(outreg, function)	\
	cllc CAPABILITY_REG(outreg), _C_LABEL(function)
#define _CALL_FNPTR(reg)	cjalr CAPABILITY_REG(reg)
#define _TAILCALL_FNPTR(reg)	cjr CAPABILITY_REG(reg)
#define _RETURN	cret
#else
#define _GET_LOCAL_FNPTR(outreg, function)	\
	lla outreg, _C_LABEL(function)
#define _GET_FNPTR(outreg, function)	\
	la outreg, _C_LABEL(function)
#define _CALL_FNPTR(reg)	jalr reg
#define _TAILCALL_FNPTR(reg)	jr reg
#define _RETURN	ret
#endif

#define ASM_LOCAL_TAILCALL(tmpreg, function)				\
	_GET_LOCAL_FNPTR(tmpreg, function);				\
	_TAILCALL_FNPTR(tmpreg)
#define ASM_LOCAL_CALL(tmpreg, function)				\
	_GET_LOCAL_FNPTR(tmpreg, function);				\
	_CALL_FNPTR(tmpreg)

#define	SYSCALL(name)						\
ENTRY(__sys_##name);						\
	WEAK_REFERENCE(__sys_##name, name);			\
	WEAK_REFERENCE(__sys_##name, _##name);			\
	_SYSCALL(name);						\
	_RETURN;						\
END(__sys_##name)

#define	PSEUDO(name)						\
ENTRY(__sys_##name);						\
	WEAK_REFERENCE(__sys_##name, _##name);			\
	_SYSCALL(name);						\
	bnez	t0, 1f; 					\
	_RETURN;						\
1:	ASM_LOCAL_TAILCALL(t1, cerror);				\
END(__sys_##name)

#define	RSYSCALL(name)						\
ENTRY(__sys_##name);						\
	WEAK_REFERENCE(__sys_##name, name);			\
	WEAK_REFERENCE(__sys_##name, _##name);			\
	_SYSCALL(name);						\
	bnez	t0, 1f; 					\
	_RETURN;						\
1:	ASM_LOCAL_TAILCALL(t1, cerror);				\
END(__sys_##name)

/* Do a system call where the _x() is also custom (e.g. fcntl, ioctl) */
#define NO_UNDERSCORE(name)					\
ENTRY(__sys_##name);						\
	_SYSCALL(name);						\
	bnez	t0, 1f; 					\
	_RETURN;						\
1:	ASM_LOCAL_TAILCALL(t1, cerror);				\
END(__sys_##name)
