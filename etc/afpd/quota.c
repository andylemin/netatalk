/*
 *
 * Copyright (c) 1990,1993 Regents of The University of Michigan.
 * All Rights Reserved.  See COPYRIGHT.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif /* HAVE_CONFIG_H */

#ifndef NO_QUOTA_SUPPORT
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/param.h>
#include <unistd.h>
#include <fcntl.h>

#if defined(HAVE_SYS_MNTTAB_H) || defined(__svr4__)
#include <sys/mntent.h>
#endif

#include <atalk/logger.h>
#include <atalk/afp.h>
#include <atalk/compat.h>
#include <atalk/unix.h>
#include <atalk/util.h>

#include "auth.h"
#include "directory.h"
#include "volume.h"
#include "unix.h"

#ifdef HAVE_LIBQUOTA
#include <quota.h>


static int
getfreespace(const AFPObj *obj, struct vol *vol, VolSpace *bfree,
             VolSpace *btotal, id_t id, int idtype)
{
    uid_t prevuid;
    const char *msg;
    struct quotahandle *qh;
    struct quotakey qk;
    struct quotaval qv;
    time_t now;

    if (time(&now) == -1) {
        LOG(log_info, logtype_afpd, "time(): %s",
            strerror(errno));
        return -1;
    }

    become_root();
    /*
     * In a tidier world we might keep the quotahandle open for longer...
     */
    qh = quota_open(vol->v_path);

    if (qh == NULL) {
        if (errno == EOPNOTSUPP || errno == ENXIO) {
            /* no quotas on this volume */
            seteuid(prevuid);
            return 0;
        }

        LOG(log_info, logtype_afpd, "quota_open(%s): %s", vol->v_path,
            strerror(errno));
        seteuid(prevuid);
        return -1;
    }

    qk.qk_idtype = idtype;
    qk.qk_id = id;
    qk.qk_objtype = QUOTA_OBJTYPE_BLOCKS;

    if (quota_get(qh, &qk, &qv) < 0) {
        if (errno == ENOENT) {
            /* no quotas for this id */
            quota_close(qh);
            seteuid(prevuid);
            return 0;
        }

        msg = strerror(errno);
        LOG(log_info, logtype_afpd, "quota_get(%s, %s): %s",
            vol->v_path, quota_idtype_getname(qh, idtype), msg);
        quota_close(qh);
        seteuid(prevuid);
        return -1;
    }

    quota_close(qh);
    unbecome_root();

    if (qv.qv_usage >= qv.qv_hardlimit ||
            (qv.qv_usage >= qv.qv_softlimit && now > qv.qv_expiretime)) {
        *bfree = 0;
        *btotal = dbtob(qv.qv_usage);
    } else {
        *bfree = dbtob(qv.qv_hardlimit - qv.qv_usage);
        *btotal = dbtob(qv.qv_hardlimit);
    }

    return 1;
}

int uquota_getvolspace(const AFPObj *obj, struct vol *vol, VolSpace *bfree,
                       VolSpace *btotal, const uint32_t bsize)
{
    int uret, gret;
    VolSpace ubfree, ubtotal;
    VolSpace gbfree, gbtotal;
    uret = getfreespace(obj, vol, &ubfree, &ubtotal,
                        obj->uid, QUOTA_IDTYPE_USER);

    if (uret == 1) {
        LOG(log_info, logtype_afpd, "quota_get(%s, user): %d %d",
            vol->v_path, (int)ubfree, (int)ubtotal);
    }

    if (obj->ngroups >= 1) {
        gret = getfreespace(obj, vol, &gbfree, &gbtotal,
                            obj->groups[0], QUOTA_IDTYPE_GROUP);

        if (gret == 1) {
            LOG(log_info, logtype_afpd, "quota_get(%s, group): %d %d",
                vol->v_path, (int)gbfree, (int)gbtotal);
        }
    } else {
        gret = 0;
    }

    if (uret < 1 && gret < 1) { /* no quota for this fs */
        return AFPERR_PARAM;
    }

    if (uret < 1) {
        /* no user quotas, but group quotas; use them */
        *bfree = gbfree;
        *btotal = gbtotal;
    } else if (gret < 1) {
        /* no group quotas, but user quotas; use them */
        *bfree = ubfree;
        *btotal = ubtotal;
    } else {
        /* both; return smallest remaining space of user and group */
        if (ubfree < gbfree) {
            *bfree = ubfree;
            *btotal = ubtotal;
        } else {
            *bfree = gbfree;
            *btotal = gbtotal;
        }
    }

    return AFP_OK;
}

