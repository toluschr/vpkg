#include "util.hh"

#include <algorithm>
#include <sys/stat.h>

#include <assert.h>
#include <string.h>
#include <stdlib.h>

bool is_xdeb(xbps_dictionary_t dict)
{
    const char *tags;

    if (!xbps_dictionary_get_cstring_nocopy(dict, "tags", &tags)) {
        return false;
    }

    if (strcmp(tags, "xdeb") != 0) {
        return false;
    }

    return true;
}

bool yes_no_prompt(void)
{
    bool ok;
    int ch;

    fprintf(stderr, "Do you want to continue? [Y/n] ");

    if (fflush(stderr) < 0) {
        return false;
    }

    ch = fgetc(stdin);
    ok = ch == '\n' || ch == EOF || ch == 'y' || ch == 'Y';

    while (ch != '\n' && ch != EOF)
        ch = fgetc(stdin);

    return ok && !ferror(stdin);
}

int xbps_vpkg_gtver(xbps_dictionary_t xpkg, const vpkg::package *vpkg)
{
    const char *pkgver;

    assert(xpkg != NULL);

    if (!xbps_dictionary_get_cstring_nocopy(xpkg, "pkgver", &pkgver)) {
        return -1;
    }

    if (vpkg->version.size()) {
        std::string new_version;
        std::string old_version;
        const char *version = xbps_pkg_version(pkgver);
        const char *revision = xbps_pkg_revision(pkgver);

        if (version == NULL || revision == NULL) {
            return -1;
        }

        assert(revision >= version);

        old_version = std::string{version, (long unsigned int)(revision - version - 1)};
        new_version = std::string{vpkg->version};

        std::replace(new_version.begin(), new_version.end(), '-', '.');
        std::replace(new_version.begin(), new_version.end(), '_', '.');
        std::replace(new_version.begin(), new_version.end(), '/', '.');

        return (xbps_cmpver(old_version.c_str(), new_version.c_str()) < 0);
    }

    if (vpkg->last_modified != 0) {
        const char *install_date;
        const char *repository;
        const char *arch;
        time_t time;
        char *buf;


        if (xbps_dictionary_get_cstring_nocopy(xpkg, "install-date", &install_date)) {
            struct tm t;

            char *next;
            memset(&t, 0, sizeof(t));

            // Parse timestamp
            next = strptime(install_date, "%Y-%m-%d %H:%M %Z", &t);
            if (next == NULL || *next != '\0' || next == install_date) {
                return -1;
            }

            time = mktime(&t);
        } else {
            if (!xbps_dictionary_get_cstring_nocopy(xpkg, "repository", &repository)) {
                return -1;
            }

            if (!xbps_dictionary_get_cstring_nocopy(xpkg, "architecture", &arch)) {
                return -1;
            }

            if (asprintf(&buf, "%s/%s.%s.xbps", repository, pkgver, arch) < 0) {
                return -1;
            }

            struct stat st;
            if (stat(buf, &st) < 0) {
                free(buf);
                return -1;
            }

            time = st.st_mtime;
            free(buf);
        }


        return (time < vpkg->last_modified);
    }

    return -1;
}

void perror_exit(const char *msg)
{
    perror(msg);
    exit(EXIT_FAILURE);
}

void free_preserve_errno(void *ptr)
{
    int e = errno;
    free(ptr);
    errno = e;
}
