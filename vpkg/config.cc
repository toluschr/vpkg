#include "vpkg/config.hh"
#include "vpkg/defs.h"

#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <pwd.h>

#include "simdini/ini.h"

static int cb_ini_vpkg_config(const char *s_, size_t sl_, const char *k_, size_t kl_, const char *v_, size_t vl_, void *user_)
{
    ::vpkg::config *user = static_cast<::vpkg::config *>(user_);

    auto section = s_ ? std::string_view{s_, sl_} : std::string_view{""};
    auto key = std::string_view{k_, kl_};
    auto value = std::string_view{v_, vl_};
    auto version = std::string_view{};
    const char *at;

    if ((at = (const char *)memrchr(s_, '-', sl_)) && isdigit(at[1])) {
        int off = at - s_;
        version = std::string_view{section.begin() + off + 1, section.end()};
        section = section.substr(0, off);
    }

    auto [iterator, inserted] = user->insert(std::pair<std::string_view, vpkg::package>{section, {}});

    if (!inserted && version.size() && iterator->second.version.size()) {
        char this_pkgver[version.size() + 1] = {0};
        char other_pkgver[iterator->second.version.size() + 1] = {0};

        memcpy(this_pkgver, version.data(), version.size());
        memcpy(other_pkgver, iterator->second.version.data(), iterator->second.version.size());

        if (xbps_cmpver(this_pkgver, other_pkgver) < 0) {
            return 0;
        }
    }

    if (inserted && version.size()) {
        iterator->second.version = version;
    }

    if (section.size()) {
        if (key == "url") {
            iterator->second.url = value;
        } else if (key == "deps") {
            iterator->second.deps = value;
        } else if (key == "not_deps") {
            iterator->second.not_deps = value;
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
