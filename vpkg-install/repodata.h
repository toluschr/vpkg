#ifndef VPKG_REPODATA_H_
#define VPKG_REPODATA_H_

#include <stdbool.h>
#include <xbps.h>

#ifdef __cplusplus
extern "C" {
#endif

int
repodata_flush(
    const char *repodir,
    const char *arch,
    xbps_dictionary_t index,
    xbps_dictionary_t stage,
    xbps_dictionary_t meta,
    const char *compression);


int
repodata_commit(
    const char *repodir,
    const char *repoarch,
    xbps_dictionary_t index,
    xbps_dictionary_t stage,
    xbps_dictionary_t meta,
    const char *compression);

int
index_add_pkg(
    struct xbps_handle *xhp,
    xbps_dictionary_t index,
    xbps_dictionary_t stage,
    const char *file,
    bool force);

#ifdef __cplusplus
}
#endif

#endif // VPKG_REPODATA_H_
