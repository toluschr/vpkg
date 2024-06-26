#ifndef VPKG_CONFIG_HH_
#define VPKG_CONFIG_HH_

#include <string_view>
#include <string>
#include <map>

#include <xbps.h>

#include <stddef.h>

#include "package.hh"

typedef std::map<std::string_view, vpkg::package> vpkg_config;

// Planned for future removal

/*!
 * @param[in] str A non-nullterminated string, that will be parsed as ini.
 * After parsing, the config entries will point into this memory region. The
 * lifetime of config entries must not exceed the lifetime of this string.
 * @param[in] len The length of the str argument
 *
 * @return nonzero if any error occurred, the exact value is currently undefined.
 */
int vpkg_config_parse(vpkg_config *out, const char *str, size_t len);

/*!
 * @return Empty string on error, set errno
 * @return Config path on success
 */
std::string vpkg_config_path(const char *default_value);

struct vpkg_context {
    bool verbose = false;
    bool force_modified_since = false;
    bool force_reinstall = false;
    vpkg_config config;
    struct xbps_handle xbps_handle;
};

#endif // VPKG_CONFIG_HH_
