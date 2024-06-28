#ifndef VPKG_PACKAGE_HH_
#define VPKG_PACKAGE_HH_

namespace vpkg {
struct package {
    std::string_view url;
    std::string_view name;
    std::string_view deps;
    std::string_view version;
    std::string_view not_deps;
    time_t last_modified;
};
}

#endif // VPKG_PACKAGE_HH_