#else /* HAVE_LIBQUOTA */

#define WANT_USER_QUOTA 0
#define WANT_GROUP_QUOTA 1

#ifdef NEED_QUOTACTL_WRAPPER
int quotactl(int cmd, const char *special, int id, caddr_t addr)
{
    return syscall(__NR_quotactl, cmd, special, id, addr);
}
#endif /* NEED_QUOTACTL_WRAPPER */

static int overquota(struct dqblk *);

#ifdef __linux__

#ifdef HAVE_LINUX_XQM_H
#include <linux/xqm.h>
#else
#ifdef HAVE_XFS_XQM_H
#include <xfs/xqm.h>
#define HAVE_LINUX_XQM_H
#else
#ifdef  HAVE_LINUX_DQBLK_XFS_H
#include <linux/dqblk_xfs.h>
#define HAVE_LINUX_XQM_H
#endif /* HAVE_LINUX_DQBLK_XFS_H */
#endif /* HAVE_XFS_XQM_H */
#endif /* HAVE_LINUX_XQM_H */

#include <linux/unistd.h>

static int is_xfs = 0;

static int get_linux_xfs_quota(int, char *, uid_t, struct dqblk *);
static int get_linux_fs_quota(int, char *, uid_t, struct dqblk *);

/* format supported by current kernel */
static int kernel_iface = IFACE_UNSET;

/*
**  Check kernel quota version
**  Taken from quota-tools 3.08 by Jan Kara <jack@suse.cz>
*/
static void linuxquota_get_api(void)
{
#ifndef LINUX_API_VERSION
    struct stat st;

    if (stat("/proc/sys/fs/quota", &st) == 0) {
        kernel_iface = IFACE_GENERIC;
    } else {
        struct dqstats_v2 v2_stats;
        struct sigaction  sig;
        struct sigaction  oldsig;
        /* This signal handling is needed because old kernels send us SIGSEGV as they try to resolve the device */
        sig.sa_handler   = SIG_IGN;
        sig.sa_sigaction = NULL;
        sig.sa_flags     = 0;
        sigemptyset(&sig.sa_mask);

        if (sigaction(SIGSEGV, &sig, &oldsig) < 0) {
            LOG(log_error, logtype_afpd, "cannot set SEGV signal handler: %s",
                strerror(errno));
            goto failure;
        }

        if (quotactl(QCMD(Q_V2_GETSTATS, 0), NULL, 0, (void *)&v2_stats) >= 0) {
            kernel_iface = IFACE_VFSV0;
        } else if (errno != ENOSYS && errno != ENOTSUP) {
            /* RedHat 7.1 (2.4.2-2) newquota check
             * Q_V2_GETSTATS in it's old place, Q_GETQUOTA in the new place
             * (they haven't moved Q_GETSTATS to its new value) */
            int err_stat = 0;
            int err_quota = 0;
            char tmp[1024];         /* Just temporary buffer */

            if (quotactl(QCMD(Q_V1_GETSTATS, 0), NULL, 0, tmp)) {
                err_stat = errno;
            }

            if (quotactl(QCMD(Q_V1_GETQUOTA, 0), "/dev/null", 0, tmp)) {
                err_quota = errno;
            }

            /* On a RedHat 2.4.2-2 	we expect 0, EINVAL
             * On a 2.4.x 		we expect 0, ENOENT
             * On a 2.4.x-ac	we wont get here */
            if (err_stat == 0 && err_quota == EINVAL) {
                kernel_iface = IFACE_VFSV0;
            } else {
                kernel_iface = IFACE_VFSOLD;
            }
        } else {
            /* This branch is *not* in quota-tools 3.08
            ** but without it quota version is not correctly
            ** identified for the original SuSE 8.0 kernel */
            unsigned int vers_no;
            FILE *qf;

            if ((qf = fopen("/proc/fs/quota", "r"))) {
                if (fscanf(qf, "Version %u", &vers_no) == 1) {
                    if ((vers_no == (6 * 10000 + 5 * 100 + 0)) ||
                            (vers_no == (6 * 10000 + 5 * 100 + 1))) {
                        kernel_iface = IFACE_VFSV0;
                    }
                }

                fclose(qf);
            }
        }

        if (sigaction(SIGSEGV, &oldsig, NULL) < 0) {
            LOG(log_error, logtype_afpd, "cannot reset signal handler: %s",
                strerror(errno));
            goto failure;
        }
    }

failure:

    if (kernel_iface == IFACE_UNSET) {
        kernel_iface = IFACE_VFSOLD;
    }

#else /* defined LINUX_API_VERSION */
    kernel_iface = LINUX_API_VERSION;
#endif
}

