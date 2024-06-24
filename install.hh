#ifndef VPKG_INSTALL_HH_
#define VPKG_INSTAlL_HH_

#include <vector>
#include "config.hh"

int vpkg_download_and_install_multi(struct xbps_handle *xhp, const std::vector<vpkg_config::iterator> &packages_to_update, bool force_reinstall);

int vpkg_do_install(vpkg_context *ctx, int argc, char **argv);

#endif // VPKG_INSTALL_HH_
