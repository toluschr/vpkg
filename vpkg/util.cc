#include "util.h"

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

#if 0
bool is_vpkg(xbps_dictionary_t dict)
{
    const char *pkgname;

    if (!is_xbps(dict)) {
        return false;
    }

    if (!xbps_dictionary_get_cstring_nocopy(dict, "pkgname", &pkgname)) {
        return false;
    }

    /*
    if () {
    }
    */

    return true;
}
#endif

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
