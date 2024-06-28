#ifndef VPKG_INSTALL_HH_
#define VPKG_INSTAlL_HH_

#include "vpkg.hh"

#include <vector>

namespace vpkg {
int download_and_install_multi(struct xbps_handle *xhp, const std::vector<::vpkg::config::iterator> &download_and_install, int manual_size, bool force_reinstall, bool update);
}

#endif // VPKG_INSTALL_HH_
