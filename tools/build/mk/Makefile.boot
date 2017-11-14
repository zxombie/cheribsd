# $FreeBSD$

.include <host-target.mk>

.if ${HOST_OS} == "FreeBSD"
CFLAGS+=	-I${WORLDTMP}/legacy/usr/include
DPADD+=		${WORLDTMP}/legacy/usr/lib/libegacy.a
LDADD+=		-legacy
LDFLAGS+=	-L${WORLDTMP}/legacy/usr/lib
.elif ${HOST_OS} == "Linux"
CFLAGS+=	-I/usr/include/bsd -DLIBBSD_OVERLAY=1 -D_GNU_SOURCE=1 -D__unused= -DEFTYPE=EINVAL
LDFLAGS+=	-lbsd
NO_SHARED=	no
.elif ${HOST_OS} == "Darwin"
CFLAGS+=	"-D__packed=__attribute__((packed))" -D_DARWIN_C_SOURCE=1
NO_SHARED=	no
# CheriBSD adds --no-warn-mismatch to the LDFLAGS, but the OSX linker is not
# compatible with the bfd flags
# LDFLAGS:=${LDFLAGS:N-Wl,--no-warn-mismatch}
LDFLAGS:=
.endif
.info "Building ${.CURDIR} for ${.MAKE.OS}"
# we do not want to capture dependencies referring to the above
UPDATE_DEPENDFILE= no
