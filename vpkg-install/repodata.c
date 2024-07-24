#include "repodata.h"

#include <assert.h>
#include <archive.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

#include "vpkg/defs.h"

bool repodata_flush(struct xbps_handle *xhp, const char *repodir,
                    const char *reponame, xbps_dictionary_t idx,
                    xbps_dictionary_t meta, const char *compression)
{
    struct archive *ar;
    char *repofile, *tname, *buf;
    int rv, repofd = -1;
    mode_t mask;
    bool result;

    /* Create a tempfile for our repository archive */
    repofile = xbps_repo_path_with_name(xhp, repodir, reponame);
    assert(repofile);
    tname = xbps_xasprintf("%s.XXXXXXXXXX", repofile);
    assert(tname);
    mask = umask(S_IXUSR|S_IRWXG|S_IRWXO);
    if ((repofd = mkstemp(tname)) == -1)
        return false;

    umask(mask);
    /* Create and write our repository archive */
    ar = archive_write_new();
    if (ar == NULL)
        return false;

    /*
     * Set compression format, zstd by default.
     */
    if (compression == NULL || strcmp(compression, "zstd") == 0) {
        archive_write_add_filter_zstd(ar);
        archive_write_set_options(ar, "compression-level=9");
    } else if (strcmp(compression, "gzip") == 0) {
        archive_write_add_filter_gzip(ar);
        archive_write_set_options(ar, "compression-level=9");
    } else if (strcmp(compression, "bzip2") == 0) {
        archive_write_add_filter_bzip2(ar);
        archive_write_set_options(ar, "compression-level=9");
    } else if (strcmp(compression, "lz4") == 0) {
        archive_write_add_filter_lz4(ar);
        archive_write_set_options(ar, "compression-level=9");
    } else if (strcmp(compression, "xz") == 0) {
        archive_write_add_filter_xz(ar);
        archive_write_set_options(ar, "compression-level=9");
    } else if (strcmp(compression, "none") == 0) {
        /* empty */
    } else {
        return false;
    }

    archive_write_set_format_pax_restricted(ar);
    if (archive_write_open_fd(ar, repofd) != ARCHIVE_OK)
        return false;

    /* XBPS_REPOIDX */
    buf = xbps_dictionary_externalize(idx);
    if (buf == NULL)
        return false;
    rv = xbps_archive_append_buf(ar, buf, strlen(buf),
        XBPS_REPOIDX, 0644, "root", "root");
    free(buf);
    if (rv != 0)
        return false;

    /* XBPS_REPOIDX_META */
    if (meta == NULL) {
        /* fake entry */
        buf = strdup("DEADBEEF");
        if (buf == NULL)
            return false;
    } else {
        buf = xbps_dictionary_externalize(meta);
    }
    rv = xbps_archive_append_buf(ar, buf, strlen(buf),
        XBPS_REPOIDX_META, 0644, "root", "root");
    free(buf);
    if (rv != 0)
        return false;

    /* Write data to tempfile and rename */
    if (archive_write_close(ar) != ARCHIVE_OK)
        return false;
    if (archive_write_free(ar) != ARCHIVE_OK)
        return false;
#ifdef HAVE_FDATASYNC
    fdatasync(repofd);
#else
    fsync(repofd);
#endif
    if (fchmod(repofd, 0664) == -1) {
        close(repofd);
        unlink(tname);
        result = false;
        goto out;
    }
    close(repofd);
    if (rename(tname, repofile) == -1) {
        unlink(tname);
        result = false;
        goto out;
    }
    result = true;
out:
    free(repofile);
    free(tname);

    return result;
}

