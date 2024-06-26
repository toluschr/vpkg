#ifndef VPKG_PACKAGE_HH_
#define VPKG_PACKAGE_HH_

namespace vpkg {
struct package {
    std::string url;
    std::string name;
    std::string deps;
    std::string base_url;
    std::string not_deps;
    std::string filename;

    int resolve_url(time_t *out);
};
}

#endif // VPKG_PACKAGE_HH_
