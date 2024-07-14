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

struct vpkg_progress {
    enum {
        INIT,
        CURL,
        XDEB,
        DONE,
        ERROR,
    } state;

    std::string_view name;
    size_t current_offset;
    int tid_local;

    union {
        struct {
            curl_off_t dltotal;
            curl_off_t dlnow;
            curl_off_t ultotal;
            curl_off_t ulnow;
        };

        char *error_message;
    };
};

struct vpkg_do_update_thread_data {
    std::vector<::vpkg::config::iterator> *packages_to_update;
    std::queue<::vpkg_progress> *progress;

    struct xbps_handle *xhp;
    vpkg::config *config;

    std::atomic<unsigned long> *next_package;
    std::atomic<unsigned long> *packages_done;

    std::vector<char *> *register_xbps;
    std::vector<char *> *install_xbps;

    sem_t *sem_data;
    sem_t *sem_notify;
    // sem_t *sem_continue;

    sem_t *sem_prod_cons;

    size_t current_offset;
    size_t manual_size;
    int tid_local;

    ::vpkg::config::iterator current;
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

static void push_to_progress_queue(struct vpkg_do_update_thread_data *self, struct vpkg_progress prog)
{
    prog.name = self->current->first;
    prog.current_offset = self->current_offset;
    prog.tid_local = self->tid_local;

    sem_wait(self->sem_data);
    self->progress->push(prog);
    sem_post(self->sem_data);
    sem_post(self->sem_notify);
}

static int progressfn(void *arg, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow)
{
    push_to_progress_queue(static_cast<struct vpkg_do_update_thread_data *>(arg), (struct vpkg_progress){
        .state = vpkg_progress::CURL,
        .dltotal = dltotal,
        .dlnow = dlnow,
        .ultotal = ultotal,
        .ulnow = ulnow
    });
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

static void *post_thread_error(struct vpkg_do_update_thread_data *self, const char *fmt, ...)
{
    va_list va;
    va_start(va, fmt);

    char *error_message;
    if (vasprintf(&error_message, fmt, va) < 0) {
        va_end(va);
        push_to_progress_queue(self, (struct vpkg_progress){.state = vpkg_progress::ERROR, .error_message = NULL});
    }

    va_end(va);
    push_to_progress_queue(self, (struct vpkg_progress){.state = vpkg_progress::ERROR, .error_message = error_message});
    return NULL;
}

static void *vpkg_do_update_thread(void *arg_)
{
    int length;
    vpkg_do_update_thread_data *arg = static_cast<vpkg_do_update_thread_data *>(arg_);

    for (;;) {
        std::string url;

        sem_wait(arg->sem_prod_cons);
        sem_wait(arg->sem_data);

        arg->current_offset = arg->next_package->fetch_add(1);
        if (*arg->packages_done >= arg->packages_to_update->size()) {
            sem_post(arg->sem_data);

            sem_post(arg->sem_prod_cons);
            sem_post(arg->sem_notify);
            return NULL;
        }

        arg->current = arg->packages_to_update->at(arg->current_offset);
        sem_post(arg->sem_data);

        push_to_progress_queue(arg, (struct vpkg_progress){ .state = vpkg_progress::INIT });

        url = arg->current->second.url;
        length = snprintf(NULL, 0, "%s/%d/%.*s.deb", VPKG_TEMPDIR, gettid(), (int)arg->current->first.size(), arg->current->first.data());
        if (length < 0) {
            return post_thread_error(arg, "failed to format pathname: %s", strerror(errno));
        }

        char deb_package_path[length + 1];

        length = snprintf(deb_package_path, sizeof(deb_package_path), "%s/%d/%.*s.deb", VPKG_TEMPDIR, gettid(), (int)arg->current->first.size(), arg->current->first.data());
        if (length < 0) {
            return post_thread_error(arg, "failed to format pathname: %s", strerror(errno));
        }

        char *at = strrchr(deb_package_path, '/');
        assert(at);

        *at = '\0';
        if (mkdir(deb_package_path, 0644) < 0 && errno != EEXIST) {
            return post_thread_error(arg, "failed to create destination file: %s", strerror(errno));
        }

        *at = '/';
        FILE *f = fopen(deb_package_path, "w");
        if (f == NULL) {
            return post_thread_error(arg, "failed to open destination file: %s", strerror(errno));
        }

        CURLcode code = download(url.c_str(), f, arg);
        fclose(f);

        // download all packages first
        if (code != CURLE_OK) {
            return post_thread_error(arg, "failed to download package: %s", curl_easy_strerror(code));
        }

        push_to_progress_queue(arg, (struct vpkg_progress){ .state = vpkg_progress::XDEB });

        int stderr_pipefd[2];
        if (pipe(stderr_pipefd) < 0) {
            return post_thread_error(arg, "failed to create stderr pipe: %s", strerror(errno));
        }

        int stdout_pipefd[2];
        if (pipe(stdout_pipefd) < 0) {
            close(stderr_pipefd[0]);
            close(stderr_pipefd[1]);

            return post_thread_error(arg, "failed to create stdout pipe: %s", strerror(errno));
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

            // @todo: handle
            close(stdout_pipefd[0]);
            close(stderr_pipefd[0]);

            not_deps = "--not-deps=";
            not_deps += arg->current->second.not_deps;

            deps = "--deps=";
            deps += arg->current->second.deps;

            name = "--name=";
            name += arg->current->first;

            version = "--version=";
            version += arg->current->second.version;

            execlp("xdeb", "xdeb", "-edR", not_deps.c_str(), deps.c_str(), name.c_str(), version.c_str(), "--", deb_package_path, NULL);
            break;
        default:
            // ignore error, @todo: handle EINTR
            close(stdout_pipefd[1]);
            close(stderr_pipefd[1]);

            if (waitpid(pid, &status, 0) < 0) {
                return post_thread_error(arg, "failed to wait for child to complete: %s", strerror(errno));
            }

            if (!WIFEXITED(status) || WEXITSTATUS(status) != EXIT_SUCCESS) {
                char *buf = NULL;
                size_t bufsz = 0;

                for (;;) {
                    char *new_buf = (char *)realloc(buf, bufsz + BUFSIZ + 1);
                    if (new_buf == NULL) {
                        post_thread_error(arg, "xdeb failed with %d (unable to format output: %s)", WEXITSTATUS(status), strerror(errno));
                        free(buf);
                        return NULL;
                    }

                    buf = new_buf;

                    ssize_t nr = read(stderr_pipefd[0], buf, BUFSIZ);
                    if (nr < 0) {
                        post_thread_error(arg, "xdeb failed with %d (unable to format output: %s)", WEXITSTATUS(status), strerror(errno));
                        free(buf);
                        return NULL;
                    }

                    bufsz += nr;
                    buf[bufsz] = '\0';

                    if (nr != BUFSIZ) {
                        break;
                    }
                }

                while (bufsz && buf[bufsz - 1] == '\n')
                    buf[--bufsz] = '\0';

                post_thread_error(arg, "xdeb failed with %d:\n%s", WEXITSTATUS(status), buf);
                free(buf);
                return NULL;
            }

            FILE *f = fdopen(stdout_pipefd[0], "r");
            // @todo: inval ignored

            char *line = NULL;
            size_t len = 0;

            ssize_t nread = getline(&line, &len, f);
            if (nread == -1) {
                return post_thread_error(arg, "failed to parse xdeb output: %s", strerror(errno));
            }

            if (nread > 0 && line[nread - 1] == '\n') {
                line[--nread] = '\0';
            }

            fclose(f);

            push_to_progress_queue(arg, (struct vpkg_progress){ .state = vpkg_progress::DONE });

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

            if (arg->current_offset < arg->manual_size) {
                arg->install_xbps->push_back(line);
            }

            if ((*arg->packages_done += 1) >= arg->packages_to_update->size()) {
                sem_post(arg->sem_prod_cons);
            }

            sem_post(arg->sem_data);
            break;
        }
    }

    // Post "done" after full successful download
    sem_post(arg->sem_notify);
    return NULL;
}

static int print_bar(struct vpkg_progress *prog)
{
    printf("\033[K");
    switch (prog->state) {
    case vpkg_progress::INIT:
        printf("%.*s initializing\n", (int)prog->name.size(), prog->name.data());
        break;
    case vpkg_progress::CURL:
        printf("%.*s downloading %ld/%ld\n", (int)prog->name.size(), prog->name.data(), prog->dlnow, prog->dltotal);
        break;
    case vpkg_progress::XDEB:
        printf("%.*s xdeb\n", (int)prog->name.size(), prog->name.data());
        break;
    case vpkg_progress::DONE:
        printf("%.*s finished\n", (int)prog->name.size(), prog->name.data());
        break;
    case vpkg_progress::ERROR:
        if (prog->error_message) {
            printf("%.*s \033[31;1merror\033[0m %s\n", (int)prog->name.size(), prog->name.data(), prog->error_message);
        } else {
            printf("%.*s \033[31;1merror\033[0m invalid error\n", (int)prog->name.size(), prog->name.data());
        }
        break;
    default:
        printf("\n");
        break;
    }

    return prog->state;
}

static int download_and_install_multi(struct xbps_handle *xhp, vpkg::config *conf, std::vector<::vpkg::config::iterator> *packages_to_update, bool force_install, bool update)
{
    int rv;
    sem_t sem_data;
    sem_t sem_notify;
    sem_t sem_prod_cons;
    std::vector<char *> install_xbps;
    std::vector<char *> register_xbps;
    std::queue<::vpkg_progress> progress;
    std::atomic<unsigned long> packages_done = 0;
    std::atomic<unsigned long> next_package = 0;
    unsigned long maxthreads;
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

    maxthreads = sysconf(_SC_NPROCESSORS_ONLN);
    if (maxthreads == (unsigned long)-1) {
        fprintf(stderr, "failed to get core count: %s, executing using two threads.\n", strerror(errno));
        maxthreads = 1;
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
        thread_data[i].next_package = &next_package;
        thread_data[i].packages_done = &packages_done;
        thread_data[i].progress = &progress;
        thread_data[i].xhp = xhp;
        thread_data[i].tid_local = i;

        if ((errno = pthread_create(&threads[i], NULL, vpkg_do_update_thread, &thread_data[i]))) {
            // @todo: handle this
            perror("pthread_create");
        }
    }

    struct vpkg_progress progress_display[i];
    int tid_to_offset[i];

    size_t base_offset = 0;
    size_t nrunning = 0;

    bool anyerr = false;
    for (;;) {
        sem_wait(&sem_notify);

        if (nrunning) {
            printf("\033[%zuA", nrunning);
        }

        for (;;) {
            sem_trywait(&sem_notify);

            sem_wait(&sem_data);
            if (progress.size() == 0) {
                sem_post(&sem_data);
                break;
            }

            ::vpkg_progress prog = progress.front();
            progress.pop();
            sem_post(&sem_data);

            switch (prog.state) {
            case vpkg_progress::DONE: {
                assert(nrunning > 0);

                int tid0 = progress_display[base_offset % i].tid_local;
                int tid1 = prog.tid_local;

                progress_display[tid_to_offset[tid1]] = progress_display[base_offset % i];
                progress_display[base_offset % i] = prog;

                tid_to_offset[tid0] = tid_to_offset[tid1];
                tid_to_offset[tid1] = base_offset % i;

                print_bar(&prog);
                base_offset++, nrunning--;
                break;
            }
            case vpkg_progress::INIT: {
                assert(nrunning < i);

                tid_to_offset[prog.tid_local] = (base_offset + nrunning) % i;
                progress_display[tid_to_offset[prog.tid_local]] = prog;
                nrunning++;
                break;
            }
            case vpkg_progress::ERROR: {
                assert(nrunning > 0);

                progress_display[tid_to_offset[prog.tid_local]] = progress_display[(base_offset + nrunning - 1) % i];
                progress_display[(base_offset + nrunning - 1) % i] = prog;
                break;
            }
            default: {
                progress_display[tid_to_offset[prog.tid_local]] = prog;
                break;
            }
            }
        }

        for (size_t j = base_offset; j < base_offset + nrunning; j++) {
            if (print_bar(&progress_display[j % i]) == vpkg_progress::ERROR) {
                if (progress_display[j % i].error_message) {
                    free(progress_display[j % i].error_message);
                }

                anyerr = true;
            }
        }

        if (anyerr || packages_done >= packages_to_update->size()) {
            break;
        }
    }

    if (anyerr) {
        for (unsigned long j = 0; j < i; j++) {
            if ((errno = pthread_kill(threads[j], SIGTERM))) {
                perror("pthread_kill");
            }
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

    const char *config_path = VPKG_CONFIG_PATH;
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

    config_fd = open(config_path, O_RDONLY);
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
