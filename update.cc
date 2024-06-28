#include "update.hh"

#include <curl/curl.h>
#include <sys/wait.h>
#include <atomic>

#include <semaphore.h>
#include <pthread.h>
#include <memory.h>
#include <libgen.h>
#include <assert.h>
#include <math.h>

#include <vector>
#include <string>

#include "install.hh"
#include "repodata.h"
#include "util.h"

struct vpkg_check_update_cb_data {
    vpkg::vpkg *ctx;
    sem_t sem_data;
    std::vector<::vpkg::config::iterator> packages_to_update;
};

static int vpkg_check_update_cb(struct xbps_handle *xhp, xbps_object_t obj, const char *, void *user_, bool *)
{
    // time_t last_modified;
    const char *next;
    const char *pkgver;
    const char *pkgname;
    const char *install_date;
    xbps_dictionary_t dict = static_cast<xbps_dictionary_t>(obj);
    vpkg_check_update_cb_data *user = static_cast<vpkg_check_update_cb_data *>(user_);

    assert(xbps_object_type(obj) == XBPS_TYPE_DICTIONARY);

    if (!is_xdeb(dict)) {
        return 0;
    }

    if (!xbps_dictionary_get_cstring_nocopy(dict, "pkgver", &pkgver)) {
        return 0;
    }

    if (!xbps_dictionary_get_cstring_nocopy(dict, "pkgname", &pkgname)) {
        return 0;
    }

    if (!xbps_dictionary_get_cstring_nocopy(dict, "install-date", &install_date)) {
        return 0;
    }

    // If not in config
    auto it = user->ctx->config.find(pkgname);
    if (it == user->ctx->config.end()) {
        return 0;
    }

    if (user->ctx->force_update) {
        goto insert_and_out;
    }

    if (it->second.version.size()) {
        std::string new_version;
        std::string old_version;
        const char *version = xbps_pkg_version(pkgver);
        const char *revision = xbps_pkg_revision(pkgver);

        if (version == NULL || revision == NULL) {
            fprintf(stderr, "unable to get version or revision: %s\n", pkgver);
            return 0;
        }

        assert(revision >= version);

        old_version = std::string{version, (long unsigned int)(revision - version)};
        new_version = std::string{it->second.version};

        if (xbps_cmpver(old_version.c_str(), new_version.c_str()) <= 0) {
            return 0;
        }
    } else if (it->second.last_modified != 0) {
        struct tm t;

        // Parse timestamp
        memset(&t, 0, sizeof(t));

        next = strptime(install_date, "%Y-%m-%d %H:%M %Z", &t);
        if (next == NULL || *next != '\0' || next == install_date) {
            fprintf(stderr, "%s: failed to parse package install_date\n", pkgname);
            return 0;
        }

        if (it->second.last_modified <= mktime(&t)) {
            return 0;
        }
    } else {
        fprintf(stderr, "%s: Neither version nor last_modified set. Will never update.\n", pkgname);
        return 0;
    }

insert_and_out:
    // @todo: Handle EINTR
    sem_wait(&user->sem_data);
    user->packages_to_update.push_back(it);
    sem_post(&user->sem_data);

    return 0;
}


int vpkg::vpkg::cmd_update(int argc, char **argv)
{
    if (argc != 0) {
        fprintf(stderr, "usage: vpkg update\n");
        return EXIT_FAILURE;
    }

    vpkg_check_update_cb_data d;
    d.ctx = this;

    if ((errno = sem_init(&d.sem_data, 0, 1)) != 0) {
        perror("sem_init failed");
        return EXIT_FAILURE;
    }

    // @todo: Filter inside threads aswell.
    if ((errno = xbps_pkgdb_foreach_cb_multi(&this->xbps_handle, vpkg_check_update_cb, &d)) != 0) {
        fprintf(stderr, "xbps_pkgdb_foreach_cb_multi failed\n");
        return EXIT_FAILURE;
    }

    if (d.packages_to_update.size() == 0) {
        fprintf(stderr, "Nothing to do.\n");
        return EXIT_FAILURE;
    }

    return ::vpkg::download_and_install_multi(&this->xbps_handle, d.packages_to_update, d.packages_to_update.size(), this->force_install, true);
}
