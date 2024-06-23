#include "util.h"

#include <string.h>
#include <stdlib.h>

bool is_xdeb(xbps_object_t obj)
{
    const char *tags;
    xbps_dictionary_t dict = static_cast<xbps_dictionary_t>(obj);

    if (xbps_object_type(obj) != XBPS_TYPE_DICTIONARY) {
        return false;
    }

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
