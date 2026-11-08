/*
 * cmmctl_ff.c — Fast-forward control CLI sub-commands
 *
 * cmmctl ff enable|disable              not supported on this hardware
 * cmmctl ff ipsec-frag enable|disable   not supported on this hardware
 *
 * Copyright 2026 Mono Technologies Inc.
 * Copyright (C) 2007 Mindspeed Technologies, Inc.
 * Copyright 2014-2016 Freescale Semiconductor, Inc.
 * Copyright 2017, 2021 NXP
 * SPDX-License-Identifier: GPL-2.0+
 */

#include <stdio.h>
#include <string.h>

#include "cmmctl.h"
#include "fpp.h"

static void
ff_usage(void)
{
	fprintf(stderr,
	    "usage: cmmctl ff <command>\n\n"
	    "Commands:\n"
	    "  enable                Not supported on this hardware\n"
	    "  disable               Not supported on this hardware\n"
	    "  ipsec-frag enable     Not supported on this hardware\n"
	    "  ipsec-frag disable    Not supported on this hardware\n");
}

/* CDX keeps the fast-forward switch but nothing reads it, so say so. */
static int
ff_control(int fd __unused, int enable)
{
	fprintf(stderr, "ff %s: not supported on this hardware, nothing "
	    "changed\n", enable ? "enable" : "disable");
	return (1);
}

/* CDX takes the IPsec pre-fragmentation setting but never applies it. */
static int
ff_ipsec_frag(int fd __unused, int enable)
{
	fprintf(stderr, "ff ipsec-frag %s: not supported on this hardware, "
	    "nothing changed\n", enable ? "enable" : "disable");
	return (1);
}

int
cmmctl_ff_main(int argc, char **argv, int fd)
{

	if (argc < 1) {
		ff_usage();
		return (1);
	}

	if (strcmp(argv[0], "enable") == 0)
		return (ff_control(fd, 1));

	if (strcmp(argv[0], "disable") == 0)
		return (ff_control(fd, 0));

	if (strcmp(argv[0], "ipsec-frag") == 0) {
		if (argc < 2) {
			fprintf(stderr,
			    "usage: cmmctl ff ipsec-frag enable|disable\n");
			return (1);
		}
		if (strcmp(argv[1], "enable") == 0)
			return (ff_ipsec_frag(fd, 1));
		if (strcmp(argv[1], "disable") == 0)
			return (ff_ipsec_frag(fd, 0));
		fprintf(stderr, "unknown ipsec-frag action: %s\n", argv[1]);
		return (1);
	}

	fprintf(stderr, "unknown ff command: %s\n", argv[0]);
	ff_usage();
	return (1);
}
