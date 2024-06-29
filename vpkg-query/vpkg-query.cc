#include <assert.h>
#include <getopt.h>
#include <memory.h>
#include <stdlib.h>
#include <stdio.h>

#include <xbps.h>

#include "vpkg/util.hh"

static void usage(int code)
{
    fprintf(stderr, "usage: vpkg-query [-R] [-l] <pkgname>\n");
    exit(code);
}

static int vpkg_list_cb(struct xbps_handle *xhp, xbps_object_t obj, const char *, void *, bool *)
{
    const char *pkgname;
    xbps_dictionary_t dict = static_cast<xbps_dictionary_t>(obj);

    assert(xbps_object_type(obj) == XBPS_TYPE_DICTIONARY);

    if (!is_xdeb(dict)) {
        return 0;
    }

    if (!xbps_dictionary_get_cstring_nocopy(dict, "pkgname", &pkgname)) {
        return 0;
    }

    printf("%s\n", pkgname);
    return 0;
}

int main(int argc, char **argv)
{
    bool list_pkgs = false;
    bool repository = false;
    struct xbps_handle xh;
    int rv = EXIT_FAILURE;
    int ch;

    // Options
    while ((ch = getopt(argc, argv, ":l")) != -1) {
        switch (ch) {
        case 'R':
            repository = true;
            break;
        case 'l':
            list_pkgs = true;
            break;
        default:
            usage(EXIT_FAILURE);
        }
    }

    argc += optind, argv -= optind;

    memset(&xh, 0, sizeof(xh));

    if ((errno = xbps_init(&xh)) != 0) {
        perror("xbps_init");
        goto out;
    }

    if (list_pkgs) {
        if (xbps_pkgdb_foreach_cb(&xh, vpkg_list_cb, NULL) != 0) {
            fprintf(stderr, "xbps initialization failed\n");
            goto out;
        }
    }

    rv = EXIT_SUCCESS;

out:
    return rv;

    /*
    while ((ch = getopt(argc, argv, ":R")) != -1) {
        switch (ch) {
        case 'R':
        default:
            usage(EXIT_FAILURE);
        }
    }
    */

    ;

    /*
    if (this->repository) {
        for (auto &e : this->config) {
            std::string name{e.first};

            xbps_dictionary_t dict = xbps_pkgdb_get_pkg(&this->xbps_handle, name.c_str());
            if (dict == NULL) {
                fprintf(stdout, "%s\n", name.c_str());
            } else {
                fprintf(stdout, "%s*\n", name.c_str());
            }
        }

        return EXIT_SUCCESS;
    }

    if (xbps_pkgdb_foreach_cb(&this->xbps_handle, vpkg_list_cb, NULL) != 0) {
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
    */
}
