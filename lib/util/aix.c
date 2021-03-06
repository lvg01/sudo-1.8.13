/*
 * Copyright (c) 2008, 2010-2015 Todd C. Miller <Todd.Miller@courtesan.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <config.h>

#include <sys/types.h>
#include <sys/resource.h>

#include <stdio.h>
#ifdef STDC_HEADERS
# include <stdlib.h>
# include <stddef.h>
#else
# ifdef HAVE_STDLIB_H
#  include <stdlib.h>
# endif
#endif /* STDC_HEADERS */
#include <usersec.h>
#include <uinfo.h>

#define DEFAULT_TEXT_DOMAIN	"sudo"
#include "sudo_gettext.h"	/* must be included before sudo_compat.h */

#include "sudo_compat.h"
#include "sudo_alloc.h"
#include "sudo_fatal.h"
#include "sudo_debug.h"
#include "sudo_util.h"

#ifdef HAVE_GETUSERATTR

#ifndef HAVE_SETRLIMIT64
# define setrlimit64(a, b) setrlimit(a, b)
# define rlimit64 rlimit
# define rlim64_t rlim_t
# define RLIM64_INFINITY RLIM_INFINITY
#endif /* HAVE_SETRLIMIT64 */

#ifndef RLIM_SAVED_MAX
# define RLIM_SAVED_MAX	RLIM64_INFINITY
#endif

struct aix_limit {
    int resource;
    char *soft;
    char *hard;
    int factor;
};

static struct aix_limit aix_limits[] = {
    { RLIMIT_FSIZE, S_UFSIZE, S_UFSIZE_HARD, 512 },
    { RLIMIT_CPU, S_UCPU, S_UCPU_HARD, 1 },
    { RLIMIT_DATA, S_UDATA, S_UDATA_HARD, 512 },
    { RLIMIT_STACK, S_USTACK, S_USTACK_HARD, 512 },
    { RLIMIT_RSS, S_URSS, S_URSS_HARD, 512 },
    { RLIMIT_CORE, S_UCORE, S_UCORE_HARD, 512 },
    { RLIMIT_NOFILE, S_UNOFILE, S_UNOFILE_HARD, 1 }
};

static int
aix_getlimit(char *user, char *lim, int *valp)
{
    debug_decl(aix_getlimit, SUDO_DEBUG_UTIL)

    if (getuserattr(user, lim, valp, SEC_INT) != 0)
	debug_return_int(-1);
    debug_return_int(0);
}

static int
aix_setlimits(char *user)
{
    struct rlimit64 rlim;
    int val;
    size_t n;
    debug_decl(aix_setlimits, SUDO_DEBUG_UTIL)

    if (setuserdb(S_READ) != 0) {
	sudo_warn(U_("unable to open userdb"));
	debug_return_int(-1);
    }

    /*
     * For each resource limit, get the soft/hard values for the user
     * and set those values via setrlimit64().  Must be run as euid 0.
     */
    for (n = 0; n < sizeof(aix_limits) / sizeof(aix_limits[0]); n++) {
	/*
	 * We have two strategies, depending on whether or not the
	 * hard limit has been defined.
	 */
	if (aix_getlimit(user, aix_limits[n].hard, &val) == 0) {
	    rlim.rlim_max = val == -1 ? RLIM64_INFINITY : (rlim64_t)val * aix_limits[n].factor;
	    if (aix_getlimit(user, aix_limits[n].soft, &val) == 0)
		rlim.rlim_cur = val == -1 ? RLIM64_INFINITY : (rlim64_t)val * aix_limits[n].factor;
	    else
		rlim.rlim_cur = rlim.rlim_max;	/* soft not specd, use hard */
	} else {
	    /* No hard limit set, try soft limit, if it exists. */
	    if (aix_getlimit(user, aix_limits[n].soft, &val) == -1)
		continue;
	    rlim.rlim_cur = val == -1 ? RLIM64_INFINITY : (rlim64_t)val * aix_limits[n].factor;

	    /* Set hard limit per AIX /etc/security/limits documentation. */
	    switch (aix_limits[n].resource) {
		case RLIMIT_CPU:
		case RLIMIT_FSIZE:
		    rlim.rlim_max = rlim.rlim_cur;
		    break;
		case RLIMIT_STACK:
		    rlim.rlim_max = RLIM_SAVED_MAX;
		    break;
		default:
		    rlim.rlim_max = RLIM64_INFINITY;
		    break;
	    }
	}
	(void)setrlimit64(aix_limits[n].resource, &rlim);
    }
    enduserdb();
    debug_return_int(0);
}

#ifdef HAVE_SETAUTHDB

# if defined(HAVE_DECL_SETAUTHDB) && !HAVE_DECL_SETAUTHDB
int setauthdb(char *new, char *old);
# endif
# if defined(HAVE_DECL_USRINFO) && !HAVE_DECL_USRINFO
int usrinfo(int cmd, char *buf, int count);
# endif

/*
 * Look up administrative domain for user (SYSTEM in /etc/security/user) and
 * set it as the default for the process.  This ensures that password and
 * group lookups are made against the correct source (files, NIS, LDAP, etc).
 */
int
aix_setauthdb_v1(char *user)
{
    char *registry;
    debug_decl(aix_setauthdb, SUDO_DEBUG_UTIL)

    if (user != NULL) {
	if (setuserdb(S_READ) != 0) {
	    sudo_warn(U_("unable to open userdb"));
	    debug_return_int(-1);
	}
	if (getuserattr(user, S_REGISTRY, &registry, SEC_CHAR) == 0) {
	    if (setauthdb(registry, NULL) != 0) {
		sudo_warn(U_("unable to switch to registry \"%s\" for %s"),
		    registry, user);
		debug_return_int(-1);
	    }
	}
	enduserdb();
    }
    debug_return_int(0);
}

/*
 * Restore the saved administrative domain, if any.
 */
int
aix_restoreauthdb_v1(void)
{
    debug_decl(aix_setauthdb, SUDO_DEBUG_UTIL)

    if (setauthdb(NULL, NULL) != 0) {
	sudo_warn(U_("unable to restore registry"));
	debug_return_int(-1);
    }
    debug_return_int(0);
}
#endif

int
aix_prep_user_v1(char *user, const char *tty)
{
    char *info;
    int len;
    debug_decl(aix_setauthdb, SUDO_DEBUG_UTIL)

    /* set usrinfo, like login(1) does */
    len = sudo_easprintf(&info, "NAME=%s%cLOGIN=%s%cLOGNAME=%s%cTTY=%s%c",
	user, '\0', user, '\0', user, '\0', tty ? tty : "", '\0');
    (void)usrinfo(SETUINFO, info, len);
    sudo_efree(info);

#ifdef HAVE_SETAUTHDB
    /* set administrative domain */
    if (aix_setauthdb(user) != 0)
	debug_return_int(-1);
#endif

    /* set resource limits */
    if (aix_setlimits(user) != 0)
	debug_return_int(-1);

    debug_return_int(0);
}
#endif /* HAVE_GETUSERATTR */
