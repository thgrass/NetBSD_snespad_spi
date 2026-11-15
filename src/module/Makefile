.include "../Makefile.inc"

.PATH:	${S}/dev/spi

KMOD=	snespad
IOCONF=	snespad.ioconf

SRCS=	snespad.c

# number of pads/CS lines to support (driver default 4; match the
# enabled "snespadN at spi? slave N" instances in snespad.ioconf):
CPPFLAGS+= -DSNESPAD_MAX_PORTS=2

.include <bsd.kmodule.mk>
