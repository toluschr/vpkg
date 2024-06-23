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

struct vpkg_check_update_cb_data {
    vpkg_context *ctx;
    sem_t sem_data;
    std::vector<vpkg_config::iterator> packages_to_update;
};

static time_t get_last_modified(const char *url)
{
    long filetime = 0;
    CURLcode code = CURLE_OK;
    CURL *curl = curl_easy_init();

    if (curl == NULL) {
        return (time_t)(-1);
    }

    code = (code == CURLE_OK) ? curl_easy_setopt(curl, CURLOPT_URL, url) : code;
    code = (code == CURLE_OK) ? curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L) : code;
    code = (code == CURLE_OK) ? curl_easy_setopt(curl, CURLOPT_NOBODY, 1L) : code;
    code = (code == CURLE_OK) ? curl_easy_setopt(curl, CURLOPT_FILETIME, 1L) : code;
    code = (code == CURLE_OK) ? curl_easy_perform(curl) : code;
    code = (code == CURLE_OK) ? curl_easy_getinfo(curl, CURLINFO_FILETIME, &filetime) : code;

    curl_easy_cleanup(curl);
    return (code == CURLE_OK) ? (time_t)filetime : (time_t)(-1);
}

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

    /*
    for (auto &it : user->ctx->config) {
        fprintf(stderr, "%.*s\n", it.first.size(), it.first.data());
    }
    */

    // If not in config
    auto it = user->ctx->config.find(pkgname);
    if (it == user->ctx->config.end()) {
        return 0;
    }

    if (user->ctx->force) {
        goto insert_and_out;
    }

    // Parse timestamp
    memset(&t, 0, sizeof(t));

    next = strptime(install_date, "%Y-%m-%d %H:%M %Z", &t);
    if (next == NULL || *next != '\0' || next == install_date) {
        fprintf(stderr, "%s: failed to parse package install_date\n", pkgname);
        return 0;
    }

    last_modified = get_last_modified(std::string{it->second.url}.c_str());
    if (last_modified == (time_t)-1) {
        fprintf(stderr, "Error while making request\n");
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

    if (system("xdeb -SQ >/dev/null 2>&1") != EXIT_SUCCESS) {
        fprintf(stderr, "failed to update shlibs\n");
        return -1;
    }

    // @todo: Filter inside threads aswell.
    if ((errno = xbps_pkgdb_foreach_cb_multi(&ctx->xbps_handle, vpkg_check_update_cb, &d)) != 0) {
        fprintf(stderr, "xbps_pkgdb_foreach_cb_multi failed\n");
        return -1;
    }

    return vpkg_download_and_install_multi(&ctx->xbps_handle, d.packages_to_update);
}
