#ifndef VPKG_INSTALL_HH_
#define VPKG_INSTAlL_HH_

#include <vector>
#include "config.hh"

int vpkg_download_and_install_multi(struct xbps_handle *xhp, const std::vector<vpkg_config::iterator> &packages_to_update);

#endif // VPKG_INSTALL_HH_
