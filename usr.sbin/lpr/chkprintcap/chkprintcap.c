/*
 * Copyright 1997 Massachusetts Institute of Technology
 *
 * Permission to use, copy, modify, and distribute this software and
 * its documentation for any purpose and without fee is hereby
 * granted, provided that both the above copyright notice and this
 * permission notice appear in all copies, that both the above
 * copyright notice and this permission notice appear in all
 * supporting documentation, and that the name of M.I.T. not be used
 * in advertising or publicity pertaining to distribution of the
 * software without specific, written prior permission.  M.I.T. makes
 * no representations about the suitability of this software for any
 * purpose.  It is provided "as is" without express or implied
 * warranty.
 * 
 * THIS SOFTWARE IS PROVIDED BY M.I.T. ``AS IS''.  M.I.T. DISCLAIMS
 * ALL EXPRESS OR IMPLIED WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE. IN NO EVENT
 * SHALL M.I.T. BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF
 * USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

static const char copyright[] =
	"Copyright (C) 1997, Massachusetts Institute of Technology\r\n";
static const char rcsid[] =
	"$Id$";

#include <sys/types.h>
#include <sys/queue.h>
#include <sys/stat.h>

#include <err.h>
#include <errno.h>
#include <grp.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/param.h>		/* needed for lp.h but not used here */
#include <dirent.h>		/* ditto */
#include "lp.h"
#include "lp.local.h"

static	void check_spool_dirs(void);
static	int interpret_error(const struct printer *pp, int error);
static	void make_spool_dir(const struct printer *pp);
static	void note_spool_dir(const struct printer *pp, const struct stat *st);
static	void usage(void) __dead2;

static	int problems;		/* number of problems encountered */

/*
 * chkprintcap - check the printcap file for syntactic and semantic errors
 * Returns the number of problems found.
 */
int
main(int argc, char **argv)
{
	int c, error, makedirs, more;
	struct printer myprinter, *pp;

	makedirs = 0;
	pp = &myprinter;

	while ((c = getopt(argc, argv, "df:")) != -1) {
		switch (c) {
		case 'd':
			makedirs = 1;
			break;

		case 'f':
			setprintcap(optarg);
			break;

		default:
			usage();
		}
	}

	if (optind != argc)
		usage();

	more = firstprinter(pp, &error);
	if (interpret_error(pp, error) && more)
		goto next;

	while (more) {
		struct stat stab;

		errno = 0;
		if (stat(pp->spool_dir, &stab) < 0) {
			if (errno == ENOENT && makedirs) {
				make_spool_dir(pp);
			} else {
				problems++;
				warn("%s: %s", pp->printer, pp->spool_dir);
			}
		} else {
			note_spool_dir(pp, &stab);
		}

		/* Make other validity checks here... */

next:
		more = nextprinter(pp, &error);
		if (interpret_error(pp, error) && more)
			goto next;
	}
	check_spool_dirs();
	return problems;
}

/*
 * Interpret the error code.  Returns 1 if we should skip to the next
 * record (as this record is unlikely to make sense).  If the problem
 * is very severe, exit.  Otherwise, return zero.
 */
static int
interpret_error(const struct printer *pp, int error)
{
	switch(error) {
	case PCAPERR_OSERR:
		err(++problems, "reading printer database");
	case PCAPERR_TCLOOP:
		++problems;
		warnx("%s: loop detected in tc= expansion", pp->printer);
		return 1;
	case PCAPERR_TCOPEN:
		warnx("%s: unresolved tc= expansion", pp->printer);
		return 1;
	case PCAPERR_SUCCESS:
		break;
	default:
		errx(++problems, "unknown printcap library error %d", error);
	}
	return 0;
}

/*
 * Keep the list of spool directories.  Note that we don't whine
 * until all spool directories are noted, so that all of the more serious
 * problems are noted first.  We keep the list sorted by st_dev and
 * st_ino, so that the problem spool directories can be noted in
 * a single loop.
 */
struct	dirlist {
	LIST_ENTRY(dirlist) link;
	struct stat stab;
	char *path;
	char *printer;
};

static	LIST_HEAD(, dirlist) dirlist;

static int
lessp(const struct dirlist *a, const struct dirlist *b)
{
	if (a->stab.st_dev == b->stab.st_dev)
		return a->stab.st_ino < b->stab.st_ino;
	return a->stab.st_dev < b->stab.st_dev;
}

static int
equal(const struct dirlist *a, const struct dirlist *b)
{
	return ((a->stab.st_dev == b->stab.st_dev)
		&& (a->stab.st_ino == b->stab.st_ino));
}

static void
note_spool_dir(const struct printer *pp, const struct stat *st)
{
	struct dirlist *dp, *dp2, *last;

	dp = malloc(sizeof *dp);
	if (dp == 0)
		err(++problems, "malloc(%lu)", (u_long)sizeof *dp);
	
	dp->stab = *st;
	dp->printer = strdup(pp->printer);
	if (dp->printer == 0)
		err(++problems, "malloc(%lu)", strlen(pp->printer) + 1UL);
	dp->path = strdup(pp->spool_dir);
	if (dp->path == 0)
		err(++problems, "malloc(%lu)", strlen(pp->spool_dir) + 1UL);
	
	last = 0;
	dp2 = dirlist.lh_first;
	while (dp2 && lessp(dp, dp2)) {
		last = dp2;
		dp2 = dp2->link.le_next;
	}

	if (last) {
		LIST_INSERT_AFTER(last, dp, link);
	} else {
		LIST_INSERT_HEAD(&dirlist, dp, link);
	}
}

static void
check_spool_dirs(void)
{
	struct dirlist *dp, *dp2;

	for (dp = dirlist.lh_first; dp; dp = dp2) {
		dp2 = dp->link.le_next;

		if (dp2 != 0 && equal(dp, dp2)) {
			++problems;
			if (strcmp(dp->path, dp2->path) == 0) {
				warnx("%s and %s share the same spool, %s",
				      dp->printer, dp2->printer, dp->path);
			} else {
				warnx("%s (%s) and %s (%s) are the same "
				      "directory", dp->path, dp->printer,
				      dp2->path, dp2->printer);
			}
			continue;
		}
		/* Should probably check owners and modes here. */
	}
}

#ifndef SPOOL_DIR_MODE
#define	SPOOL_DIR_MODE	(S_IRUSR | S_IWUSR | S_IXUSR \
			 | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH)
#endif

static void
make_spool_dir(const struct printer *pp)
{
	char *sd = pp->spool_dir;
	struct group *gr;
	struct stat stab;

	if (mkdir(sd, S_IRUSR | S_IXUSR) < 0) {
		problems++;
		warn("%s: mkdir %s", pp->printer, sd);
		return;
	}
	gr = getgrnam("daemon");
	if (gr == 0)
		errx(++problems, "cannot locate daemon group");

	if (chown(sd, pp->daemon_user, gr->gr_gid) < 0) {
		++problems;
		warn("%s: cannot change ownership to %ld:%ld", sd,
		     (long)pp->daemon_user, (long)gr->gr_gid);
		return;
	}

	if (chmod(sd, SPOOL_DIR_MODE) < 0) {
		++problems;
		warn("%s: cannot change mode to %lo", (long)SPOOL_DIR_MODE);
		return;
	}
	if (stat(sd, &stab) < 0)
		err(++problems, "stat: %s", sd);

	note_spool_dir(pp, &stab);
}

static void
usage(void)
{
	fprintf(stderr, "usage:\n\tchkprintcap [-d] [-f printcapfile]\n");
	exit(1);
}
