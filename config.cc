#include "config.hh"

#include <unistd.h>
#include <stdlib.h>
#include <pwd.h>

#include "simdini/ini.h"

static int cb_ini_vpkg_config(const char *s_, size_t sl_, const char *k_, size_t kl_, const char *v_, size_t vl_, void *user_)
{
    vpkg_config *user = static_cast<vpkg_config *>(user_);
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
                return 1;
            }

            iterator->second.last_modified = (time_t)last_modified;
        } else {
            return 1;
        }
    } else {
        fprintf(stderr, "Not supported!\n");
        return 1;
    }

    return 0;
}

int vpkg_config_parse(vpkg_config *out, const char *str, size_t len)
{
    if (!ini_parse_string(static_cast<const char *>(str), len, cb_ini_vpkg_config, out)) {
        return 1;
    }

    for (auto &pair : *out) {
        if (pair.first.back() == '/') {
            fprintf(stderr, "Repos not implemented yet\n");
            return 1;
        }
    }

    return 0;
}

std::string vpkg_config_path(const char *default_value)
{
    std::string config_home;

    if (default_value) {
        return std::string(default_value);
    }

    // Better: openat
    {
        const char *xdg_config_home = getenv("XDG_CONFIG_HOME");

        if (xdg_config_home != NULL) {
            config_home = std::string(xdg_config_home);
        } else {
            struct passwd *p = getpwuid(getuid());
            if (p == NULL) {
                return "";
            }

            config_home = std::string(p->pw_dir) + "/.config";
        }
    }

    return config_home + "/vpkg.ini";
}
