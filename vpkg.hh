#ifndef VPKG_CONFIG_HH_
#define VPKG_CONFIG_HH_

#include <string_view>
#include <string>
#include <map>

#include <xbps.h>

#include <stddef.h>

#include "package.hh"

namespace vpkg {
using config = typename std::map<std::string_view, vpkg::package>;

struct vpkg {
    bool verbose;
    bool repository;
    bool force_update;
    bool force_install;
    ::vpkg::config config;
    ::xbps_handle xbps_handle;

    int cmd_list(int argc, char **argv);
    int cmd_update(int argc, char **argv);
    int cmd_install(int argc, char **argv);
};

/*!
 * @return Empty string on error, set errno
 * @return Config path on success
 */
std::string config_path(const char *default_value);
}

// Planned for future removal

/*!
 * @param[in] str A non-nullterminated string, that will be parsed as ini.
 * After parsing, the config entries will point into this memory region. The
 * lifetime of config entries must not exceed the lifetime of this string.
 * @param[in] len The length of the str argument
 *
 * @return nonzero if any error occurred, the exact value is currently undefined.
 */
int vpkg_config_parse(::vpkg::config *out, const char *str, size_t len);

#endif // VPKG_CONFIG_HH_
