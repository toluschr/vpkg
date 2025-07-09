#include "repodata.h"

#include <assert.h>
#include <archive.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

#include <limits.h>

static struct archive *
open_archive(int fd, const char *compression)
{
    struct archive *ar;
    int r;

    ar = archive_write_new();
    if (!ar)
        return NULL;
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
        archive_write_free(ar);
        errno = EINVAL;
        return NULL;
    }

    archive_write_set_format_pax_restricted(ar);
    r = archive_write_open_fd(ar, fd);
    if (r != ARCHIVE_OK) {
        r = -archive_errno(ar);
        if (r == 1)
            r = -EINVAL;
        archive_write_free(ar);
        errno = -r;
        return NULL;
    }

    return ar;
}

static int
archive_dict(struct archive *ar, const char *filename, xbps_dictionary_t dict)
{
    char *buf;
    int r;

    if (xbps_dictionary_count(dict) == 0) {
        r = xbps_archive_append_buf(ar, "", 0, filename, 0644,
            "root", "root");
        if (r < 0)
            return r;
        return 0;
    }

    errno = 0;
    buf = xbps_dictionary_externalize(dict);
    if (!buf) {
        r = -errno;
        xbps_error_printf("failed to externalize dictionary for: %s\n",
            filename);
        if (r == 0)
            return -EINVAL;
        return 0;
    }

    r = xbps_archive_append_buf(ar, buf, strlen(buf), filename, 0644,
        "root", "root");

    free(buf);

    if (r < 0) {
        xbps_error_printf("failed to write archive entry: %s: %s\n",
            filename, strerror(-r));
    }
    return r;
}

int
repodata_flush(
    const char *repodir,
    const char *arch,
    xbps_dictionary_t index,
    xbps_dictionary_t stage,
    xbps_dictionary_t meta,
    const char *compression)
{
    char path[PATH_MAX];
    char tmp[PATH_MAX];
    struct archive *ar = NULL;
    mode_t prevumask;
    int r;
    int fd;

    r = snprintf(path, sizeof(path), "%s/%s-repodata", repodir, arch);
    if (r < 0 || (size_t)r >= sizeof(tmp)) {
        xbps_error_printf("repodata path too long: %s: %s\n", path,
            strerror(ENAMETOOLONG));
        return -ENAMETOOLONG;
    }

    r = snprintf(tmp, sizeof(tmp), "%s.XXXXXXX", path);
    if (r < 0 || (size_t)r >= sizeof(tmp)) {
        xbps_error_printf("repodata tmp path too long: %s: %s\n", path,
            strerror(ENAMETOOLONG));
        return -ENAMETOOLONG;
    }

    prevumask = umask(S_IXUSR|S_IRWXG|S_IRWXO);
    fd = mkstemp(tmp);
    if (fd == -1) {
        r = -errno;
        xbps_error_printf("failed to open temp file: %s: %s", tmp, strerror(-r));
        umask(prevumask);
        goto err;
    }
    umask(prevumask);

    ar = open_archive(fd, compression);
    if (!ar) {
        r = -errno;
        goto err;
    }

    r = archive_dict(ar, XBPS_REPODATA_INDEX, index);
    if (r < 0)
        goto err;
    r = archive_dict(ar, XBPS_REPODATA_META, meta);
    if (r < 0)
        goto err;
    r = archive_dict(ar, XBPS_REPODATA_STAGE, stage);
    if (r < 0)
        goto err;

    /* Write data to tempfile and rename */
    if (archive_write_close(ar) == ARCHIVE_FATAL) {
        r = -archive_errno(ar);
        if (r == 1)
            r = -EINVAL;
        xbps_error_printf("failed to close archive: %s\n", archive_error_string(ar));
        goto err;
    }
    if (archive_write_free(ar) == ARCHIVE_FATAL) {
        r = -errno;
        xbps_error_printf("failed to free archive: %s\n", strerror(-r));
        goto err;
    }

#ifdef HAVE_FDATASYNC
    fdatasync(fd);
#else
    fsync(fd);
#endif

    if (fchmod(fd, 0664) == -1) {
        errno = -r;
        xbps_error_printf("failed to set mode: %s: %s\n",
           tmp, strerror(-r));
        close(fd);
        unlink(tmp);
        return r;
    }
    close(fd);

    if (rename(tmp, path) == -1) {
        r = -errno;
        xbps_error_printf("failed to rename repodata: %s: %s: %s\n",
           tmp, path, strerror(-r));
        unlink(tmp);
        return r;
    }
    return 0;

err:
    if (ar) {
        archive_write_close(ar);
        archive_write_free(ar);
    }
    if (fd != -1)
        close(fd);
    unlink(tmp);
    return r;
}

