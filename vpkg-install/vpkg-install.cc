#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/wait.h>

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <pwd.h>

#include <filesystem>
#include <algorithm>
#include <string>
#include <limits>
#include <map>

#include <curl/curl.h>
#include <semaphore.h>
#include <pthread.h>
#include <math.h>
#include <xbps.h>

#include "simdini/ini.h"

#include "vpkg-install/repodata.h"

#include "vpkg/config.hh"
#include "vpkg/util.hh"

#include <atomic>
#include <queue>

static void die(const char *message)
{
    perror(message);
    exit(EXIT_FAILURE);
}

static void usage(int code)
{
    fprintf(stderr, "usage: vpkg [-vfRu] [-c <config_path>]\n");
    exit(code);
}

struct vpkg_check_update_cb_data {
    vpkg::config *config;

    sem_t *sem_data;
    bool force;

    std::vector<::vpkg::config::iterator> *packages_to_update;
};

struct vpkg_status_entry {
    vpkg_status_entry() = delete;

    vpkg_status_entry(const std::string_view &name_, unsigned long offset_,
                      curl_off_t dltotal_, curl_off_t dlnow_,
                      curl_off_t ultotal_, curl_off_t ulnow_) noexcept :
        name{name_},
        offset{offset_},
        dltotal{dltotal_},
        dlnow{dlnow_},
        ultotal{ultotal_},
        ulnow{ulnow_} {}

    std::string_view name;
    unsigned long offset;
    curl_off_t dltotal;
    curl_off_t dlnow;
    curl_off_t ultotal;
    curl_off_t ulnow;
};

struct vpkg_do_update_thread_data {
    std::vector<::vpkg::config::iterator> *packages_to_update;

    struct xbps_handle *xhp;
    vpkg::config *config;

    std::queue<struct vpkg_status_entry> *status;

    std::atomic<bool> *anyerr;
    std::atomic<unsigned long> *next_package;
    std::atomic<unsigned long> *packages_done;

    std::vector<char *> *register_xbps;
    std::vector<char *> *install_xbps;

    sem_t *sem_data;
    sem_t *sem_notify;
    // sem_t *sem_continue;

    sem_t *sem_prod_cons;

    size_t manual_size;
    size_t status_offset;
    // size_t start;
    // size_t size;
    // size_t i;
};

static int vpkg_check_update_cb(struct xbps_handle *xhp, xbps_object_t obj, const char *, void *user_, bool *)
{
    const char *pkgname;
    xbps_dictionary_t xpkg = static_cast<xbps_dictionary_t>(obj);
    vpkg_check_update_cb_data *user = static_cast<vpkg_check_update_cb_data *>(user_);

    assert(xbps_object_type(obj) == XBPS_TYPE_DICTIONARY);

    if (!is_xdeb(xpkg)) {
        return 0;
    }

    if (!xbps_dictionary_get_cstring_nocopy(xpkg, "pkgname", &pkgname)) {
        return 0;
    }

    auto it = user->config->find(pkgname);
    if (it == user->config->end()) {
        return 0;
    }

    if (!user->force && xbps_vpkg_gtver(xpkg, &it->second) != 1) {
        return 0;
    }

    // @todo: Handle EINTR
    sem_wait(user->sem_data);
    user->packages_to_update->push_back(it);
    sem_post(user->sem_data);

    return 0;
}

static int progressfn(void *arg_, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow)
{
    vpkg_do_update_thread_data *arg = static_cast<vpkg_do_update_thread_data *>(arg_);

    sem_wait(arg->sem_data);
    arg->status->emplace(
        arg->packages_to_update->at(arg->status_offset)->first,
        arg->status_offset,
        dltotal,
        dlnow,
        ultotal,
        ulnow
    );

    sem_post(arg->sem_notify);
    sem_post(arg->sem_data);

    return 0;
}

