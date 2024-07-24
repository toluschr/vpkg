#ifndef VPKG_REPODATA_H_
#define VPKG_REPODATA_H_

#include <stdbool.h>
#include <xbps.h>

#ifdef __cplusplus
extern "C" {
#endif

bool repodata_flush(struct xbps_handle *xhp, const char *repodir,
                    const char *reponame, xbps_dictionary_t idx,
                    xbps_dictionary_t meta, const char *compression);

bool repodata_commit(struct xbps_handle *xhp, const char *repodir,
                            xbps_dictionary_t idx, xbps_dictionary_t meta,
                            xbps_dictionary_t stage, const char *compression);

xbps_dictionary_t repodata_add(struct xbps_handle *xhp, const char *pathname,
                               xbps_dictionary_t idx, xbps_dictionary_t meta,
                               xbps_dictionary_t stage);

// int index_add(struct xbps_handle *xhp, struct xbps_repo *repo, char **argv, int nargs);

#ifdef __cplusplus
}
#endif

#endif // VPKG_REPODATA_H_
