#include <sys/stat.h>
#include <sys/mman.h>
#include <string.h>

#include <stdlib.h>
#include <stdio.h>

#include <assert.h>
#include <unistd.h>
#include <fcntl.h>
#include <pwd.h>

#include <string>
#include <limits>
#include <map>

#include <curl/curl.h>
#include <semaphore.h>
#include <pthread.h>
#include <math.h>
#include <xbps.h>

#include "simdini/ini.h"
#include "config.hh"

#include <queue>

#define VPKG_TEMPDIR "/tmp/vpkg"
#define VPKG_XDEB_SHLIBS "/tmp/vpkg/shlibs"

static const char *cmd_config_path = NULL;
static bool cmd_verbose = false;
static bool cmd_force = false;

static sem_t sem_global_data;
// static sem_t sem_download;

enum vpkg_package_state {
    STATE_DOWNLOAD,
    STATE_INSTALL,
    STATE_ABORT,
};

std::vector<vpkg_config::iterator> packages_to_update;

static void die(const char *message)
{
    perror(message);
    exit(EXIT_FAILURE);
}

static void usage(int code)
{
    fprintf(stderr, "usage: vpkg <list|update|upgrade>\n");
    exit(code);
}

static bool is_xdeb(xbps_object_t obj)
{
    const char *cstring;
    xbps_dictionary_t dict = static_cast<xbps_dictionary_t>(obj);

    if (xbps_object_type(obj) != XBPS_TYPE_DICTIONARY) {
        return false;
    }

    if (!xbps_dictionary_get_cstring_nocopy(dict, "tags", &cstring)) {
        return false;
    }

    if (strcmp(cstring, "xdeb") != 0) {
        return false;
    }

    return true;
}

static int vpkg_list_cb(struct xbps_handle *xhp, xbps_object_t obj, const char *, void *, bool *)
{
    const char *pkgname;
    // const char *build_options;
    xbps_dictionary_t dict = static_cast<xbps_dictionary_t>(obj);

    assert(xbps_object_type(obj) == XBPS_TYPE_DICTIONARY);

    if (!is_xdeb(obj)) {
        return 0;
    }

    if (!xbps_dictionary_get_cstring_nocopy(dict, "pkgname", &pkgname)) {
        return 0;
    }

    // if (!xbps_dictionary_get_cstring_nocopy(dict, "build-options", &build_options)) {
        // return 0;
    // }

    // printf("%s\n", build_options);

    printf("%s\n", pkgname);
    return 0;
}

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

    if (code != CURLE_OK) {
        curl_easy_cleanup(curl);
        return (time_t)(-1);
    }

    curl_easy_cleanup(curl);
    return (time_t)filetime;
}

