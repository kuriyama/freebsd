# $FreeBSD$

.PATH: ${.CURDIR}/../../../../geom/part ${.CURDIR}/../../../../geom

KMOD=	geom_part_bsd
SRCS=	g_part_bsd.c geom_bsd_enc.c

SRCS+=	bus_if.h device_if.h g_part_if.h

MFILES=	kern/bus_if.m kern/device_if.m geom/part/g_part_if.m

.include <bsd.kmod.mk>
