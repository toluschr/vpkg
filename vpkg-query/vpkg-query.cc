#include <assert.h>
#include <getopt.h>
#include <memory.h>
#include <stdlib.h>
#include <stdio.h>

#include <xbps.h>

#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

#include "vpkg/util.hh"

static void usage(int code)
{
    fprintf(stderr, "usage: vpkg-query [-c <config_path>] [-R] [-lv] <pkgname>\n");
    exit(code);
}

static int vpkg_list_cb(struct xbps_handle *xhp, xbps_object_t obj, const char *pkgname, void *str, bool *)
{
    xbps_dictionary_t dict = static_cast<xbps_dictionary_t>(obj);
    assert(xbps_object_type(obj) == XBPS_TYPE_DICTIONARY);

    if (!is_xdeb(dict)) {
        return 0;
    }

    if (!str || strstr(pkgname, (const char *)str)) {
        printf("%s\n", pkgname);
    }

    return 0;
}

int main(int argc, char **argv)
{
    const char *config_path = VPKG_CONFIG_PATH;

    const char *filter_pkgname = NULL;
    size_t filter_pkgname_length = 0;

    bool list = false;
    bool repository = false;

    vpkg::config config;
    struct xbps_handle xh;
    int rv = EXIT_FAILURE;
    int ch;

    memset(&xh, 0, sizeof(xh));

    // Options
    while ((ch = getopt(argc, argv, ":c:Rlv")) != -1) {
        switch (ch) {
        case 'R':
            repository = true;
            break;
        case 'c':
            config_path = optarg;
            break;
        case 'l':
            list = true;
            break;
        case 'v':
            fprintf(stderr, "vpkg-%s\n", VPKG_REVISION);
            exit(EXIT_FAILURE);
            break;
        default:
            usage(EXIT_FAILURE);
            break;
        }
    }

    argc -= optind, argv += optind;

    if (argc) {
        filter_pkgname = argv[0];
        filter_pkgname_length = strlen(argv[0]);
    }

    if (vpkg::config_init(&config, config_path) != 0) {
        perror("failed to parse config file");
        goto end_munmap;
    }

    if ((errno = xbps_init(&xh)) != 0) {
        perror("xbps_init");
        goto out;
    }

    if (list && repository) {
        for (auto &it : config.packages) {
            if (!filter_pkgname || memmem(it.first.data(), it.first.size(), filter_pkgname, filter_pkgname_length)) {
                fprintf(stderr, "%.*s\n", (int)it.first.size(), it.first.data());
            }
        }
    } else if (list) {
        if (xbps_pkgdb_foreach_cb(&xh, vpkg_list_cb, (void *)filter_pkgname) != 0) {
            fprintf(stderr, "xbps initialization failed\n");
            goto out;
        }
    }

    rv = EXIT_SUCCESS;

end_xbps_lock:
end_xbps:
    xbps_end(&xh);

end_munmap:
    vpkg::config_fini(&config);

out:
    return rv;
}
