/*
 * Copyright (c) 2010, Frank Lahm <franklahm@googlemail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif /* HAVE_CONFIG_H */

#include <errno.h>
#include <libgen.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <bstrlib.h>

#include <atalk/adouble.h>
#include <atalk/queue.h>
#include <atalk/unix.h>
#include <atalk/util.h>
#include <atalk/vfs.h>
#include <atalk/volume.h>

#include "ad.h"

#define STRIP_TRAILING_SLASH(p) {                                   \
        while ((p).p_end > (p).p_path + 1 && (p).p_end[-1] == '/')  \
            *--(p).p_end = 0;                                       \
    }

static int fflg, iflg, nflg, vflg;

static afpvol_t svolume, dvolume;
static cnid_t did, pdid;
static volatile sig_atomic_t alarmed;

static int copy(const char *, const char *);
static int do_move(const char *, const char *);

/*
  SIGNAL handling:
  catch SIGINT and SIGTERM which cause clean exit. Ignore anything else.
*/

static void sig_handler(int signo _U_)
{
    alarmed = 1;
    return;
}

static void set_signal(void)
{
    struct sigaction sv;
    sv.sa_handler = sig_handler;
    sv.sa_flags = SA_RESTART;
    sigemptyset(&sv.sa_mask);

    if (sigaction(SIGTERM, &sv, NULL) < 0) {
        ERROR("error in sigaction(SIGTERM): %s", strerror(errno));
    }

    if (sigaction(SIGINT, &sv, NULL) < 0) {
        ERROR("error in sigaction(SIGINT): %s", strerror(errno));
    }

    memset(&sv, 0, sizeof(struct sigaction));
    sv.sa_handler = SIG_IGN;
    sigemptyset(&sv.sa_mask);

    if (sigaction(SIGABRT, &sv, NULL) < 0) {
        ERROR("error in sigaction(SIGABRT): %s", strerror(errno));
    }

    if (sigaction(SIGHUP, &sv, NULL) < 0) {
        ERROR("error in sigaction(SIGHUP): %s", strerror(errno));
    }

    if (sigaction(SIGQUIT, &sv, NULL) < 0) {
        ERROR("error in sigaction(SIGQUIT): %s", strerror(errno));
    }
}

static void usage_mv(void)
{
    printf(
        "Usage: ad mv [-f | -i | -n] [-v] source target\n"
        "       ad mv [-f | -i | -n] [-v] source ... directory\n\n"
        "Move files around within an AFP volume, updating the CNID\n"
        "database as needed. If either:\n"
        " - source or destination is not an AFP volume\n"
        " - source volume != destination volume\n"
        "the files are copied and removed from the source.\n\n"
        "The following options are available:\n\n"
        "   -f   Do not prompt for confirmation before overwriting the destination\n"
        "        path.  (The -f option overrides any previous -i or -n options.)\n"
        "   -i   Cause mv to write a prompt to standard error before moving a file\n"
        "        that would overwrite an existing file.  If the response from the\n"
        "        standard input begins with the character `y' or `Y', the move is\n"
        "        attempted.  (The -i option overrides any previous -f or -n\n"
        "        options.)\n"
        "   -n   Do not overwrite an existing file.  (The -n option overrides any\n"
        "        previous -f or -i options.)\n"
        "   -v   Cause mv to be verbose, showing files after they are moved.\n"
    );
    exit(EXIT_FAILURE);
}