static CURLcode download(const char *url, FILE *file, vpkg_do_update_thread_data *arg)
{
    CURLcode code = CURLE_OK;

    CURL *curl = curl_easy_init();
    if (curl == NULL) {
        return CURLE_FAILED_INIT;
    }

    code = (code == CURLE_OK) ? curl_easy_setopt(curl, CURLOPT_URL, url) : code;
    code = (code == CURLE_OK) ? curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L) : code;
    code = (code == CURLE_OK) ? curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progressfn) : code;
    code = (code == CURLE_OK) ? curl_easy_setopt(curl, CURLOPT_XFERINFODATA, arg) : code;
    code = (code == CURLE_OK) ? curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L) : code;
    // code = (code == CURLE_OK) ? curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L) : code;
    code = (code == CURLE_OK) ? curl_easy_setopt(curl, CURLOPT_USERAGENT, "curl/8.8.0") : code;
    code = (code == CURLE_OK) ? curl_easy_setopt(curl, CURLOPT_WRITEDATA, file) : code;
    code = (code == CURLE_OK) ? curl_easy_perform(curl) : code;
    curl_easy_cleanup(curl);

    return code;
}

static void *vpkg_do_update_thread(void *arg_)
{
    int length;
    vpkg_do_update_thread_data *arg = static_cast<vpkg_do_update_thread_data *>(arg_);

    for (;;) {
        ::vpkg::config::iterator it;
        std::string url;

        sem_wait(arg->sem_prod_cons);

        arg->status_offset = arg->next_package->fetch_add(1);
        if (*arg->packages_done >= arg->packages_to_update->size()) {
            // printf("no more work, im done\n");
            sem_post(arg->sem_prod_cons);
            sem_post(arg->sem_notify);
            return NULL;
        }

        sem_wait(arg->sem_data);
        it = arg->packages_to_update->at(arg->status_offset);
        sem_post(arg->sem_data);

        url = it->second.url;
        length = snprintf(NULL, 0, "/tmp/vpkg/%d/%.*s.deb", gettid(), (int)it->first.size(), it->first.data());
        if (length < 0) {
            *arg->anyerr = true;
            sem_post(arg->sem_notify);
            return NULL;
        }

        char buf[length + 1];

        length = snprintf(buf, sizeof(buf), "/tmp/vpkg/%d/%.*s.deb", gettid(), (int)it->first.size(), it->first.data());
        if (length < 0) {
            *arg->anyerr = true;
            sem_post(arg->sem_notify);
            return NULL;
        }

        char *at = strrchr(buf, '/');
        assert(at);

        *at = '\0';
        if (mkdir(buf, 0644) < 0 && errno != EEXIST) {
            *arg->anyerr = true;
            sem_post(arg->sem_notify);
            return NULL;
        }

        *at = '/';
        FILE *f = fopen(buf, "w");
        if (f == NULL) {
            *arg->anyerr = true;
            sem_post(arg->sem_notify);
            return NULL;
        }

        CURLcode code = download(url.c_str(), f, arg);
        fclose(f);

        // download all packages first
        if (code != CURLE_OK) {
            *arg->anyerr = true;
            sem_post(arg->sem_notify);
            return NULL;
        }

        int stderr_pipefd[2];
        if (pipe(stderr_pipefd) < 0) {
            *arg->anyerr = true;
            sem_post(arg->sem_notify);
            return NULL;
        }

        int stdout_pipefd[2];
        if (pipe(stdout_pipefd) < 0) {
            close(stderr_pipefd[0]);
            close(stderr_pipefd[1]);

            *arg->anyerr = true;
            sem_post(arg->sem_notify);
            return NULL;
        }

        char *deb_package_path;
        length = asprintf(&deb_package_path, "/tmp/vpkg/%d/%.*s.deb", gettid(), (int)it->first.size(), it->first.data());
        if (length < 0) {
            close(stderr_pipefd[0]);
            close(stderr_pipefd[1]);

            *arg->anyerr = true;
            sem_post(arg->sem_notify);
            return NULL;
        }

        std::string version;
        std::string name;
        std::string deps;
        std::string not_deps;

        pid_t pid = fork();
        switch (pid) {
            int status;
            char *at;

        case -1:
            perror("fork");
            break;
        case 0:
            if (dup2(stderr_pipefd[1], STDERR_FILENO) < 0) {
                exit(EXIT_FAILURE);
            }

            if (dup2(stdout_pipefd[1], STDOUT_FILENO) < 0) {
                // @todo message
                exit(EXIT_FAILURE);
            }

            at = strrchr(deb_package_path, '/');
            assert(at);

            *at = '\0';
            if (setenv("XDEB_PKGROOT", deb_package_path, 1) < 0) {
                fprintf(stderr, "failed to set pkgroot\n");
                exit(EXIT_FAILURE);
            }
            *at = '/';

            if (setenv("XDEB_BINPKGS", VPKG_BINPKGS, 1) < 0) {
                fprintf(stderr, "failed to set binpkgs\n");
                exit(EXIT_FAILURE);
            }

            if (close(stdout_pipefd[0]) < 0) {
                // @todo message
                exit(EXIT_FAILURE);
            }

            if (close(stderr_pipefd[0]) < 0) {
                // @todo message
                exit(EXIT_FAILURE);
            }

            not_deps = "--not-deps=";
            not_deps += it->second.not_deps;

            deps = "--deps=";
            deps += it->second.deps;

            name = "--name=";
            name += it->first;

            version = "--version=";
            version += it->second.version;

            execlp("xdeb", "xdeb", "-edR", not_deps.c_str(), deps.c_str(), name.c_str(), version.c_str(), "--", deb_package_path, NULL);
            break;
        default:
            // ignore error, @todo: handle EINTR
            close(stdout_pipefd[1]);
            close(stderr_pipefd[1]);

            if (waitpid(pid, &status, 0) < 0) {
                perror("waitpid");
                sem_post(arg->sem_notify);
                return NULL;
            }

            sem_wait(arg->sem_data);

            char buf[4096];
            ssize_t nr;
            do {
                nr = read(stderr_pipefd[0], buf, sizeof(buf));
                if (nr < 0) {
                    break;
                }

                write(STDERR_FILENO, buf, nr);
            } while (nr == sizeof(buf));

            sem_post(arg->sem_data);

            if (!WIFEXITED(status) || WEXITSTATUS(status) != EXIT_SUCCESS) {
                fprintf(stderr, "Failed to build %.*s\n", (int)it->first.size(), it->first.data());

                *arg->anyerr = true;
                sem_post(arg->sem_notify);
                return NULL;
            }

            FILE *f = fdopen(stdout_pipefd[0], "r");
            // @todo: inval ignored

            char *line = NULL;
            size_t len = 0;

            ssize_t nread = getline(&line, &len, f);
            if (nread == -1) {
                fprintf(stderr, "failed to read output path\n");
                sem_post(arg->sem_notify);
                return NULL;
            }

            if (nread > 0 && line[nread - 1] == '\n') {
                line[--nread] = '\0';
            }

            xbps_dictionary_t binpkgd = xbps_archive_fetch_plist(line, "/props.plist");

            xbps_object_t obj = xbps_dictionary_get(binpkgd, "run_depends");
            if (obj != NULL) {
                xbps_array_t arr = static_cast<xbps_array_t>(obj);
                xbps_object_iterator_t it = xbps_array_iterator(arr);

                while ((obj = xbps_object_iterator_next(it)) != NULL) {
                    xbps_string_t str = static_cast<xbps_string_t>(obj);
                    const char *dep = xbps_string_cstring_nocopy(str);

                    if (dep == NULL) {
                        continue;
                    }

                    size_t dep_len = strlen(dep);
                    char name[dep_len + 1];

                    if (!xbps_pkgpattern_name(name, dep_len, dep)) {
                        continue;
                    }

                    auto it = arg->config->find(name);
                    if (it == arg->config->end()) {
                        continue;
                    }

                    xbps_dictionary_t xpkg = static_cast<xbps_dictionary_t>(xbps_dictionary_get(arg->xhp->pkgdb, name));
                    if (xpkg != NULL && (!is_xdeb(xpkg) || xbps_vpkg_gtver(xpkg, &it->second) != 1)) {
                        continue;
                    }

                    sem_wait(arg->sem_data);

                    if (std::find(arg->packages_to_update->begin(), arg->packages_to_update->end(), it) == arg->packages_to_update->end()) {
                        arg->packages_to_update->push_back(it);
                        sem_post(arg->sem_prod_cons);
                    }

                    sem_post(arg->sem_data);
                }

                xbps_object_iterator_release(it);
                xbps_object_release(binpkgd);
            }

            sem_wait(arg->sem_data);
            arg->register_xbps->push_back(line);

            if (arg->status_offset < arg->manual_size) {
                arg->install_xbps->push_back(line);
            }

            if ((*arg->packages_done += 1) >= arg->packages_to_update->size()) {
                sem_post(arg->sem_prod_cons);
            }

            sem_post(arg->sem_data);

            fclose(f);
            break;
        }
    }

    // Post "done" after full successful download
    sem_post(arg->sem_notify);
    return NULL;
}

