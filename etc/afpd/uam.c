/*
 *
 * Copyright (c) 1999 Adrian Sun (asun@zoology.washington.edu)
 * All Rights Reserved.  See COPYRIGHT.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif /* HAVE_CONFIG_H */

#include <arpa/inet.h>
#include <ctype.h>

#ifdef HAVE_DLFCN_H
#include <dlfcn.h>
#endif /* HAVE_DLFCN_H */

#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <bstrlib.h>

#include <atalk/afp.h>
#include <atalk/dsi.h>
#include <atalk/globals.h>
#include <atalk/logger.h>
#include <atalk/util.h>
#include <atalk/volume.h>

#include "afp_config.h"
#include "auth.h"
#include "uam_auth.h"

/* --- server uam functions -- */

/* uam_load. uams must have a uam_setup function. */
struct uam_mod *uam_load(AFPObj *obj, const char *path, const char *name)
{
    char buf[MAXPATHLEN + 1], *p;
    struct uam_mod *mod;
    void *module;

    if ((module = mod_open(path)) == NULL) {
        LOG(log_error, logtype_afpd, "uam_load(%s): failed to load: %s", name,
            mod_error());
        return NULL;
    }

    if ((mod = (struct uam_mod *) malloc(sizeof(struct uam_mod))) == NULL) {
        LOG(log_error, logtype_afpd, "uam_load(%s): malloc failed", name);
        goto uam_load_fail;
    }

    strlcpy(buf, name, sizeof(buf));

    if ((p = strchr(buf, '.'))) {
        *p = '\0';
    }

    if ((mod->uam_fcn = mod_symbol(module, buf)) == NULL) {
        LOG(log_error, logtype_afpd, "uam_load(%s): mod_symbol error for symbol %s",
            name,
            buf);
        goto uam_load_err;
    }

    if (mod->uam_fcn->uam_type != UAM_MODULE_SERVER) {
        LOG(log_error, logtype_afpd,
            "uam_load(%s): attempted to load a non-server module",
            name);
        goto uam_load_err;
    }

    /* version check would go here */

    if (!mod->uam_fcn->uam_setup ||
            ((*mod->uam_fcn->uam_setup)(obj, name) < 0)) {
        LOG(log_error, logtype_afpd, "uam_load(%s): uam_setup failed", name);
        goto uam_load_err;
    }

    mod->uam_module = module;
    return mod;
uam_load_err:
    free(mod);
uam_load_fail:
    mod_close(module);
    return NULL;
}

/* unload the module. we check for a cleanup function, but we don't
 * die if one doesn't exist. however, things are likely to leak without one.
 */
void uam_unload(struct uam_mod *mod)
{
    if (mod->uam_fcn->uam_cleanup) {
        (*mod->uam_fcn->uam_cleanup)();
    }

    mod_close(mod->uam_module);
    free(mod);
}

/* -- client-side uam functions -- */
/* set up stuff for this uam. */
int uam_register(const int type, const char *path, const char *name, ...)
{
    va_list ap;
    struct uam_obj *uam;
    int ret;

    if (!name) {
        return -1;
    }

    /* see if it already exists. */
    if ((uam = auth_uamfind(type, name, strlen(name)))) {
        if (strcmp(uam->uam_path, path)) {
            /* it exists, but it's not the same module. */
            LOG(log_error, logtype_afpd, "uam_register: \"%s\" already loaded by %s",
                name, path);
            return -1;
        }

        uam->uam_count++;
        return 0;
    }

    /* allocate space for uam */
    if ((uam = calloc(1, sizeof(struct uam_obj))) == NULL) {
        return -1;
    }

    uam->uam_name = name;
    uam->uam_path = strdup(path);
    uam->uam_count++;
    va_start(ap, name);

    switch (type) {
    case UAM_SERVER_LOGIN_EXT:
        /* expect four arguments */
        uam->u.uam_login.login = va_arg(ap, int (*)(void *, struct passwd **, char *,
                                        int, char *, size_t *));
        uam->u.uam_login.logincont = va_arg(ap, int (*)(void *, struct passwd **,
                                            char *, int, char *, size_t *));
        uam->u.uam_login.logout = va_arg(ap, void (*)(void));
        uam->u.uam_login.login_ext = va_arg(ap, int (*)(void *, char *,
                                            struct passwd **, char *, int, char *, size_t *));
        break;

    case UAM_SERVER_LOGIN:
        /* expect three arguments */
        uam->u.uam_login.login_ext = NULL;
        uam->u.uam_login.login = va_arg(ap, int (*)(void *, struct passwd **, char *,
                                        int, char *, size_t *));
        uam->u.uam_login.logincont = va_arg(ap, int (*)(void *, struct passwd **,
                                            char *, int, char *, size_t *));
        uam->u.uam_login.logout = va_arg(ap, void (*)(void));
        break;

    case UAM_SERVER_CHANGEPW:
        /* one argument */
        uam->u.uam_changepw = va_arg(ap, int (*)(void *, char *, struct passwd *,
                                     char *, int, char *, size_t *));
        break;

    case UAM_SERVER_PRINTAUTH:

    /* x arguments */
    default:
        break;
    }

    va_end(ap);
    /* attach to other uams */
    ret = auth_register(type, uam);

    if (ret) {
        free(uam->uam_path);
        free(uam);
    }

    return ret;
}