/****************************************************************************/

static int get_linux_quota(int what, char *path, uid_t euser_id,
                           struct dqblk *dp)
{
    int r; /* result */

    if (is_xfs) {
        r = get_linux_xfs_quota(what, path, euser_id, dp);
    } else {
        r = get_linux_fs_quota(what, path, euser_id, dp);
    }

    return r;
}

/****************************************************************************
 Abstract out the XFS Quota Manager quota get call.
****************************************************************************/

static int get_linux_xfs_quota(int what, char *path, uid_t euser_id,
                               struct dqblk *dqb)
{
    int ret = -1;
#ifdef HAVE_LINUX_XQM_H
    struct fs_disk_quota D;
    memset(&D, 0, sizeof(D));

    if ((ret = quotactl(QCMD(Q_XGETQUOTA, (what ? GRPQUOTA : USRQUOTA)), path,
                        euser_id, (caddr_t)&D))) {
        return ret;
    }

    dqb->bsize = (uint64_t)512;
    dqb->dqb_bsoftlimit  = (uint64_t)D.d_blk_softlimit;
    dqb->dqb_bhardlimit  = (uint64_t)D.d_blk_hardlimit;
    dqb->dqb_ihardlimit  = (uint64_t)D.d_ino_hardlimit;
    dqb->dqb_isoftlimit  = (uint64_t)D.d_ino_softlimit;
    dqb->dqb_curinodes   = (uint64_t)D.d_icount;
    dqb->dqb_curblocks   = (uint64_t)D.d_bcount;
#endif
    return ret;
}

/*
** Wrapper for the quotactl(GETQUOTA) call.
** For API v2 the results are copied back into a v1 structure.
** Taken from quota-1.4.8 perl module
*/
static int get_linux_fs_quota(int what, char *path, uid_t euser_id,
                              struct dqblk *dqb)
{
    int ret;

    if (kernel_iface == IFACE_UNSET) {
        linuxquota_get_api();
    }

    if (kernel_iface == IFACE_GENERIC) {
        struct dqblk_v3 dqb3;
        ret = quotactl(QCMD(Q_V3_GETQUOTA, (what ? GRPQUOTA : USRQUOTA)), path,
                       euser_id, (caddr_t) &dqb3);

        if (ret == 0) {
            dqb->dqb_bhardlimit = dqb3.dqb_bhardlimit;
            dqb->dqb_bsoftlimit = dqb3.dqb_bsoftlimit;
            dqb->dqb_curblocks  = dqb3.dqb_curspace / DEV_QBSIZE;
            dqb->dqb_ihardlimit = dqb3.dqb_ihardlimit;
            dqb->dqb_isoftlimit = dqb3.dqb_isoftlimit;
            dqb->dqb_curinodes  = dqb3.dqb_curinodes;
            dqb->dqb_btime      = dqb3.dqb_btime;
            dqb->dqb_itime      = dqb3.dqb_itime;
            dqb->bsize	        = DEV_QBSIZE;
        }
    } else if (kernel_iface == IFACE_VFSV0) {
        struct dqblk_v2 dqb2;
        ret = quotactl(QCMD(Q_V2_GETQUOTA, (what ? GRPQUOTA : USRQUOTA)), path,
                       euser_id, (caddr_t) &dqb2);

        if (ret == 0) {
            dqb->dqb_bhardlimit = dqb2.dqb_bhardlimit;
            dqb->dqb_bsoftlimit = dqb2.dqb_bsoftlimit;
            dqb->dqb_curblocks  = dqb2.dqb_curspace / DEV_QBSIZE;
            dqb->dqb_ihardlimit = dqb2.dqb_ihardlimit;
            dqb->dqb_isoftlimit = dqb2.dqb_isoftlimit;
            dqb->dqb_curinodes  = dqb2.dqb_curinodes;
            dqb->dqb_btime      = dqb2.dqb_btime;
            dqb->dqb_itime      = dqb2.dqb_itime;
            dqb->bsize	        = DEV_QBSIZE;
        }
    } else { /* if (kernel_iface == IFACE_VFSOLD) */
        struct dqblk_v1 dqb1;
        ret = quotactl(QCMD(Q_V1_GETQUOTA, (what ? GRPQUOTA : USRQUOTA)), path,
                       euser_id, (caddr_t) &dqb1);

        if (ret == 0) {
            dqb->dqb_bhardlimit = dqb1.dqb_bhardlimit;
            dqb->dqb_bsoftlimit = dqb1.dqb_bsoftlimit;
            dqb->dqb_curblocks  = dqb1.dqb_curblocks;
            dqb->dqb_ihardlimit = dqb1.dqb_ihardlimit;
            dqb->dqb_isoftlimit = dqb1.dqb_isoftlimit;
            dqb->dqb_curinodes  = dqb1.dqb_curinodes;
            dqb->dqb_btime      = dqb1.dqb_btime;
            dqb->dqb_itime      = dqb1.dqb_itime;
            dqb->bsize	        = DEV_QBSIZE;
        }
    }