int ad_mv(int argc, char *argv[], AFPObj *obj)
{
    size_t baselen;
    size_t len;
    int rval;
    char *endp;
    struct stat sb;
    int ch;
    char path[MAXPATHLEN];
    pdid = htonl(1);
    did = htonl(2);
    argc--;
    argv++;

    while ((ch = getopt(argc, argv, "finv")) != -1)
        switch (ch) {
        case 'i':
            iflg = 1;
            fflg = nflg = 0;
            break;

        case 'f':
            fflg = 1;
            iflg = nflg = 0;
            break;

        case 'n':
            nflg = 1;
            fflg = iflg = 0;
            break;

        case 'v':
            vflg = 1;
            break;

        default:
            usage_mv();
        }

    argc -= optind;
    argv += optind;

    if (argc < 2) {
        usage_mv();
    }

    set_signal();
    cnid_init();

    if (openvol(obj, argv[argc - 1], &dvolume) != 0) {
        SLOG("Error opening CNID database for source \"%s\": ", argv[argc - 1]);
        return 1;
    }

    /*
     * If the stat on the target fails or the target isn't a directory,
     * try the move.  More than 2 arguments is an error in this case.
     */
    if (stat(argv[argc - 1], &sb) || !S_ISDIR(sb.st_mode)) {
        if (argc > 2) {
            usage_mv();
        }

        if (openvol(obj, argv[0], &svolume) != 0) {
            SLOG("Error opening CNID database for destination \"%s\": ", argv[0]);
            return 1;
        }

        rval = do_move(argv[0], argv[1]);
        closevol(&svolume);
        closevol(&dvolume);
        return 1;
    }

    /* It's a directory, move each file into it. */
    if (strlen(argv[argc - 1]) >= sizeof(path)) {
        SLOG("%s: destination pathname too long", argv[argc - 1]);
        return 1;
    }

    size_t copied = strlcpy(path, argv[argc - 1], sizeof(path));

    if (copied >= sizeof(path)) {
        SLOG("%s: destination pathname too long", argv[argc - 1]);
        return 1;
    }

    baselen = strlen(path);
    endp = &path[baselen];

    if (!baselen || *(endp - 1) != '/') {
        *endp++ = '/';
        ++baselen;
    }

    for (rval = 0; --argc; ++argv) {
        /*
         * Find the last component of the source pathname using basename
         */
        char *src_copy = strdup(*argv);

        if (src_copy == NULL) {
            SLOG("Memory allocation error");
            rval = 1;
            continue;
        }

        const char *base_name = basename(src_copy);
        len = strnlen(base_name, PATH_MAX);

        if ((baselen + len) >= PATH_MAX) {
            SLOG("%s: base name too long", base_name);
            free(src_copy);
            return 1;
        }

        if ((baselen + len) >= PATH_MAX) {
            SLOG("%s: destination pathname too long", *argv);
            rval = 1;
        } else {
            /* Copy safely with bounds checking */
            if (strlcpy(endp, base_name, PATH_MAX - baselen) >= (PATH_MAX - baselen)) {
                SLOG("%s: destination pathname too long", *argv);
                rval = 1;
            } else {
                openvol(obj, *argv, &svolume);

                if (do_move(*argv, path)) {
                    rval = 1;
                }

                closevol(&svolume);
            }
        }

        free(src_copy);
    }

    closevol(&dvolume);
    return rval;
}

static int do_move(const char *from, const char *to)
{
    struct stat sb;
    int ask, ch, first;

    /*
     * Check access.  If interactive and file exists, ask user if it
     * should be replaced.  Otherwise if file exists but isn't writable
     * make sure the user wants to clobber it.
     */
    if (!fflg && !access(to, F_OK)) {
        /* prompt only if source exist */
        if (lstat(from, &sb) == -1) {
            SLOG("%s: %s", from, strerror(errno));
            return 1;
        }

        ask = 0;

        if (nflg) {
            if (vflg) {
                printf("%s not overwritten\n", to);
            }

            return 0;
        } else if (iflg) {
            (void)fprintf(stderr, "overwrite %s? (y/n [n]) ", to);
            ask = 1;
        } else if (access(to, W_OK) && !stat(to, &sb)) {
            (void)fprintf(stderr, "override for %s? (y/n [n]) ", to);
            ask = 1;
        }

        if (ask) {
            first = ch = getchar();

            while (ch != '\n' && ch != EOF) {
                ch = getchar();
            }

            if (first != 'y' && first != 'Y') {
                (void)fprintf(stderr, "not overwritten\n");
                return 0;
            }
        }
    }

    int mustcopy = 0;

    /*
     * Besides the usual EXDEV we copy instead of moving if
     * 1) source AFP volume != dest AFP volume
     * 2) either source or dest isn't even an AFP volume
     */
    if (!svolume.vol->v_path
            || !dvolume.vol->v_path
            || strcmp(svolume.vol->v_path, dvolume.vol->v_path) != 0) {
        mustcopy = 1;
    }

    cnid_t cnid = 0;

    if (!mustcopy) {
        if ((cnid = cnid_for_path(svolume.vol->v_cdb, svolume.vol->v_path, from,
                                  &did)) == CNID_INVALID) {
            SLOG("Couldn't resolve CNID for %s", from);
            return -1;
        }

        int srcfd = open(from, O_RDONLY);

        if (srcfd == -1) {
            SLOG("Can't open %s: %s", from, strerror(errno));
            return -1;
        }

        if (fstat(srcfd, &sb) != 0) {
            close(srcfd);
            SLOG("Can't fstat %s: %s", from, strerror(errno));
            return -1;
        }

        if (renameat(AT_FDCWD, from, AT_FDCWD, to) != 0) {
            close(srcfd);

            if (errno == EXDEV) {
                mustcopy = 1;
                char path[MAXPATHLEN];

                /*
                 * If the source is a symbolic link and is on another
                 * filesystem, it can be recreated at the destination.
                 */
                if (lstat(from, &sb) == -1) {
                    SLOG("%s: %s", from, strerror(errno));
                    return -1;
                }

                if (!S_ISLNK(sb.st_mode)) {
                    /* Can't mv(1) a mount point. */
                    if (realpath(from, path) == NULL) {
                        SLOG("cannot resolve %s: %s: %s", from, path, strerror(errno));
                        return 1;
                    }
                }
            } else { /* != EXDEV */
                SLOG("rename %s to %s: %s", from, to, strerror(errno));
                return 1;
            }
        } /* rename != 0*/

        close(srcfd);

        switch (sb.st_mode & S_IFMT) {
        case S_IFREG:
            if (dvolume.vol->vfs->vfs_renamefile(dvolume.vol, -1, from, to) != 0) {
                SLOG("Error moving adouble file for %s", from);
                return -1;
            }

            break;

        case S_IFDIR:
            break;

        default:
            SLOG("Not a file or dir: %s", from);
            return -1;
        }

        /* get CNID of new parent dir */
        cnid_t newpdid, newdid;

        if ((newdid = cnid_for_paths_parent(&dvolume, to, &newpdid)) == CNID_INVALID) {
            SLOG("Couldn't resolve CNID for parent of %s", to);
            return -1;
        }

        if (stat(to, &sb) != 0) {
            SLOG("Can't stat %s: %s", to, strerror(errno));
            return 1;
        }

        char *p = strdup(to);
        char *name = basename(p);

        if (cnid_update(dvolume.vol->v_cdb, cnid, &sb, newdid, name,
                        strlen(name)) != 0) {
            SLOG("Can't update CNID for: %s", to);
            free(p);
            return 1;
        }

        free(p);
        struct adouble ad;
        ad_init(&ad, dvolume.vol);

        if (ad_open(&ad, to, S_ISDIR(sb.st_mode) ? (ADFLAGS_DIR | ADFLAGS_HF |
                    ADFLAGS_RDWR) : ADFLAGS_HF | ADFLAGS_RDWR) != 0) {
            SLOG("Error opening adouble for: %s", to);
            return 1;
        }

        ad_setid(&ad, sb.st_dev, sb.st_ino, cnid, newdid, dvolume.db_stamp);
        ad_flush(&ad);
        ad_close(&ad, ADFLAGS_HF);

        if (vflg) {
            printf("%s -> %s\n", from, to);
        }

        return 0;
    }

    if (mustcopy) {
        return copy(from, to);
    }

    /* If we get here it's an error */
    return -1;
}