void uam_unregister(const int type, const char *name)
{
    struct uam_obj *uam;

    if (!name) {
        return;
    }

    uam = auth_uamfind(type, name, strlen(name));

    if (!uam || --uam->uam_count > 0) {
        return;
    }

    auth_unregister(uam);
    free(uam->uam_path);
    free(uam);
}

/* --- helper functions for plugin uams ---
 * name: user name
 * len:  size of name buffer.
*/

struct passwd *uam_getname(void *private, char *name, const int len)
{
    AFPObj *obj = private;
    struct passwd *pwent = NULL;
    struct passwd pwent_buf;
    static char username[256];
    static char user[256];
    static char pwname[256];
    char *p;
    size_t namelen;
    size_t gecoslen = 0;
    size_t pwnamelen = 0;
    long bufsize = sysconf(_SC_GETPW_R_SIZE_MAX);

    if (bufsize == -1) {
        bufsize = 16384;
    }

    char *buffer = malloc(bufsize);

    if (buffer == NULL) {
        free(buffer);
        return NULL;
    }

#ifdef HAVE_GETPWNAM_SHADOW

    if (pwent = getpwnam_shadow(name)) {
#else

    if (getpwnam_r(name, &pwent_buf, buffer, sizeof(buffer), &pwent) == 0
            && pwent != NULL) {
        free(buffer);
#endif
        return pwent;
    }

    /* if we have a NT domain name try with it */
    if (obj->options.addomain || (obj->options.ntdomain
                                  && obj->options.ntseparator)) {
        bstring princ;

        if (obj->options.addomain) {
            princ = bformat("%s@%s", name, obj->options.addomain);
        } else {
            princ = bformat("%s%s%s", obj->options.ntdomain, obj->options.ntseparator,
                            name);
        }

        if (bdata(princ) != NULL) {
            const char *bdatum = bdata(princ);
            getpwnam_r(bdatum, &pwent_buf, buffer, sizeof(buffer), &pwent);
        }

        bdestroy(princ);

        if (pwent) {
            int len = strlen(pwent->pw_name);

            if (len < MAXUSERLEN) {
                strncpy(name, pwent->pw_name, MAXUSERLEN);
            } else {
                LOG(log_error, logtype_uams, "The name '%s' is longer than %d", pwent->pw_name,
                    MAXUSERLEN);
            }

            free(buffer);
            return pwent;
        }
    }

    free(buffer);
    namelen = convert_string((utf8_encoding(obj)) ? CH_UTF8_MAC :
                             obj->options.maccharset,
                             CH_UCS2, name, -1, username, sizeof(username));

    if (namelen == -1) {
        return NULL;
    }

    setpwent();

    while ((pwent = getpwent())) {
        if ((p = strchr(pwent->pw_gecos, ','))) {
            *p = '\0';
        }

        gecoslen = convert_string(obj->options.unixcharset, CH_UCS2,
                                  pwent->pw_gecos, -1, user, sizeof(username));
        pwnamelen = convert_string(obj->options.unixcharset, CH_UCS2,
                                   pwent->pw_name, -1, pwname, sizeof(username));

        if ((size_t) -1 == gecoslen && (size_t) -1 == pwnamelen) {
            continue;
        }

        /* check against both the gecos and the name fields. the user
         * might have just used a different capitalization. */

        if ((namelen == gecoslen
                && strncasecmp_w((ucs2_t *)user, (ucs2_t *)username, len) == 0) ||
                (namelen == pwnamelen
                 && strncasecmp_w((ucs2_t *) pwname, (ucs2_t *) username, len) == 0)) {
            strlcpy(name, pwent->pw_name, len);
            break;
        }
    }

    endpwent();
    /* os x server doesn't keep anything useful if we do getpwent */
    return pwent ? getpwnam(name) : NULL;
}

int uam_checkuser(const struct passwd *pwd)
{
#if !defined(DISABLE_SHELLCHECK)
    const char *p;
#endif /* DISABLE_SHELLCHECK */

    if (!pwd) {
        return -1;
    }

#if !defined(DISABLE_SHELLCHECK)

    if (!pwd->pw_shell || (*pwd->pw_shell == '\0')) {
        LOG(log_info, logtype_afpd, "uam_checkuser: User %s does not have a shell",
            pwd->pw_name);
        return -1;
    }

    while ((p = getusershell())) {
        if (strcmp(p, pwd->pw_shell) == 0) {
            break;
        }
    }

    endusershell();

    if (!p) {
        LOG(log_info, logtype_afpd, "illegal shell %s for %s", pwd->pw_shell,
            pwd->pw_name);
        return -1;
    }

#endif /* DISABLE_SHELLCHECK */
    return 0;
}

int uam_random_string(AFPObj *obj, char *buf, int len)
{
    uint32_t result;
    int ret;
    int fd;

    if ((len <= 0) || (len % sizeof(result))) {
        return -1;
    }

    /* construct a random number */
    if ((fd = open("/dev/urandom", O_RDONLY)) < 0) {
        struct timeval tv;
        struct timezone tz;
        int i;

        if (gettimeofday(&tv, &tz) < 0) {
            return -1;
        }

        srandom(tv.tv_sec + (unsigned long) obj + (unsigned long) obj->dsi);

        for (i = 0; i < len; i += sizeof(result)) {
            result = random();
            memcpy(buf + i, &result, sizeof(result));
        }
    } else {
        ret = read(fd, buf, len);
        close(fd);

        if (ret <= 0) {
            return -1;
        }
    }

    return 0;
}

/* afp-specific functions */
int uam_afpserver_option(void *private, const int what, void *option,
                         size_t *len)
{
    AFPObj *obj = private;
    const char **buf = (const char **) option; /* most of the options are this */
    struct session_info **sinfo = (struct session_info **) option;

    if (!obj || !option) {
        return -1;
    }

    switch (what) {
    case UAM_OPTION_USERNAME:
        *buf = &(obj->username[0]);

        if (len) {
            *len = sizeof(obj->username) - 1;
        }

        break;

    case UAM_OPTION_GUEST:
        *buf = obj->options.guest;

        if (len) {
            *len = strlen(obj->options.guest);
        }

        break;

    case UAM_OPTION_PASSWDOPT:
        if (!len) {
            return -1;
        }

        switch (*len) {
        case UAM_PASSWD_FILENAME:
            *buf = obj->options.passwdfile;
            *len = strlen(obj->options.passwdfile);
            break;

        case UAM_PASSWD_MINLENGTH:
            *((int *) option) = obj->options.passwdminlen;
            *len = sizeof(obj->options.passwdminlen);
            break;

        case UAM_PASSWD_EXPIRETIME: /* not implemented */
        default:
            return -1;
            break;
        }

        break;

    case UAM_OPTION_SIGNATURE:
        if (obj->proto == AFPPROTO_DSI) {
            *buf = (void *)obj->dsi->signature;
        } else {
            *buf = (void *)obj->signature;
        }

        if (len) {
            *len = 16;
        }

        break;

    case UAM_OPTION_RANDNUM: /* returns a random number in 4-byte units. */
        if (!len) {
            return -1;
        }

        return uam_random_string(obj, option, *len);
        break;

    case UAM_OPTION_HOSTNAME:
        *buf = obj->options.hostname;

        if (len) {
            *len = strlen(obj->options.hostname);
        }

        break;

    case UAM_OPTION_PROTOCOL:
        *((int *)option) = obj->proto;
        break;

    case UAM_OPTION_CLIENTNAME: {
        struct DSI *dsi;

        if (obj->proto == AFPPROTO_DSI) {
            dsi = obj->dsi;
        } else {
            dsi = obj->handle;
        }

        const struct sockaddr *sa;
        static char hbuf[NI_MAXHOST];
        sa = (struct sockaddr *)&dsi->client;

        if (getnameinfo(sa, sizeof(dsi->client), hbuf, sizeof(hbuf), NULL, 0, 0) == 0) {
            *buf = hbuf;
        } else {
            *buf = getip_string((struct sockaddr *)&dsi->client);
        }

        break;
    }

    case UAM_OPTION_COOKIE:
        /* it's up to the uam to actually store something useful here.
         * this just passes back a handle to the cookie. the uam side
         * needs to do something like **buf = (void *) cookie to store
         * the cookie. */
        *buf = (void *) &obj->uam_cookie;
        break;

    case UAM_OPTION_KRB5SERVICE:
        *buf = obj->options.k5service;

        if (len) {
            *len = (*buf) ? strlen(*buf) : 0;
        }

        break;

    case UAM_OPTION_KRB5REALM:
        *buf = obj->options.k5realm;

        if (len) {
            *len = (*buf) ? strlen(*buf) : 0;
        }

        break;

    case UAM_OPTION_FQDN:
        *buf = obj->options.fqdn;

        if (len) {
            *len = (*buf) ? strlen(*buf) : 0;
        }

        break;

    case UAM_OPTION_MACCHARSET:
        *((int *) option) = obj->options.maccharset;
        *len = sizeof(obj->options.maccharset);
        break;

    case UAM_OPTION_UNIXCHARSET:
        *((int *) option) = obj->options.unixcharset;
        *len = sizeof(obj->options.unixcharset);
        break;

    case UAM_OPTION_SESSIONINFO:
        *sinfo = &(obj->sinfo);
        break;

    default:
        return -1;
        break;
    }

    return 0;
}

/* --- papd-specific functions (just placeholders) --- */
struct papfile;

UAM_MODULE_EXPORT void append(struct papfile *pf  _U_, const char *data _U_,
                              int len _U_)
{
    return;
}