    return ret;
}

#endif /* linux */

#if defined(HAVE_SYS_MNTTAB_H) || defined(__svr4__)
/*
 * Return the mount point associated with the filesystem
 * on which "file" resides.  Returns NULL on failure.
 */
static char *
mountp(char *file, int *nfs)
{
    struct stat			sb;
    FILE 			*mtab;
    dev_t			devno;
    static struct mnttab	mnt;

    if (stat(file, &sb) < 0) {
        return NULL;
    }

    devno = sb.st_dev;

    if ((mtab = fopen("/etc/mnttab", "r")) == NULL) {
        return NULL;
    }

    while (getmntent(mtab, &mnt) == 0) {
        /* local fs */
        if ((stat(mnt.mnt_special, &sb) == 0) && (devno == sb.st_rdev)) {
            fclose(mtab);
            return mnt.mnt_mountp;
        }

        /* check for nfs. */
        if ((stat(mnt.mnt_mountp, &sb) == 0) && (devno == sb.st_dev)) {
            *nfs = strcmp(mnt.mnt_fstype, MNTTYPE_NFS) == 0 ? 1 : 0;
            fclose(mtab);
            return mnt.mnt_special;
        }
    }

    fclose(mtab);
    return NULL;
}

#else /* __svr4__ */
#if (defined(HAVE_SYS_MOUNT_H) && !defined(__linux__)) || defined(BSD4_4)

static char *
special(char *file, int *nfs)
{
    static struct statfs	sfs;

    if (statfs(file, &sfs) < 0) {
        return NULL;
    }

    /* XXX: make sure this really detects an nfs mounted fs */
    if (strchr(sfs.f_mntfromname, ':')) {
        *nfs = 1;
    }

    return sfs.f_mntfromname;
}

#else /* BSD4_4 */

static char *
special(char *file, int *nfs)
{
    struct stat		sb;
    FILE 		*mtab;
    dev_t		devno;
    struct mntent	*mnt;
    int 		found = 0;

    if (stat(file, &sb) < 0) {
        return NULL;
    }

    devno = sb.st_dev;

    if ((mtab = setmntent("/etc/mtab", "r")) == NULL) {
        return NULL;
    }

    while ((mnt = getmntent(mtab)) != NULL) {
        /* check for local fs */
        if ((stat(mnt->mnt_fsname, &sb) == 0) && devno == sb.st_rdev) {
            found = 1;
            break;
        }

        /* check for an nfs mount entry. the alternative is to use
        * strcmp(mnt->mnt_type, MNTTYPE_NFS) instead of the strchr. */
        if ((stat(mnt->mnt_dir, &sb) == 0) && (devno == sb.st_dev) &&
                strchr(mnt->mnt_fsname, ':')) {
            *nfs = 1;
            found = 1;
            break;
        }
    }

    endmntent(mtab);

    if (!found) {
        return NULL;
    }

#ifdef __linux__

    if (strcmp(mnt->mnt_type, "xfs") == 0) {
        is_xfs = 1;
    }

#endif
    return mnt->mnt_fsname;
}