static int download_and_install_multi(struct xbps_handle *xhp, vpkg::config *conf, std::vector<::vpkg::config::iterator> *packages_to_update, bool force_install, bool update)
{
    int rv;
    sem_t sem_notify;
    sem_t sem_data;
    sem_t sem_prod_cons;
    std::queue<vpkg_status_entry> status;
    std::vector<char *> install_xbps;
    std::vector<char *> register_xbps;
    std::atomic<bool> anyerr = false;
    std::atomic<unsigned long> packages_done = 0;
    std::atomic<unsigned long> next_package = 0;
    unsigned long maxthreads = sysconf(_SC_NPROCESSORS_ONLN);
    size_t manual_size = packages_to_update->size();

    if (sem_init(&sem_data, 0, 1) < 0) {
        return -1;
    }

    if (sem_init(&sem_notify, 0, 0) < 0) {
        return -1;
    }

    if (sem_init(&sem_prod_cons, 0, manual_size) < 0) {
        return -1;
    }

    if (maxthreads == (unsigned long)-1) {
        // @todo: handle this
        return -1;
    }

    pthread_t threads[maxthreads];
    struct vpkg_do_update_thread_data thread_data[maxthreads];

    unsigned long i;
    for (i = 0; i < maxthreads; i++) {
        thread_data[i].manual_size = manual_size;
        thread_data[i].packages_to_update = packages_to_update;
        thread_data[i].install_xbps = &install_xbps;
        thread_data[i].config = conf;
        thread_data[i].register_xbps = &register_xbps;
        thread_data[i].sem_prod_cons = &sem_prod_cons;
        thread_data[i].sem_notify = &sem_notify;
        thread_data[i].sem_data = &sem_data;
        thread_data[i].status = &status;
        thread_data[i].next_package = &next_package;
        thread_data[i].packages_done = &packages_done;
        thread_data[i].anyerr = &anyerr;
        thread_data[i].xhp = xhp;

        if ((errno = pthread_create(&threads[i], NULL, vpkg_do_update_thread, &thread_data[i]))) {
            // @todo: handle this
            perror("pthread_create");
        }
    }

    unsigned long max_offset = 0;
    unsigned long last_offset = 0;
    for (;;) {
        sem_wait(&sem_notify);

        if (anyerr || packages_done >= packages_to_update->size()) {
            break;
        }

        for (;;) {
            sem_wait(&sem_data);
            if (status.size() == 0) {
                sem_post(&sem_data);
                break;
            }

            vpkg_status_entry temp_entry = status.front();
            status.pop();
            sem_post(&sem_data);

            if (last_offset < temp_entry.offset) {
                for (size_t i = last_offset; i < temp_entry.offset; i++) {
                    fprintf(stderr, "\n");
                }
            }
            if (last_offset > temp_entry.offset) {
                fprintf(stderr, "\033[%ldA", last_offset - temp_entry.offset);
            }

            double progress;
            if (temp_entry.dltotal == 0)
                progress = 0;
            else
                progress = (100.0l * temp_entry.dlnow) / (double)temp_entry.dltotal;

            fprintf(stderr, "\r\033[K%.*s %g %%", (int)temp_entry.name.size(), temp_entry.name.data(), progress);
            fflush(stderr);

            if (max_offset < temp_entry.offset)
                max_offset = temp_entry.offset;

            last_offset = temp_entry.offset;
        }
    }

    printf("\n");

    if (anyerr) {
        for (unsigned long j = 0; j < i; j++) {
            pthread_kill(threads[j], SIGINT);
        }
    }

    for (unsigned long j = 0; j < i; j++) {
        if ((errno = pthread_join(threads[j], NULL))) {
            perror("pthread_join");
        }
    }

    // Adds all packages to repo
    index_add(xhp, register_xbps.data(), register_xbps.size());

    for (auto &pathname : install_xbps) {
        const char *pkgver;

        xbps_dictionary_t binpkgd = xbps_archive_fetch_plist(pathname, "/props.plist");
        free(pathname);

        if (!xbps_dictionary_get_cstring_nocopy(binpkgd, "pkgver", &pkgver)) {
            fprintf(stderr, "pkgver unset\n");
            xbps_object_release(binpkgd);
            continue;
        }

        if (update) {
            rv = xbps_transaction_update_pkg(xhp, pkgver);
        } else {
            rv = xbps_transaction_install_pkg(xhp, pkgver, force_install);
        }

        // @todo: free vector
        switch (rv) {
        case 0:
            xbps_object_release(binpkgd);
            break;
        case EEXIST:
            // ignore already installed "packages may be updated"
            fprintf(stderr, "%s: Already installed\n", pkgver);
            xbps_object_release(binpkgd);
            continue;
        case ENOENT:
            fprintf(stderr, "%s: Not found in repository pool\n", pkgver);
            xbps_object_release(binpkgd);
            return -1;
        default:
            fprintf(stderr, "%s: Unexpected error: %d\n", pkgver, rv);
            xbps_object_release(binpkgd);
            return -1;
        }
    }

    rv = xbps_transaction_prepare(xhp);
    switch (rv) {
    case 0:
        break;
    case ENODEV:
        fprintf(stderr, "Missing dependencies.\n");
        return -1;
    default:
        fprintf(stderr, "transaction_prepare: unexpected error: %d\n", rv);
        return -1;
    }

    if (xhp->transd == NULL) {
        fprintf(stderr, "Nothing to do.\n");
        return 0;
    }

    xbps_object_t obj;
    xbps_object_iterator_t it = xbps_array_iter_from_dict(xhp->transd, "packages");

    printf("Summary of changes:\n");
    while ((obj = xbps_object_iterator_next(it)) != NULL) {
        xbps_dictionary_t dict = static_cast<xbps_dictionary_t>(obj);
        const char *pkgname;
        const char *pkgver;

        assert(xbps_dictionary_get_cstring_nocopy(dict, "pkgname", &pkgname));
        assert(xbps_dictionary_get_cstring_nocopy(dict, "pkgver", &pkgver));

        pkgver = xbps_pkg_version(pkgver);

        printf("%s -> %s\n", pkgname, pkgver);
    }

    xbps_object_iterator_release(it);

    if (!yes_no_prompt()) {
        fprintf(stderr, "Aborting!\n");
        return -1;
    }

    rv = xbps_transaction_commit(xhp);
    switch (rv) {
    case 0:
        break;
    default:
        fprintf(stderr, "Transaction failed: %d\n", rv);
        return -1;
    }

    return 0;
}

