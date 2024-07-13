#include "vpkg/config.hh"
#include "vpkg/defs.h"

#include <unistd.h>
#include <stdlib.h>
#include <pwd.h>

#include "simdini/ini.h"

static int cb_ini_vpkg_config(const char *s_, size_t sl_, const char *k_, size_t kl_, const char *v_, size_t vl_, void *user_)
{
    ::vpkg::config *user = static_cast<::vpkg::config *>(user_);
    std::string_view section = s_ ? std::string_view{s_, sl_} : std::string_view{""};
    std::string_view key = std::string_view{k_, kl_};
    std::string_view value = std::string_view{v_, vl_};

    auto [iterator, _] = user->insert(std::pair<std::string_view, vpkg::package>{section, {}});

    if (section.size()) {
        if (key == "url") {
            iterator->second.url = value;
        } else if (key == "deps") {
            iterator->second.deps = value;
        } else if (key == "name") {
            iterator->second.name = value;
        } else if (key == "not_deps") {
            iterator->second.not_deps = value;
        } else if (key == "version") {
            iterator->second.version = value;
        } else if (key == "last_modified") {
            char *end;
            int eno = errno;

            unsigned long last_modified = strtoul(value.data(), &end, 10);
            if (errno != eno || *end != '\n' || end == value.data()) {
                fprintf(stderr, "unable to parse timestamp\n");
                return 1;
            }

            iterator->second.last_modified = (time_t)last_modified;
        } else {
            fprintf(stderr, "invalid key: %.*s\n", (int)key.size(), key.data());
            return 1;
        }
    } else {
        fprintf(stderr, "Not supported!\n");
        return 1;
    }

    return 0;
}

int vpkg::parse_config(::vpkg::config *out, const char *str, size_t len)
{
    if (!ini_parse_string(static_cast<const char *>(str), len, cb_ini_vpkg_config, out)) {
        return 1;
    }

    return 0;
}
