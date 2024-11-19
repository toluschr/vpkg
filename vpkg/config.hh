#ifndef VPKG_CONFIG_HH_
#define VPKG_CONFIG_HH_

#include <string_view>
#include <string>
#include <map>

#include <stddef.h>
#include <xbps.h>

#include "defs.h"

namespace vpkg {
struct package {
    std::string_view url{};
    std::string_view deps{};
    std::string_view provides{};
    std::string_view replaces{};
    std::string_view version{};
    std::string_view not_deps{};
    time_t last_modified{0};
};

using packages = std::map<std::string_view, vpkg::package>;

struct config {
    ::vpkg::packages packages;

    void *mem;
    size_t len;
};

/*!
 * @param[in] str A non-nullterminated string, that will be parsed as ini.
 * After parsing, the config entries will point into this memory region. The
 * lifetime of config entries must not exceed the lifetime of this string.
 * @param[in] len The length of the str argument
 *
 * @return nonzero if any error occurred, the exact value is currently undefined.
 */
int config_init(::vpkg::config *config, const char *config_path);
void config_fini(::vpkg::config *config);
}

#endif // VPKG_CONFIG_HH_
