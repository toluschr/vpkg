#include "util.hh"

#include <algorithm>

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
            fprintf(stderr, "unable to get version or revision: %s\n", pkgver);
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

        if (!xbps_dictionary_get_cstring_nocopy(xpkg, "install-date", &install_date)) {
            return -1;
        }

        char *next;
        struct tm t;

        // Parse timestamp
        memset(&t, 0, sizeof(t));

        next = strptime(install_date, "%Y-%m-%d %H:%M %Z", &t);
        if (next == NULL || *next != '\0' || next == install_date) {
            fprintf(stderr, "%s: uable to parse install_date '%s'\n", pkgver, install_date);
            return -1;
        }

        return (mktime(&t) < vpkg->last_modified);
    }

    return -1;
}

