#ifndef VPKG_UTIL_H_
#define VPKG_UTIL_H_

#include <xbps.h>

#include "vpkg/config.hh"

#define RETRY_EINTR(C) ({ int rc; while ((rc = (C)) < 0 && errno == EINTR); rc; })
#define ASSERT_NOERR(C) assert((C) == 0);

bool is_xdeb(xbps_dictionary_t dict);
bool yes_no_prompt(void);
int xbps_vpkg_gtver(xbps_dictionary_t xpkg, const vpkg::package *vpkg);

__attribute__((noreturn))
void perror_exit(const char *msg);

void free_preserve_errno(void *ptr);

#endif // VPKG_UTIL_H_