bool repodata_commit(struct xbps_handle *xhp, const char *repodir,
                     xbps_dictionary_t idx, xbps_dictionary_t meta,
                     xbps_dictionary_t stage, const char *compression)
{
    xbps_object_iterator_t iter;
    xbps_dictionary_keysym_t keysym;
    int rv;
    xbps_dictionary_t oldshlibs, usedshlibs;

    if (xbps_dictionary_count(stage) == 0) {
        return true;
    }

    /*
     * Find old shlibs-provides
     */
    oldshlibs = xbps_dictionary_create();
    usedshlibs = xbps_dictionary_create();

    iter = xbps_dictionary_iterator(stage);
    while ((keysym = xbps_object_iterator_next(iter))) {
        const char *pkgname = xbps_dictionary_keysym_cstring_nocopy(keysym);
        xbps_dictionary_t pkg = xbps_dictionary_get(idx, pkgname);
        xbps_array_t pkgshlibs;

        pkgshlibs = xbps_dictionary_get(pkg, "shlib-provides");
        for (unsigned int i = 0; i < xbps_array_count(pkgshlibs); i++) {
            const char *shlib = NULL;
            xbps_array_get_cstring_nocopy(pkgshlibs, i, &shlib);
            xbps_dictionary_set_cstring(oldshlibs, shlib, pkgname);
        }
    }
    xbps_object_iterator_release(iter);

    /*
     * throw away all unused shlibs
     */
    iter = xbps_dictionary_iterator(idx);
    while ((keysym = xbps_object_iterator_next(iter))) {
        const char *pkgname = xbps_dictionary_keysym_cstring_nocopy(keysym);
        xbps_dictionary_t pkg = xbps_dictionary_get(stage, pkgname);
        xbps_array_t pkgshlibs;
        if (!pkg)
            pkg = xbps_dictionary_get_keysym(idx, keysym);
        pkgshlibs = xbps_dictionary_get(pkg, "shlib-requires");

        for (unsigned int i = 0; i < xbps_array_count(pkgshlibs); i++) {
            const char *shlib = NULL;
            xbps_array_t users;
            xbps_array_get_cstring_nocopy(pkgshlibs, i, &shlib);
            if (!xbps_dictionary_get(oldshlibs, shlib))
                continue;
            users = xbps_dictionary_get(usedshlibs, shlib);
            if (!users) {
                users = xbps_array_create();
                xbps_dictionary_set(usedshlibs, shlib, users);
            }
            xbps_array_add_cstring(users, pkgname);
        }
    }
    xbps_object_iterator_release(iter);

    /*
     * purge all packages that are fullfilled by the index and
     * not in the stage.
     */
    iter = xbps_dictionary_iterator(idx);
    while ((keysym = xbps_object_iterator_next(iter))) {
        xbps_dictionary_t pkg = xbps_dictionary_get_keysym(idx, keysym);
        xbps_array_t pkgshlibs;


        if (xbps_dictionary_get(stage,
                                xbps_dictionary_keysym_cstring_nocopy(keysym))) {
            continue;
        }

        pkgshlibs = xbps_dictionary_get(pkg, "shlib-provides");
        for (unsigned int i = 0; i < xbps_array_count(pkgshlibs); i++) {
            const char *shlib = NULL;
            xbps_array_get_cstring_nocopy(pkgshlibs, i, &shlib);
            xbps_dictionary_remove(usedshlibs, shlib);
        }
    }
    xbps_object_iterator_release(iter);

    /*
     * purge all packages that are fullfilled by the stage
     */
    iter = xbps_dictionary_iterator(stage);
    while ((keysym = xbps_object_iterator_next(iter))) {
        xbps_dictionary_t pkg = xbps_dictionary_get_keysym(stage, keysym);
        xbps_array_t pkgshlibs;

        pkgshlibs = xbps_dictionary_get(pkg, "shlib-provides");
        for (unsigned int i = 0; i < xbps_array_count(pkgshlibs); i++) {
            const char *shlib = NULL;
            xbps_array_get_cstring_nocopy(pkgshlibs, i, &shlib);
            xbps_dictionary_remove(usedshlibs, shlib);
        }
    }
    xbps_object_iterator_release(iter);

    if (xbps_dictionary_count(usedshlibs) != 0) {
        printf("Inconsistent shlibs:\n");
        iter = xbps_dictionary_iterator(usedshlibs);
        while ((keysym = xbps_object_iterator_next(iter))) {
            const char *shlib = xbps_dictionary_keysym_cstring_nocopy(keysym),
                  *provider = NULL, *pre;
            xbps_array_t users = xbps_dictionary_get(usedshlibs, shlib);
            xbps_dictionary_get_cstring_nocopy(oldshlibs, shlib, &provider);

            printf("  %s (provided by: %s; used by: ", shlib, provider);
            pre = "";
            for (unsigned int i = 0; i < xbps_array_count(users); i++) {
                const char *user = NULL;
                xbps_array_get_cstring_nocopy(users, i, &user);
                xbps_dictionary_remove(usedshlibs, shlib);
                printf("%s%s",pre, user);
                pre = ", ";
            }
            printf(")\n");
        }
        xbps_object_iterator_release(iter);
        iter = xbps_dictionary_iterator(stage);
        while ((keysym = xbps_object_iterator_next(iter))) {
            xbps_dictionary_t pkg = xbps_dictionary_get_keysym(stage, keysym);
            const char *pkgver = NULL, *arch = NULL;
            xbps_dictionary_get_cstring_nocopy(pkg, "pkgver", &pkgver);
            xbps_dictionary_get_cstring_nocopy(pkg, "architecture", &arch);
            printf("stage: added `%s' (%s)\n", pkgver, arch);
        }
        xbps_object_iterator_release(iter);
        rv = repodata_flush(xhp, repodir, "stagedata", stage, NULL, compression);
    }
    else {
        char *stagefile;
        iter = xbps_dictionary_iterator(stage);
        while ((keysym = xbps_object_iterator_next(iter))) {
            const char *pkgname = xbps_dictionary_keysym_cstring_nocopy(keysym);
            xbps_dictionary_t pkg = xbps_dictionary_get_keysym(stage, keysym);
            const char *pkgver = NULL, *arch = NULL;
            xbps_dictionary_get_cstring_nocopy(pkg, "pkgver", &pkgver);
            xbps_dictionary_get_cstring_nocopy(pkg, "architecture", &arch);
            printf("index: added `%s' (%s).\n", pkgver, arch);
            xbps_dictionary_set(idx, pkgname, pkg);
        }
        xbps_object_iterator_release(iter);
        stagefile = xbps_repo_path_with_name(xhp, repodir, "stagedata");
        unlink(stagefile);
        free(stagefile);
        rv = repodata_flush(xhp, repodir, "repodata", idx, meta, compression);
    }
    xbps_object_release(usedshlibs);
    xbps_object_release(oldshlibs);
    return rv;
}

