#include "vpkg.hh"

#include "util.h"

#include <assert.h>

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

int vpkg::vpkg::cmd_list(int argc, char **argv) noexcept
{
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
}