static int vpkg_check_update_cb(struct xbps_handle *xhp, xbps_object_t obj, const char *, void *user_data, bool *)
{
    struct tm t;
    time_t last_modified;
    const char *next;
    const char *pkgname;
    const char *install_date;
    const char *build_options;
    xbps_dictionary_t dict = static_cast<xbps_dictionary_t>(obj);
    vpkg_config *config = static_cast<vpkg_config *>(user_data);

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

    if (!xbps_dictionary_get_cstring_nocopy(dict, "build-options", &build_options)) {
        return 0;
    }

    // If not in config
    auto it = config->find(pkgname);
    if (it == config->end()) {
        return 0;
    }

    if (cmd_force) {
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
    sem_wait(&sem_global_data);
    packages_to_update.push_back(it);
    sem_post(&sem_global_data);

    return 0;
}

static bool vpkg_do_list(struct xbps_handle *xhp, vpkg_config *conf)
{
    xbps_pkgdb_foreach_cb(xhp, vpkg_list_cb, NULL);
    return true;
}

static bool yes_no_prompt(void)
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

static bool vpkg_do_update(struct xbps_handle *xhp, vpkg_config *conf)
{
    if (system("xdeb -SQ >/dev/null 2>&1") != EXIT_SUCCESS) {
        fprintf(stderr, "Failed to update shlibs\n");
        return false;
    }

    long maxthreads = sysconf(_SC_NPROCESSORS_ONLN);
    if (maxthreads < 0) {
        // @todo: handle this
        return false;
    }

    pthread_t threads[maxthreads];

    xbps_pkgdb_foreach_cb_multi(xhp, vpkg_check_update_cb, conf);

    printf("The following packages may be updated:\n");
    for (auto &it : packages_to_update) {
        printf("%s\n", std::string{it->first}.c_str());
    }

    if (!yes_no_prompt()) {
        fprintf(stderr, "Aborting!\n");
        return false;
    }


    // struct {
    //     std::vector<vpkg_config::iterator> *packages_to_update;
    //     int from;
    //     int to;
    // };

    // pthread_t threads;
    // pthread_create(&threads, NULL, NULL, NULL);

    for (auto &it : packages_to_update) {
        printf("%s\n", std::string{it->first}.c_str());
    }

    return true;
}

int main(int argc, char **argv)
{
    void *data = NULL;
    int rc = EXIT_SUCCESS;
    int rv;
    struct stat st;
    vpkg_config conf;
    xbps_handle handle;
    int config_fd;
    int opt;

    /*
    if ((errno = pthread_mutex_init(&mutex_global_data, NULL)) != 0) {
        perror("failed to create mutex");
        exit(EXIT_FAILURE);
    }
    */

    if ((errno = sem_init(&sem_global_data, 0, 1)) != 0) {
        perror("sem_init failed");
        exit(EXIT_FAILURE);
    }

    /*
    if ((errno = sem_init(&sem_download, 0, 0)) != 0) {
        perror("sem_init failed");
        exit(EXIT_FAILURE);
    }
    */

    memset(&handle, 0, sizeof(handle));
    curl_global_init(CURL_GLOBAL_ALL);

    while ((opt = getopt(argc, argv, ":c:vf")) != -1) {
        switch (opt) {
        case 'c':
            cmd_config_path = optarg;
            break;
        case 'v':
            cmd_verbose = true;
            break;
        case 'f':
            cmd_force = true;
            break;
        default:
            usage(EXIT_FAILURE);
        }
    }

    argc -= optind, argv += optind;

    if (argc == 0) {
        usage(EXIT_FAILURE);
    }

    {
        std::string config_path;

        config_path = vpkg_config_path(cmd_config_path);
        if (config_path.size() == 0) {
            die("unable to canonicalize config path");
        }

        config_fd = open(config_path.c_str(), O_RDONLY);
    }

    if (config_fd < 0) {
        die("unable to open config file");
    }

    if (fstat(config_fd, &st) < 0) {
        die("unable to stat config file");
    }

    if (st.st_size != 0) {
        data = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, config_fd, 0);
        if (data == MAP_FAILED) {
            die("unable to map config file");
        }

        if (vpkg_config_parse(&conf, static_cast<const char *>(data), st.st_size) != 0) {
            fprintf(stderr, "unable to parse config file\n");
            goto end_munmap;
        }
    }

    xbps_init(&handle);

    if ((rv = xbps_pkgdb_lock(&handle)) != 0) {
        xbps_error_printf("failed to lock pkgdb: %s\n", strerror(rv));
        goto end_xbps;
    }

    if (setenv("XDEB_SHLIBS", VPKG_XDEB_SHLIBS, 1) != 0) {
        perror("failed to set shlibs env");
        goto end_xbps;
    }

    if (mkdir("/tmp/vpkg", 0644) < 0 && errno != EEXIST) {
        perror("failed to create tempdir");
        goto end_xbps;
    }

    if (strcmp(argv[0], "list") == 0) {
        rc = vpkg_do_list(&handle, &conf) ? EXIT_SUCCESS : EXIT_FAILURE;
    } else if (strcmp(argv[0], "update") == 0) {
        rc = vpkg_do_update(&handle, &conf) ? EXIT_SUCCESS : EXIT_FAILURE;
    } else {
        usage(EXIT_FAILURE);
    }

end_cleanup_tempdir:

end_xbps:
    xbps_end(&handle);

end_munmap:
    if (data != NULL) {
        munmap(data, st.st_size);
    }

    close(config_fd);
    return rc;
}
