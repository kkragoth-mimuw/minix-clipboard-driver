# Makefile for the clipboard driver.
PROG=	clipboard
SRCS=	ps347277.c

DPADD+=	${LIBCHARDRIVER} ${LIBSYS}
LDADD+=	-lchardriver -lsys

.include <minix.service.mk>
