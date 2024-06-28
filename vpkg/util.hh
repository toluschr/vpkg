#ifndef VPKG_UTIL_H_
#define VPKG_UTIL_H_

#include <xbps.h>

#include "vpkg/config.hh"
#include "vpkg/defs.h"

#ifdef __cplusplus
extern "C" {
#endif

bool is_xdeb(xbps_dictionary_t dict);
bool yes_no_prompt(void);
int xbps_vpkg_gtver(xbps_dictionary_t xpkg, const vpkg::package *vpkg);

#ifdef __cplusplus
}
#endif

#endif // VPKG_UTIL_H_
