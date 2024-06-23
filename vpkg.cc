#include <sys/stat.h>
#include <sys/mman.h>
#include <string.h>

#include <stdlib.h>
#include <stdio.h>

#include <assert.h>
#include <unistd.h>
#include <fcntl.h>
#include <pwd.h>

#include <filesystem>
#include <string>
#include <limits>
#include <map>

#include <curl/curl.h>
#include <semaphore.h>
#include <pthread.h>
#include <math.h>
#include <xbps.h>

#include "simdini/ini.h"
#include "update.hh"
#include "config.hh"
#include "util.h"

#include <queue>

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

static bool vpkg_do_list(struct xbps_handle *xhp, vpkg_config *conf)
{
    xbps_pkgdb_foreach_cb(xhp, vpkg_list_cb, NULL);
    return true;
}

int main(int argc, char **argv)
{
    const char *config_path = nullptr;
    struct vpkg_context ctx;
    std::error_code ec;

    void *data = nullptr;
    int rc = EXIT_SUCCESS;
    int rv;
    struct stat st;
    int config_fd;
    int opt;

    memset(&ctx.xbps_handle, 0, sizeof(ctx.xbps_handle));
    curl_global_init(CURL_GLOBAL_ALL);

    while ((opt = getopt(argc, argv, ":c:vf")) != -1) {
        switch (opt) {
        case 'c':
            config_path = optarg;
            break;
        case 'v':
            ctx.verbose = true;
            break;
        case 'f':
            ctx.force = true;
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
        std::string resolved_config_path;

        resolved_config_path = vpkg_config_path(config_path);
        if (resolved_config_path.size() == 0) {
            die("unable to canonicalize config path");
        }

        config_fd = open(resolved_config_path.c_str(), O_RDONLY);
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

        if (vpkg_config_parse(&ctx.config, static_cast<const char *>(data), st.st_size) != 0) {
            fprintf(stderr, "unable to parse config file\n");
            goto end_munmap;
        }
    }

    xbps_init(&ctx.xbps_handle);

    if ((rv = xbps_pkgdb_lock(&ctx.xbps_handle)) != 0) {
        xbps_error_printf("failed to lock pkgdb: %s\n", strerror(rv));
        goto end_xbps;
    }

    if (setenv("XDEB_SHLIBS", VPKG_XDEB_SHLIBS, 1) != 0) {
        perror("failed to set shlibs env");
        goto end_xbps_lock;
    }

    if (mkdir(VPKG_TEMPDIR, 0644) < 0 && errno != EEXIST) {
        perror("failed to create tempdir");
        goto end_xbps_lock;
    }

    if (strcmp(argv[0], "list") == 0) {
        rc = vpkg_do_list(&ctx.xbps_handle, &ctx.config) ? EXIT_SUCCESS : EXIT_FAILURE;
    } else if (strcmp(argv[0], "update") == 0) {
        rc = (vpkg_do_update(&ctx, argc - 1, &argv[1]) == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
        /*
    } else if (strcmp(argv[0], "install") == 0) {
        rc = vpkg_do_install(&ctx) ? EXIT_SUCCESS : EXIT_FAILURE;
        */
    } else {
        usage(EXIT_FAILURE);
    }

end_cleanup_tempdir:
    if (std::filesystem::remove_all(VPKG_TEMPDIR, ec) == static_cast<std::uintmax_t>(-1)) {
        fprintf(stderr, "failed to cleanup tempdir\n");
    }

end_xbps_lock:
    ;
    // free(ctx.xbps_handle.pkgdb_plist);
    // ctx.xbps_handle.pkgdb_plist = NULL;

end_xbps:
    xbps_end(&ctx.xbps_handle);

end_munmap:
    if (data != NULL) {
        munmap(data, st.st_size);
    }

    close(config_fd);
    return rc;
}
