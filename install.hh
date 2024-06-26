#ifndef VPKG_INSTALL_HH_
#define VPKG_INSTAlL_HH_

#include <vector>
#include "config.hh"

namespace vpkg {
int download_and_install_multi(struct xbps_handle *xhp, const std::vector<vpkg_config::iterator> &download_and_install, int manual_size, bool force_reinstall, bool update);

int do_install(vpkg_context *ctx, int argc, char **argv);
}

#endif // VPKG_INSTALL_HH_
