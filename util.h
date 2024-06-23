#ifndef VPKG_UTIL_H_
#define VPKG_UTIL_H_

#include <xbps.h>

#define VPKG_TEMPDIR "/tmp/vpkg"
#define VPKG_BINPKGS "/var/lib/vpkg"
#define VPKG_XDEB_SHLIBS "/tmp/vpkg/shlibs"

#ifdef __cplusplus
extern "C" {
#endif

bool is_xdeb(xbps_object_t obj);
bool yes_no_prompt(void);

#ifdef __cplusplus
}
#endif

#endif // VPKG_UTIL_H_