xbps_dictionary_t repodata_add(struct xbps_handle *xhp, const char *pathname,
                                      xbps_dictionary_t idx, xbps_dictionary_t meta,
                                      xbps_dictionary_t stage)
{
    // const char *filename;
    const char *architecture;
    const char *pkgver;
    char sha256[XBPS_SHA256_SIZE];
    char pkgname[XBPS_NAME_SIZE];
    xbps_dictionary_t binpkgd = xbps_archive_fetch_plist(pathname, "/props.plist");

    if (!xbps_dictionary_get_cstring_nocopy(binpkgd, "architecture", &architecture)) {
        fprintf(stderr, "architecture unset\n");
        xbps_object_release(binpkgd);
        return NULL;
    }

    if (!xbps_dictionary_get_cstring_nocopy(binpkgd, "pkgver", &pkgver)) {
        fprintf(stderr, "pkgver unset\n");
        xbps_object_release(binpkgd);
        return NULL;
    }

    if (!xbps_pkg_arch_match(xhp, architecture, NULL)) {
        fprintf(stderr, "arch invalid\n");
        xbps_object_release(binpkgd);
        return NULL;
    }

    if (!xbps_pkg_name(pkgname, sizeof(pkgname), pkgver)) {
        fprintf(stderr, "pkgver invalid\n");
        xbps_object_release(binpkgd);
        return NULL;
    }

    if (!xbps_file_sha256(sha256, sizeof(sha256), pathname)) {
        fprintf(stderr, "sha256 invalid\n");
        xbps_object_release(binpkgd);
        return NULL;
    }

    if (!xbps_dictionary_set_cstring(binpkgd, "filename-sha256", sha256)) {
        fprintf(stderr, "filename-sha256 invalid\n");
        xbps_object_release(binpkgd);
        return NULL;
    }

    xbps_dictionary_remove(binpkgd, "pkgname");
    xbps_dictionary_remove(binpkgd, "version");
    xbps_dictionary_remove(binpkgd, "packaged-with");

    if (!xbps_dictionary_set(stage, pkgname, binpkgd)) {
        xbps_object_release(binpkgd);
        return NULL;
    }

    return binpkgd;
}
