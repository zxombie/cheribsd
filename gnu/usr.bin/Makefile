# $FreeBSD$

.include <src.opts.mk>

.if ${MK_GDB} != "no" || ${MK_BINUTILS} != "no"
SUBDIR+=	binutils
.endif

SUBDIR.${MK_DIALOG}+=	dialog
SUBDIR.${MK_GNU_DIFF}+=	diff3
SUBDIR.${MK_GNU_GREP}+=	grep
SUBDIR.${MK_TESTS}+=	tests

SUBDIR_PARALLEL=

.include <bsd.subdir.mk>