int main(int argc, char **argv)
{
    bool force = false;
    bool update = false;
    bool verbose = false;

    const char *config_path = nullptr;
    vpkg::config config;
    xbps_handle xh;
    std::error_code ec;
    std::vector<::vpkg::config::iterator> to_install;

    void *data = nullptr;
    int rv = EXIT_FAILURE;
    struct stat st;
    int config_fd;
    int opt;

    memset(&xh, 0, sizeof(xh));
    curl_global_init(CURL_GLOBAL_ALL);

    while ((opt = getopt(argc, argv, ":c:vfu")) != -1) {
        switch (opt) {
        case 'u':
            update = true;
            break;
        case 'c':
            config_path = optarg;
            break;
        case 'v':
            verbose = true;
            break;
        case 'f':
            force = true;
            break;
        default:
            usage(EXIT_FAILURE);
        }
    }

    argc -= optind, argv += optind;

    {
        std::string resolved_config_path;

        resolved_config_path = vpkg::config_path(config_path);
        if (resolved_config_path.size() == 0) {
            die("unable to canonicalize config path");
        }

        config_fd = open(resolved_config_path.c_str(), O_RDONLY);
    }

    if (config_fd < 0) {
        die("unable to open config file");
    }

    if (fstat(config_fd, &st) < 0) {
        die("unable to stat config file");
    }

    if (st.st_size != 0) {
        data = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, config_fd, 0);
        if (data == MAP_FAILED) {
            die("unable to map config file");
        }

        if (vpkg::parse_config(&config, static_cast<const char *>(data), st.st_size) != 0) {
            fprintf(stderr, "unable to parse config file\n");
            goto end_munmap;
        }
    }

    if ((errno = xbps_init(&xh)) != 0) {
        perror("xbps_init");
        goto out;
    }

    if (setenv("XDEB_SHLIBS", VPKG_XDEB_SHLIBS, 1) != 0) {
        perror("failed to set shlibs env");
        goto end_xbps;
    }

    if (mkdir(VPKG_TEMPDIR, 0644) < 0 && errno != EEXIST) {
        perror("failed to create tempdir");
        goto end_xbps;
    }

    if ((errno = xbps_pkgdb_lock(&xh)) != 0) {
        perror("failed to lock pkgdb");
        goto end_xbps;
    }

    to_install.reserve(argc);

    for (int i = 0; i < argc; i++) {
        auto it = config.find(argv[i]);
        if (it == config.end()) {
            fprintf(stderr, "package %s not found\n", argv[i]);
            continue;
        }

        if (!force) {
            auto xpkg = static_cast<xbps_dictionary_t>(xbps_dictionary_get(xh.pkgdb, argv[i]));
            if (xpkg != NULL && (!is_xdeb(xpkg) || xbps_vpkg_gtver(xpkg, &it->second) != 1)) {
                continue;
            }
        }

        to_install.push_back(it);
    }

    if (update && argc == 0) {
        sem_t sem_data;
        if ((errno = sem_init(&sem_data, 0, 1)) != 0) {
            perror("sem_init");
            goto end_xbps_lock;
        }

        vpkg_check_update_cb_data cbd;
        cbd.config = &config;
        cbd.force = force;
        cbd.sem_data = &sem_data;
        cbd.packages_to_update = &to_install;

        xbps_pkgdb_foreach_cb_multi(&xh, vpkg_check_update_cb, &cbd);
    } else if (argc == 0) {
        fprintf(stderr, "usage: vpkg-install <package...>\n");
        goto end_xbps_lock;
    }

    /*
    if ((errno = add_full_deptree(&xh, &config, &to_install)) != 0) {
        perror("Unable to resolve dependencies\n");
        goto end_xbps_lock;
    }
    */

    if (to_install.size() == 0) {
        fprintf(stderr, "Nothing to do.\n");
        goto end_xbps_lock;
    }

    if (::download_and_install_multi(&xh, &config, &to_install, force, update) != 0) {
        ;
    }

    if (std::filesystem::remove_all(VPKG_TEMPDIR, ec) == static_cast<std::uintmax_t>(-1)) {
        fprintf(stderr, "failed to cleanup tempdir\n");
    }

    rv = EXIT_SUCCESS;

end_xbps_lock:
end_xbps:
    xbps_end(&xh);

end_munmap:
    if (data != NULL) {
        munmap(data, st.st_size);
    }

    close(config_fd);

out:
    return rv;
}