#endif /* BSD4_4 */
#endif /* __svr4__ */


static int getfsquota(const AFPObj *obj, struct vol *vol, const int uid,
                      struct dqblk *dq)

{
    struct dqblk dqg;
#ifdef __svr4__
    struct quotctl      qc;
#endif
#ifdef __linux__
    memset(dq, 0, sizeof(struct dqblk));
#endif
    memset(&dqg, 0, sizeof(dqg));
#ifdef __svr4__
    qc.op = Q_GETQUOTA;
    qc.uid = uid;
    qc.addr = (caddr_t)dq;

    if (ioctl(vol->v_qfd, Q_QUOTACTL, &qc) < 0) {
        return AFPERR_PARAM;
    }

#else /* __svr4__ */
#ifndef USRQUOTA
#define USRQUOTA   0
#endif
#ifndef QCMD
#define QCMD(a,b)  (a)
#endif
    /* for group quotas. we only use these if the user belongs
    * to one group. */
#ifdef BSD4_4
    become_root();

    if (quotactl(vol->v_path, QCMD(Q_GETQUOTA, USRQUOTA),
                 uid, (char *)dq) != 0) {
        /* try group quotas */
        if (obj->ngroups >= 1) {
            if (quotactl(vol->v_path, QCMD(Q_GETQUOTA, GRPQUOTA),
                         obj->groups[0], (char *) &dqg) != 0) {
                unbecome_root();
                return AFPERR_PARAM;
            }
        }
    }

    unbecome_root();
#else /* BSD4_4 */

    if (get_linux_quota(WANT_USER_QUOTA, vol->v_gvs, uid, dq) != 0) {
        LOG(log_debug, logtype_afpd, "user quota did not work!");
    }

    if (get_linux_quota(WANT_GROUP_QUOTA, vol->v_gvs, getegid(),  &dqg) != 0) {
        LOG(log_debug, logtype_afpd, "group quota did not work!");
        return AFP_OK; /* no need to check user vs group quota */
    }

#endif  /* BSD4_4 */

    /* return either the group quota entry or user quota entry,
       whichever has the least amount of space remaining
    */

    /* if user space remaining > group space remaining */
    if (
        /* if overquota, free space is 0 otherwise hard-current */
        (overquota(dq) ? 0 : (dq->dqb_bhardlimit ? dq->dqb_bhardlimit -
                              dq->dqb_curblocks : ~((uint64_t) 0)))
        >
        (overquota(&dqg) ? 0 : (dqg.dqb_bhardlimit ? dqg.dqb_bhardlimit -
                                dqg.dqb_curblocks : ~((uint64_t) 0)))
    ) { /* if */
        /* use group quota limits rather than user limits */
        dq->dqb_bhardlimit = dqg.dqb_bhardlimit;
        dq->dqb_bsoftlimit = dqg.dqb_bsoftlimit;
        dq->dqb_curblocks = dqg.dqb_curblocks;
#ifdef __linux__
        dq->dqb_ihardlimit = dqg.dqb_ihardlimit;
        dq->dqb_isoftlimit = dqg.dqb_isoftlimit;
        dq->dqb_curinodes = dqg.dqb_curinodes;
        dq->dqb_btime = dqg.dqb_btime;
        dq->dqb_itime = dqg.dqb_itime;
        dq->bsize = dqg.bsize;
#else
        dq->dqb_btimelimit = dqg.dqb_btimelimit;
#endif
    } /* if */

#endif /* __svr4__ */
    return AFP_OK;
}


