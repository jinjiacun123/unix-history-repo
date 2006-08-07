/*
 * Copyright (c) 1994, Paul Richards.
 *
 * All rights reserved.
 *
 * This software may be used, modified, copied, distributed, and sold, in both
 * source and binary form provided that the above copyright and these terms
 * are retained, verbatim, as the first lines of this file.  Under no
 * circumstances is the author responsible for the proper functioning of this
 * software, nor does the author assume any responsibility for damages
 * incurred with its use.
 *
 * $FreeBSD$
 */

#include "sade.h"
#include <stdarg.h>
#include <fcntl.h>
#include <sys/errno.h>
#include <sys/ioctl.h>
#include <sys/consio.h>

#define VTY_STATUS_LINE    24
#define TTY_STATUS_LINE    23

static void
prompt_term(char **termp, char **termcapp)
{
    char str[80];
    static struct {
	const char *term, *termcap;
    } lookup[] = { { "ansi", termcap_ansi },
		   { "vt100", termcap_vt100 },
		   { "cons25", termcap_cons25 },
		   { "cons25-m", termcap_cons25_m },
		   { "xterm", termcap_xterm },
		   { "cons25w", termcap_cons25w } }; /* must be last */

	printf("\nPlease set your TERM variable before running this program.\n");
	printf("Defaulting to an ANSI compatible terminal - please press RETURN\n");
	fgets(str, 80, stdin);	/* Just to make it interactive */
	*termp = (char *)"ansi";
	*termcapp = (char *)termcap_ansi;
}

int
set_termcap(void)
{
    char           *term;
    int		   stat;
    struct ttysize ts;

    term = getenv("TERM");
    stat = ioctl(STDERR_FILENO, GIO_COLOR, &ColorDisplay);

	if (isDebug())
	    DebugFD = open("sysinstall.debug", O_WRONLY|O_CREAT|O_TRUNC, 0644);
	else
	    DebugFD = -1;
	if (DebugFD < 0)
	    DebugFD = open("/dev/null", O_RDWR, 0);

    if (!OnVTY || (stat < 0)) {
	if (!term) {
	    char *term, *termcap;

	    prompt_term(&term, &termcap);
	    if (setenv("TERM", term, 1) < 0)
		return -1;
	    if (setenv("TERMCAP", termcap, 1) < 0)
		return -1;
	}
	if (DebugFD < 0)
	    DebugFD = open("/dev/null", O_RDWR, 0);
    }
    else {
	int i, on;

	if (getpid() == 1) {
	    DebugFD = open("/dev/ttyv1", O_WRONLY);
	    if (DebugFD != -1) {
		on = 1;
		i = ioctl(DebugFD, TIOCCONS, (char *)&on);
		msgDebug("ioctl(%d, TIOCCONS, NULL) = %d (%s)\n",
			 DebugFD, i, !i ? "success" : strerror(errno));
	    }
	}

#ifdef PC98
	if (!term) {
	    if (setenv("TERM", "cons25w", 1) < 0)
		return -1;
	    if (setenv("TERMCAP", termcap_cons25w, 1) < 0)
		return -1;
	}
#else
	if (ColorDisplay) {
	    if (!term) {
		if (setenv("TERM", "cons25", 1) < 0)
		    return -1;
		if (setenv("TERMCAP", termcap_cons25, 1) < 0)
		    return -1;
	    }
	}
	else {
	    if (!term) {
		if (setenv("TERM", "cons25-m", 1) < 0)
		    return -1;
		if (setenv("TERMCAP", termcap_cons25_m, 1) < 0)
		    return -1;
	    }
	}
#endif
    }
    if (ioctl(0, TIOCGSIZE, &ts) == -1) {
	msgDebug("Unable to get terminal size - errno %d\n", errno);
	ts.ts_lines = 0;
    }
    StatusLine = ts.ts_lines ? ts.ts_lines - 1: (OnVTY ? VTY_STATUS_LINE : TTY_STATUS_LINE);
    return 0;
}
