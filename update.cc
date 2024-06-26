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
#include "version.hh"
#include "repodata.h"

struct vpkg_check_update_cb_data {
    vpkg_context *ctx;
    sem_t sem_data;
    std::vector<vpkg_config::iterator> packages_to_update;
};

static int vpkg_check_update_cb(struct xbps_handle *xhp, xbps_object_t obj, const char *, void *user_, bool *)
{
    struct tm t;
    time_t last_modified;
    const char *next;
    const char *pkgname;
    const char *install_date;
    const char *build_options;
    xbps_dictionary_t dict = static_cast<xbps_dictionary_t>(obj);
    vpkg_check_update_cb_data *user = static_cast<vpkg_check_update_cb_data *>(user_);
    // vpkg_context *ctx = 

    assert(xbps_object_type(obj) == XBPS_TYPE_DICTIONARY);

    if (!is_xdeb(obj)) {
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

    std::string url;
    if (it->second.resolve_url(user->ctx->force_modified_since ? NULL : &last_modified) == -1) {
        return -1;
    }

    if (user->ctx->force_modified_since) {
        goto insert_and_out;
    }

    // Parse timestamp
    memset(&t, 0, sizeof(t));

    next = strptime(install_date, "%Y-%m-%d %H:%M %Z", &t);
    if (next == NULL || *next != '\0' || next == install_date) {
        fprintf(stderr, "%s: failed to parse package install_date\n", pkgname);
        return 0;
    }

    if (last_modified <= mktime(&t)) {
        return 0;
    }

insert_and_out:
    // @todo: Handle EINTR
    sem_wait(&user->sem_data);
    user->packages_to_update.push_back(it);
    sem_post(&user->sem_data);

    return 0;
}


int vpkg_do_update(vpkg_context *ctx, int argc, char **argv)
{
    if (argc != 0) {
        fprintf(stderr, "usage: vpkg update\n");
        return -1;
    }

    vpkg_check_update_cb_data d;
    d.ctx = ctx;

    if ((errno = sem_init(&d.sem_data, 0, 1)) != 0) {
        perror("sem_init failed");
        return -1;
    }

    // @todo: Filter inside threads aswell.
    if ((errno = xbps_pkgdb_foreach_cb_multi(&ctx->xbps_handle, vpkg_check_update_cb, &d)) != 0) {
        fprintf(stderr, "xbps_pkgdb_foreach_cb_multi failed\n");
        return -1;
    }

    if (d.packages_to_update.size() == 0) {
        fprintf(stderr, "Nothing to do.\n");
        return -1;
    }

    return vpkg_download_and_install_multi(&ctx->xbps_handle, d.packages_to_update, ctx->force_reinstall);
}