int
repodata_commit(
    const char *repodir,
    const char *repoarch,
    xbps_dictionary_t index,
    xbps_dictionary_t stage,
    xbps_dictionary_t meta,
    const char *compression)
{
    xbps_object_iterator_t iter;
    xbps_object_t keysym;
    int r;
    xbps_dictionary_t oldshlibs, usedshlibs;

    if (xbps_dictionary_count(stage) == 0)
        return 0;

    /*
     * Find old shlibs-provides
     */
    oldshlibs = xbps_dictionary_create();
    usedshlibs = xbps_dictionary_create();

    iter = xbps_dictionary_iterator(stage);
    while ((keysym = xbps_object_iterator_next(iter))) {
        const char *pkgname = xbps_dictionary_keysym_cstring_nocopy(keysym);
        xbps_dictionary_t pkg = xbps_dictionary_get(index, pkgname);
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
    iter = xbps_dictionary_iterator(index);
    while ((keysym = xbps_object_iterator_next(iter))) {
        const char *pkgname = xbps_dictionary_keysym_cstring_nocopy(keysym);
        xbps_dictionary_t pkg = xbps_dictionary_get(stage, pkgname);
        xbps_array_t pkgshlibs;
        if (!pkg)
            pkg = xbps_dictionary_get_keysym(index, keysym);
        pkgshlibs = xbps_dictionary_get(pkg, "shlib-requires");

        for (unsigned int i = 0; i < xbps_array_count(pkgshlibs); i++) {
            const char *shlib = NULL;
            bool alloc = false;
            xbps_array_t users;
            xbps_array_get_cstring_nocopy(pkgshlibs, i, &shlib);
            if (!xbps_dictionary_get(oldshlibs, shlib))
                continue;
            users = xbps_dictionary_get(usedshlibs, shlib);
            if (!users) {
                users = xbps_array_create();
                xbps_dictionary_set(usedshlibs, shlib, users);
                alloc = true;
            }
            xbps_array_add_cstring(users, pkgname);
            if (alloc)
                xbps_object_release(users);
        }
    }
    xbps_object_iterator_release(iter);

    /*
     * purge all packages that are fullfilled by the index and
     * not in the stage.
     */
    iter = xbps_dictionary_iterator(index);
    while ((keysym = xbps_object_iterator_next(iter))) {
        xbps_dictionary_t pkg = xbps_dictionary_get_keysym(index, keysym);
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
                printf("%s%s", pre, user);
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
    } else {
        iter = xbps_dictionary_iterator(stage);
        while ((keysym = xbps_object_iterator_next(iter))) {
            const char *pkgname = xbps_dictionary_keysym_cstring_nocopy(keysym);
            xbps_dictionary_t pkg = xbps_dictionary_get_keysym(stage, keysym);
            const char *pkgver = NULL, *arch = NULL;
            xbps_dictionary_get_cstring_nocopy(pkg, "pkgver", &pkgver);
            xbps_dictionary_get_cstring_nocopy(pkg, "architecture", &arch);
            printf("index: added `%s' (%s).\n", pkgver, arch);
            xbps_dictionary_set(index, pkgname, pkg);
        }
        xbps_object_iterator_release(iter);
        stage = NULL;
    }

    r = repodata_flush(repodir, repoarch, index, stage, meta, compression);
    xbps_object_release(usedshlibs);
    xbps_object_release(oldshlibs);
    return r;
}

int
index_add_pkg(
    struct xbps_handle *xhp,
    xbps_dictionary_t index,
    xbps_dictionary_t stage,
    const char *file,
    bool force)
{
    char sha256[XBPS_SHA256_SIZE];
    char pkgname[XBPS_NAME_SIZE];
    struct stat st;
    const char *arch = NULL;
    const char *pkgver = NULL;
    xbps_dictionary_t binpkgd, curpkgd;
    int r;

    /*
     * Read metadata props plist dictionary from binary package.
     */
    binpkgd = xbps_archive_fetch_plist(file, "/props.plist");
    if (!binpkgd) {
        xbps_error_printf("index: failed to read %s metadata for "
            "`%s', skipping!\n", XBPS_PKGPROPS, file);
        return 0;
    }
    xbps_dictionary_get_cstring_nocopy(binpkgd, "architecture", &arch);
    xbps_dictionary_get_cstring_nocopy(binpkgd, "pkgver", &pkgver);
    if (!xbps_pkg_arch_match(xhp, arch, NULL)) {
        fprintf(stderr, "index: ignoring %s, unmatched arch (%s)\n", pkgver, arch);
        goto out;
    }
    if (!xbps_pkg_name(pkgname, sizeof(pkgname), pkgver)) {
        r = -EINVAL;
        goto err;
    }

    /*
     * Check if this package exists already in the index, but first
     * checking the version. If current package version is greater
     * than current registered package, update the index; otherwise
     * pass to the next one.
     */
    curpkgd = xbps_dictionary_get(stage, pkgname);
    if (!curpkgd)
        curpkgd = xbps_dictionary_get(index, pkgname);

    if (curpkgd && !force) {
        const char *opkgver = NULL, *oarch = NULL;
        int cmp;

        xbps_dictionary_get_cstring_nocopy(curpkgd, "pkgver", &opkgver);
        xbps_dictionary_get_cstring_nocopy(curpkgd, "architecture", &oarch);

        cmp = xbps_cmpver(pkgver, opkgver);
        if (cmp < 0 && xbps_pkg_reverts(binpkgd, opkgver)) {
            /*
             * If the considered package reverts the package in the index,
             * consider the current package as the newer one.
             */
            cmp = 1;
        } else if (cmp > 0 && xbps_pkg_reverts(curpkgd, pkgver)) {
            /*
             * If package in the index reverts considered package, consider the
             * package in the index as the newer one.
             */
            cmp = -1;
        }
        if (cmp <= 0) {
            fprintf(stderr, "index: skipping `%s' (%s), already registered.\n", pkgver, arch);
            goto out;
        }
    }

    if (!xbps_file_sha256(sha256, sizeof(sha256), file))
        goto err_errno;
    if (!xbps_dictionary_set_cstring(binpkgd, "filename-sha256", sha256))
        goto err_errno;
    if (stat(file, &st) == -1)
        goto err_errno;
    if (!xbps_dictionary_set_uint64(binpkgd, "filename-size", (uint64_t)st.st_size))
        goto err_errno;

    xbps_dictionary_remove(binpkgd, "pkgname");
    xbps_dictionary_remove(binpkgd, "version");
    xbps_dictionary_remove(binpkgd, "packaged-with");

    /*
     * Add new pkg dictionary into the stage index
     */
    if (!xbps_dictionary_set(stage, pkgname, binpkgd))
        goto err_errno;

out:
    xbps_object_release(binpkgd);
    return 0;
err_errno:
    r = -errno;
err:
    xbps_object_release(binpkgd);
    return r;
}