static int copy(const char *from, const char *to)
{
    struct stat sb;
    int pid, status;

    if (lstat(to, &sb) == 0) {
        /* Destination path exists. */
        if (S_ISDIR(sb.st_mode)) {
            if (rmdir(to) != 0) {
                SLOG("rmdir %s: %s", to, strerror(errno));
                return 1;
            }
        } else {
            if (unlink(to) != 0) {
                SLOG("unlink %s: %s", to, strerror(errno));
                return 1;
            }
        }
    } else if (errno != ENOENT) {
        SLOG("%s: %s", to, strerror(errno));
        return 1;
    }

    /* Copy source to destination. */
    if (!(pid = fork())) {
        execl(_PATH_AD, "ad", "cp", vflg ? "-Rpv" : "-Rp", from, to, (char *)NULL);
        _exit(1);
    }

    while ((waitpid(pid, &status, 0)) == -1) {
        if (errno == EINTR) {
            continue;
        }

        SLOG("%s cp -R %s %s: waitpid: %s", _PATH_AD, from, to, strerror(errno));
        return 1;
    }

    if (!WIFEXITED(status)) {
        SLOG("%s cp -R %s %s: did not terminate normally", _PATH_AD, from, to);
        return 1;
    }

    switch (WEXITSTATUS(status)) {
    case 0:
        break;

    default:
        SLOG("%s cp -R %s %s: terminated with %d (non-zero) status",
             _PATH_AD, from, to, WEXITSTATUS(status));
        return 1;
    }

    /* Delete the source. */
    if (!(pid = fork())) {
        execl(_PATH_AD, "ad", "rm", "-R", from, (char *)NULL);
        _exit(1);
    }

    while ((waitpid(pid, &status, 0)) == -1) {
        if (errno == EINTR) {
            continue;
        }

        SLOG("%s rm -R %s: waitpid: %s", _PATH_AD, from, strerror(errno));
        return 1;
    }

    if (!WIFEXITED(status)) {
        SLOG("%s rm -R %s: did not terminate normally", _PATH_AD, from);
        return 1;
    }

    switch (WEXITSTATUS(status)) {
    case 0:
        break;

    default:
        SLOG("%s rm -R %s: terminated with %d (non-zero) status",
             _PATH_AD, from, WEXITSTATUS(status));
        return 1;
    }

    return 0;
}