static int getquota(const AFPObj *obj, struct vol *vol, struct dqblk *dq,
                    const uint32_t bsize)
{
    char *p;
#ifdef __svr4__
    char		buf[MAXPATHLEN + 1];

    if (vol->v_qfd == -1 && vol->v_gvs == NULL) {
        if ((p = mountp(vol->v_path, &vol->v_nfs)) == NULL) {
            LOG(log_info, logtype_afpd, "getquota: mountp %s fails", vol->v_path);
            return AFPERR_PARAM;
        }

        if (vol->v_nfs) {
            if ((vol->v_gvs = (char *)malloc(strlen(p) + 1)) == NULL) {
                LOG(log_error, logtype_afpd, "getquota: malloc: %s", strerror(errno));
                return AFPERR_MISC;
            }

            strcpy(vol->v_gvs, p);
        } else {
            sprintf(buf, "%s/quotas", p);

            if ((vol->v_qfd = open(buf, O_RDONLY, 0)) < 0) {
                LOG(log_info, logtype_afpd, "open %s: %s", buf, strerror(errno));
                return AFPERR_PARAM;
            }
        }
    }

#else

    if (vol->v_gvs == NULL) {
        if ((p = special(vol->v_path, &vol->v_nfs)) == NULL) {
            LOG(log_info, logtype_afpd, "getquota: special %s fails", vol->v_path);
            return AFPERR_PARAM;
        }

        if ((vol->v_gvs = (char *)malloc(strlen(p) + 1)) == NULL) {
            LOG(log_error, logtype_afpd, "getquota: malloc: %s", strerror(errno));
            return AFPERR_MISC;
        }

        strcpy(vol->v_gvs, p);
    }

#endif
    return vol->v_nfs ? getnfsquota(vol, obj->uid, bsize, dq) :
           getfsquota(obj, vol, obj->uid, dq);
}

static int overquota(struct dqblk *dqblk)
{
    struct timeval	tv;

    if (dqblk->dqb_curblocks > dqblk->dqb_bhardlimit &&
            dqblk->dqb_bhardlimit != 0) {
        return 1;
    }

    if (dqblk->dqb_curblocks < dqblk->dqb_bsoftlimit ||
            dqblk->dqb_bsoftlimit == 0) {
        return 0;
    }

    if (gettimeofday(&tv, NULL) < 0) {
        LOG(log_error, logtype_afpd, "overquota: gettimeofday: %s", strerror(errno));
        return AFPERR_PARAM;
    }

    if (dqblk->dqb_btimelimit && dqblk->dqb_btimelimit > tv.tv_sec) {
        return 0;
    }

    return 1;
}

/*
 * This next bit is basically for linux -- everything is fine
 * if you use 1k blocks... but if you try (for example) to mount
 * a volume via nfs from a netapp (which might use 4k blocks) everything
 * gets reported improperly.  I have no idea about dbtob on other
 * platforms.
 */

#ifdef HAVE_BROKEN_DBTOB
#undef dbtob
#define dbtob(a, b)	((VolSpace)((VolSpace)(a) * (VolSpace)(b)))
#define HAVE_2ARG_DBTOB
#endif

#ifndef dbtob
#define dbtob(a)       ((a) << 10)
#endif

/* i do the cast to VolSpace here to make sure that 64-bit shifts
   work */
#ifdef HAVE_2ARG_DBTOB
#define tobytes(a, b)  dbtob((VolSpace) (a), (VolSpace) (b))
#else
#define tobytes(a, b)  dbtob((VolSpace) (a))
#endif

int uquota_getvolspace(const AFPObj *obj, struct vol *vol, VolSpace *bfree,
                       VolSpace *btotal, const uint32_t bsize)
{
    uint64_t this_bsize;
    struct dqblk dqblk;
    this_bsize = bsize;

    if (getquota(obj, vol, &dqblk, bsize) != 0) {
        return AFPERR_PARAM;
    }

#ifdef __linux__
    this_bsize = dqblk.bsize;
#endif

    /* no limit set for this user. it might be set in the future. */
    if (dqblk.dqb_bsoftlimit == 0 && dqblk.dqb_bhardlimit == 0) {
        *btotal = *bfree = ~((VolSpace) 0);
    } else if (overquota(&dqblk)) {
        if (tobytes(dqblk.dqb_curblocks, this_bsize) > tobytes(dqblk.dqb_bsoftlimit,
                this_bsize)) {
            *btotal = tobytes(dqblk.dqb_curblocks, this_bsize);
            *bfree = 0;
        } else {
            *btotal = tobytes(dqblk.dqb_bsoftlimit, this_bsize);
            *bfree  = tobytes(dqblk.dqb_bsoftlimit, this_bsize) -
                      tobytes(dqblk.dqb_curblocks, this_bsize);
        }
    } else {
        *btotal = tobytes(dqblk.dqb_bhardlimit, this_bsize);
        *bfree  = tobytes(dqblk.dqb_bhardlimit, this_bsize) -
                  tobytes(dqblk.dqb_curblocks, this_bsize);
    }

    return AFP_OK;
}
#endif /* HAVE_LIBQUOTA */
#endif
