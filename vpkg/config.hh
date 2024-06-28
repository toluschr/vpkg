#ifndef VPKG_CONFIG_HH_
#define VPKG_CONFIG_HH_

#include <string_view>
#include <string>
#include <map>

#include <xbps.h>

#include <stddef.h>

namespace vpkg {
struct package {
    std::string_view url;
    std::string_view name;
    std::string_view deps;
    std::string_view version;
    std::string_view not_deps;
    time_t last_modified;
};

using config = typename std::map<std::string_view, vpkg::package>;

/*!
 * @return Empty string on error, set errno
 * @return Config path on success
 */
std::string config_path(const char *default_value);

/*!
 * @param[in] str A non-nullterminated string, that will be parsed as ini.
 * After parsing, the config entries will point into this memory region. The
 * lifetime of config entries must not exceed the lifetime of this string.
 * @param[in] len The length of the str argument
 *
 * @return nonzero if any error occurred, the exact value is currently undefined.
 */
int parse_config(::vpkg::config *out, const char *str, size_t len);
}

#endif // VPKG_CONFIG_HH_
