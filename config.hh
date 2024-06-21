#ifndef VPKG_CONFIG_HH_
#define VPKG_CONFIG_HH_

#include <string_view>
#include <string>
#include <map>

#include <stddef.h>

struct vpkg_config_entry {
    std::string_view name;
    std::string_view url;
    std::string_view base_url;
    std::string_view deps;
};

typedef std::map<std::string_view, vpkg_config_entry> vpkg_config;

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

#endif // VPKG_CONFIG_HH_
