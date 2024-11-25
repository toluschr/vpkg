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
#include "tqueue/tqueue.h"

#include "vpkg-install/repodata.h"

#include "vpkg/config.hh"
#include "vpkg/util.hh"

#include <atomic>
#include <vector>

static void usage(int code)
{
    fprintf(stderr, "usage: vpkg-install [-vfRuS] [-c <config_path>]\n");
    exit(code);
}

static char *read_all_null_no_tr_nl(int fd, size_t *length)
{
    char *buf = NULL;
    size_t bufsz = 0;

    for (;;) {
        char *new_buf = (char *)realloc(buf, bufsz + BUFSIZ + 1);
        if (new_buf == NULL) {
            free_preserve_errno(buf);
            return NULL;
        }

        buf = new_buf;

        ssize_t nr = read(fd, buf + bufsz, BUFSIZ);
        if (nr < 0) {
            free_preserve_errno(buf);
            return NULL;
        }

        bufsz += nr;
        buf[bufsz] = '\0';

        if (nr != BUFSIZ) {
            break;
        }
    }

    while (bufsz && buf[bufsz - 1] == '\n') {
        buf[--bufsz] = '\0';
    }

    if (length) {
        *length = bufsz;
    }

    return buf;
}

struct vpkg_progress {
    enum state {
        INIT,
        CURL,
        XDEB,
        DONE,
        ERROR,
    } state;

    std::string_view name;
    size_t current_offset;
    int tid_local;

    struct {
        curl_off_t dltotal;
        curl_off_t dlnow;
        curl_off_t ultotal;
        curl_off_t ulnow;
    };

    char *error_message;
};

struct vpkg_check_update_cb_data {
    vpkg::packages *packages;

    sem_t *sem_data;
    bool force;

    std::vector<::vpkg::packages::iterator> *packages_to_update;
};

struct vpkg_do_update_thread_shared_data {
    std::vector<::vpkg::packages::iterator> *packages_to_update;
    struct tqueue progress_queue;
    struct xbps_handle *xhp;
    struct xbps_repo *repo;

    vpkg::packages *packages;
    std::atomic<unsigned long> next_package;
    std::atomic<unsigned long> packages_done;

    std::vector<xbps_dictionary_t> install_xbps;
    bool force;

    size_t manual_size;

    sem_t sem_data;
    sem_t sem_prod_cons;
    sem_t sem_progress_limit;

    xbps_dictionary_t idx, idxmeta, idxstage;
};

struct vpkg_do_update_thread_data {
    struct vpkg_do_update_thread_shared_data *shared;

    size_t current_offset;
    int tid_local;

    ::vpkg::packages::iterator current;
};

static int vpkg_check_update_cb(struct xbps_handle *xhp, xbps_object_t obj, const char *pkgname, void *user_, bool *)
{
    xbps_dictionary_t xpkg = static_cast<xbps_dictionary_t>(obj);
    vpkg_check_update_cb_data *user = static_cast<vpkg_check_update_cb_data *>(user_);

    assert(xbps_object_type(obj) == XBPS_TYPE_DICTIONARY);

    if (!is_xdeb(xpkg)) {
        return 0;
    }

    auto it = user->packages->find(pkgname);
    if (it == user->packages->end()) {
        return 0;
    }

    if (!user->force && xbps_vpkg_gtver(xpkg, &it->second) != 1) {
        return 0;
    }

    RETRY_EINTR(sem_wait(user->sem_data));
    user->packages_to_update->push_back(it);
    ASSERT_NOERR(sem_post(user->sem_data));

    return 0;
}

static int post_state(struct vpkg_do_update_thread_data *self, enum vpkg_progress::state state)
{
    auto node = (struct tqueue_node *)malloc(tqueue_sizeof(struct vpkg_progress));
    if (node == NULL) {
        return -1;
    }

    struct vpkg_progress *data = (struct vpkg_progress *)node->data;
    data->state = state;
    data->name = self->current->first;
    data->current_offset = self->current_offset;
    data->tid_local = self->tid_local;

    RETRY_EINTR(sem_wait(&self->shared->sem_progress_limit));
    RETRY_EINTR(tqueue_put_node(&self->shared->progress_queue, node));
    return 0;
}

static void *post_error(struct vpkg_do_update_thread_data *self, const char *fmt, ...)
{
    va_list va;

    auto node = (struct tqueue_node *)malloc(tqueue_sizeof(struct vpkg_progress));
    if (node == NULL) {
        perror_exit("ran out of memory during error handling");
        return NULL;
    }

    struct vpkg_progress *data = (struct vpkg_progress *)node->data;
    data->state = vpkg_progress::ERROR;
    data->name = self->current->first;
    data->current_offset = self->current_offset;
    data->tid_local = self->tid_local;

    va_start(va, fmt);
    if (vasprintf(&data->error_message, fmt, va) < 0) {
        free_preserve_errno(node);
        perror_exit("ran out of memory during error handling");
        return NULL;
    }
    va_end(va);

    RETRY_EINTR(sem_wait(&self->shared->sem_progress_limit));
    RETRY_EINTR(tqueue_put_node(&self->shared->progress_queue, node));
    return NULL;
}

static int progressfn(void *self_, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow)
{
    struct vpkg_do_update_thread_data *self = static_cast<struct vpkg_do_update_thread_data *>(self_);

    auto node = (struct tqueue_node *)malloc(tqueue_sizeof(struct vpkg_progress));
    if (node == NULL) {
        return -1;
    }

    struct vpkg_progress *data = (struct vpkg_progress *)node->data;
    data->name = self->current->first;
    data->current_offset = self->current_offset;
    data->tid_local = self->tid_local;

    data->state = vpkg_progress::CURL;
    data->dltotal = dltotal;
    data->dlnow = dlnow;
    data->ultotal = ultotal;
    data->ulnow = ulnow;

    RETRY_EINTR(sem_wait(&self->shared->sem_progress_limit));
    RETRY_EINTR(tqueue_put_node(&self->shared->progress_queue, node));
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
    code = (code == CURLE_OK) ? curl_easy_setopt(curl, CURLOPT_USERAGENT, "curl/8.8.0") : code;
    code = (code == CURLE_OK) ? curl_easy_setopt(curl, CURLOPT_WRITEDATA, file) : code;
    code = (code == CURLE_OK) ? curl_easy_perform(curl) : code;
    curl_easy_cleanup(curl);

    return code;
}

static void *vpkg_do_update_thread(void *arg_)
{
    vpkg_do_update_thread_data *arg = static_cast<vpkg_do_update_thread_data *>(arg_);

    for (;;) {
        xbps_dictionary_t binpkgd;

        RETRY_EINTR(sem_wait(&arg->shared->sem_prod_cons));
        RETRY_EINTR(sem_wait(&arg->shared->sem_data));

        arg->current_offset = arg->shared->next_package.fetch_add(1);
        if (arg->shared->packages_done >= arg->shared->packages_to_update->size()) {
            ASSERT_NOERR(sem_post(&arg->shared->sem_data));
            ASSERT_NOERR(sem_post(&arg->shared->sem_prod_cons));
            return NULL;
        }

        arg->current = arg->shared->packages_to_update->at(arg->current_offset);
        ASSERT_NOERR(sem_post(&arg->shared->sem_data));

        // vpkg_progress::ERROR must always come after vpkg_progress::INIT
        post_state(arg, vpkg_progress::INIT);

        char pkgname[arg->current->first.size() + 1];
        memcpy(pkgname, arg->current->first.data(), arg->current->first.size());
        pkgname[arg->current->first.size()] = '\0';

        binpkgd = xbps_repo_get_pkg(arg->shared->repo, pkgname);

        // If the package was found and a newer version is available, re-download.
        if (binpkgd != NULL && xbps_vpkg_gtver(binpkgd, &arg->current->second) != 0) {
            binpkgd = NULL;
        }

        if (binpkgd) {
            xbps_object_retain(binpkgd);
        } else {
            char *deb_package_path;
            char *at;
            CURLcode code;

            if (asprintf(&deb_package_path, "%s/%d/%.*s.deb", VPKG_TEMPDIR, gettid(), (int)arg->current->first.size(), arg->current->first.data()) < 0) {
                return post_error(arg, "failed to format pathname: %s", strerror(ENOMEM));
            }

            at = strrchr(deb_package_path, '/');

            *at = '\0';
            if (mkdir(deb_package_path, 0644) < 0 && errno != EEXIST) {
                free_preserve_errno(deb_package_path);
                return post_error(arg, "failed to create pkgroot: %s", strerror(errno));
            }
            *at = '/';

            {
                char *url;
                if (asprintf(&url, "%.*s", (int)arg->current->second.url.size(), arg->current->second.url.data()) < 0) {
                    free_preserve_errno(deb_package_path);
                    return post_error(arg, "failed to format url: %s", strerror(ENOMEM));
                }

                FILE *f = fopen(deb_package_path, "w");
                if (f == NULL) {
                    free_preserve_errno(deb_package_path);
                    free_preserve_errno(url);
                    return post_error(arg, "failed to open destination file: %s", strerror(errno));
                }

                code = download(url, f, arg);

                fclose(f);
                free(url);
            }

            // download all packages first
            if (code != CURLE_OK) {
                free_preserve_errno(deb_package_path);
                return post_error(arg, "failed to download package: %s", curl_easy_strerror(code));
            }

            post_state(arg, vpkg_progress::XDEB);

            int stderr_pipefd[2];
            if (pipe(stderr_pipefd) < 0) {
                free_preserve_errno(deb_package_path);
                return post_error(arg, "failed to create stderr pipe: %s", strerror(errno));
            }

            int stdout_pipefd[2];
            if (pipe(stdout_pipefd) < 0) {
                close(stderr_pipefd[0]);
                close(stderr_pipefd[1]);

                free_preserve_errno(deb_package_path);
                return post_error(arg, "failed to create stdout pipe: %s", strerror(errno));
            }

            pid_t pid = fork();
            switch (pid) {
                int status;

            case -1:
                close(stdout_pipefd[0]);
                close(stderr_pipefd[0]);
                close(stdout_pipefd[1]);
                close(stderr_pipefd[1]);
                free_preserve_errno(deb_package_path);
                return post_error(arg, "failed to fork: %s", strerror(errno));
            case 0: {
                char *not_deps, *version, *name, *deps, *replaces, *provides;

                close(stdout_pipefd[0]);
                close(stderr_pipefd[0]);

                if (dup2(stderr_pipefd[1], STDERR_FILENO) < 0) {
                    fprintf(stderr, "failed to pipe xdeb errors to vpkg\n");
                    exit(EXIT_FAILURE);
                }

                if (dup2(stdout_pipefd[1], STDOUT_FILENO) < 0) {
                    fprintf(stderr, "failed to pipe xdeb output to vpkg\n");
                    exit(EXIT_FAILURE);
                }

                *at = '\0';
                if (setenv("XDEB_PKGROOT", deb_package_path, 1) < 0) {
                    fprintf(stderr, "failed to set environment variable XDEB_PKGROOT\n");
                    exit(EXIT_FAILURE);
                }
                *at = '/';

                if (setenv("XDEB_BINPKGS", VPKG_BINPKGS, 1) < 0) {
                    fprintf(stderr, "failed to set environment variable XDEB_BINPKGS\n");
                    exit(EXIT_FAILURE);
                }

                if (asprintf(&not_deps, "--not-deps=%.*s", (int)arg->current->second.not_deps.size(), arg->current->second.not_deps.data()) < 0 ||
                    asprintf(&deps, "--deps=%.*s", (int)arg->current->second.deps.size(), arg->current->second.deps.data()) < 0 ||
                    asprintf(&replaces, "--replaces=%.*s", (int)arg->current->second.replaces.size(), arg->current->second.replaces.data()) < 0 ||
                    asprintf(&provides, "--provides=%.*s", (int)arg->current->second.provides.size(), arg->current->second.provides.data()) < 0 ||
                    asprintf(&name, "--name=%.*s", (int)arg->current->first.size(), arg->current->first.data()) < 0 ||
                    asprintf(&version, "--version=%.*s", (int)arg->current->second.version.size(), arg->current->second.version.data()) < 0) {
                    // no free in child process
                    fprintf(stderr, "failed to format xdeb arguments\n");
                    exit(EXIT_FAILURE);
                }

                execlp("xdeb", "xdeb", "-edRL", not_deps, deps, name, version, replaces, provides, "--", deb_package_path, NULL);
                fprintf(stderr, "failed to execute xdeb binary\n");
                exit(EXIT_FAILURE);
                break;
            }

            default: {
                free(deb_package_path);

                size_t len;
                char *buf;

                // ignore error, @todo: handle EINTR
                close(stdout_pipefd[1]);
                close(stderr_pipefd[1]);

                if (waitpid(pid, &status, 0) < 0) {
                    return post_error(arg, "failed to wait for child to complete: %s", strerror(errno));
                }

                bool failed = !WIFEXITED(status) || WEXITSTATUS(status) != EXIT_SUCCESS;
                buf = read_all_null_no_tr_nl(failed ? stderr_pipefd[0] : stdout_pipefd[0], &len);

                close(stdout_pipefd[0]);
                close(stderr_pipefd[0]);

                if (failed) {
                    if (buf == NULL) {
                        return post_error(arg, "xdeb failed with %d (unable to read output: %s)", WEXITSTATUS(status), strerror(errno));
                    }

                    post_error(arg, "xdeb failed with %d:\n%s", WEXITSTATUS(status), buf);
                } else {
                    if (buf == NULL) {
                        return post_error(arg, "failed to parse xdeb output: %s", strerror(errno));
                    }

                    RETRY_EINTR(sem_wait(&arg->shared->sem_data));
                    binpkgd = repodata_add(arg->shared->xhp, buf, arg->shared->idx, arg->shared->idxmeta, arg->shared->idxstage);
                    ASSERT_NOERR(sem_post(&arg->shared->sem_data));
                }

                free(buf);
            }

            }
        }

        if (binpkgd == NULL) {
            return post_error(arg, "failed to register binpkg");
        }

        post_state(arg, vpkg_progress::DONE);

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

                auto it = arg->shared->packages->find(name);
                if (it == arg->shared->packages->end()) {
                    continue;
                }

                // Don't install packages that are provided by xbps
                xbps_dictionary_t xpkg = static_cast<xbps_dictionary_t>(xbps_dictionary_get(arg->shared->xhp->pkgdb, name));
                if (xpkg != NULL && (!is_xdeb(xpkg) || xbps_vpkg_gtver(xpkg, &it->second) != 1)) {
                    continue;
                }

                RETRY_EINTR(sem_wait(&arg->shared->sem_data));

                if (std::find(arg->shared->packages_to_update->begin(), arg->shared->packages_to_update->end(), it) == arg->shared->packages_to_update->end()) {
                    arg->shared->packages_to_update->push_back(it);
                    ASSERT_NOERR(sem_post(&arg->shared->sem_prod_cons));
                }

                ASSERT_NOERR(sem_post(&arg->shared->sem_data));
            }

            xbps_object_iterator_release(it);
        }

        RETRY_EINTR(sem_wait(&arg->shared->sem_data));

        if (arg->current_offset < arg->shared->manual_size) {
            arg->shared->install_xbps.push_back(binpkgd);
        }

        if ((arg->shared->packages_done += 1) >= arg->shared->packages_to_update->size()) {
            RETRY_EINTR(sem_wait(&arg->shared->sem_progress_limit));
            RETRY_EINTR(tqueue_put_node(&arg->shared->progress_queue, NULL));
            ASSERT_NOERR(sem_post(&arg->shared->sem_prod_cons));
        }

        ASSERT_NOERR(sem_post(&arg->shared->sem_data));
    }

    return NULL;
}

static int print_bar(struct vpkg_progress *prog)
{
    printf("\033[K");
    switch (prog->state) {
    case vpkg_progress::INIT:
        printf("%.*s init\n", (int)prog->name.size(), prog->name.data());
        break;
    case vpkg_progress::CURL:
        printf("%.*s curl (%ld/%ld)\n", (int)prog->name.size(), prog->name.data(), prog->dlnow, prog->dltotal);
        break;
    case vpkg_progress::XDEB:
        printf("%.*s xdeb\n", (int)prog->name.size(), prog->name.data());
        break;
    case vpkg_progress::DONE:
        printf("%.*s done\n", (int)prog->name.size(), prog->name.data());
        break;
    case vpkg_progress::ERROR:
        printf("%.*s \033[31;1merror\033[0m %s\n", (int)prog->name.size(), prog->name.data(), prog->error_message);
        break;
    default:
        printf("\n");
        break;
    }

    return prog->state;
}

static int state_cb(const struct xbps_state_cb_data *xscb, void *user_)
{
    if (xscb->err) {
        fprintf(stderr, "%s\n", xscb->desc);
    }

    return 0;
}

static int download_and_install_multi(struct xbps_handle *xhp, vpkg::packages *packages, std::vector<::vpkg::packages::iterator> *packages_to_update, bool force_install, bool update, bool install)
{
    int rv = 0;
    int npackagesmodified = 0;
    unsigned long maxthreads;

    struct vpkg_do_update_thread_shared_data shared;

    xbps_repo_store(xhp, VPKG_BINPKGS);

    shared.packages_to_update = packages_to_update;
    shared.manual_size = shared.packages_to_update->size();
    shared.packages_done = 0;
    shared.next_package = 0;
    shared.packages = packages;
    shared.xhp = xhp;
    shared.repo = xbps_repo_open(xhp, VPKG_BINPKGS);
    shared.force = force_install;

    shared.xhp->state_cb = state_cb;

    if (shared.repo) {
        shared.idx = xbps_dictionary_copy_mutable(shared.repo->idx);
        shared.idxmeta = xbps_dictionary_copy_mutable(shared.repo->idxmeta);
    } else {
        shared.idx = xbps_dictionary_create();
        shared.idxmeta = NULL;
    }

    shared.idxstage = xbps_dictionary_create();

    if (sem_init(&shared.sem_data, 0, 1) < 0) {
        rv = errno;
        goto out_close_repo;
    }

    if (sem_init(&shared.sem_prod_cons, 0, shared.manual_size) < 0) {
        rv = errno;
        goto out_destroy_sem_data;
    }

    if (tqueue_init(&shared.progress_queue) < 0) {
        rv = errno;
        goto out_destroy_sem_prod_cons;
    }

    if (sem_init(&shared.sem_progress_limit, 0, SEM_VALUE_MAX) < 0) {
        rv = errno;
        goto out_destroy_sem_progress_limit;
    }

    maxthreads = sysconf(_SC_NPROCESSORS_ONLN);
    if (maxthreads == (unsigned long)-1) {
        fprintf(stderr, "failed to get core count: %s, executing using one worker thread.\n", strerror(errno));
        maxthreads = 1;
    }

    /*
     * Start up to maxthreads threads and output their progress as such:
     *
     * vpkg_progress::CURL:
     *  name0 curl (current/total)
     *  name1 curl (current/total)
     *
     * vpkg_progress::XDEB:
     *  name0 curl (current/total)
     *  name1 xdeb
     *
     * vpkg_progress::DONE:
     *  name1 done
     *  name0 curl (current/total)
     */
    {
        pthread_t threads[maxthreads];
        struct vpkg_do_update_thread_data thread_data[maxthreads];

        unsigned long numthreads;
        for (numthreads = 0; numthreads < maxthreads; numthreads++) {
            thread_data[numthreads].shared = &shared;
            thread_data[numthreads].tid_local = numthreads;

            if ((errno = pthread_create(&threads[numthreads], NULL, vpkg_do_update_thread, &thread_data[numthreads]))) {
                if (numthreads == 0) {
                    fprintf(stderr, "pthread_create failed on first thread: %s, aborting\n", strerror(errno));
                    goto out_destroy_queue;
                } else {
                    fprintf(stderr, "pthread_create failed: %s, executing %ld threads only\n", strerror(errno), numthreads);
                    break;
                }
            }
        }

        struct vpkg_progress progress_display[numthreads];
        int tid_to_offset[numthreads];

        size_t base_offset = 0;
        size_t nrunning = 0;

        bool anyerr = false;
        for (;;) {
            // @fixme: Handle terminal overflow when ws.ws_row < maxthreads.
            struct tqueue_node *n;
            RETRY_EINTR(tqueue_get_node(&shared.progress_queue, &n));
            ASSERT_NOERR(sem_post(&shared.sem_progress_limit));

            if (n == NULL) {
                break;
            }

            struct vpkg_progress data = *(struct vpkg_progress *)n->data;
            free(n);

            if (nrunning) {
                printf("\033[%zuA", nrunning);
            }

            switch (data.state) {
            case vpkg_progress::DONE: {
                assert(nrunning > 0);

                int tid0 = progress_display[base_offset % numthreads].tid_local;
                int tid1 = data.tid_local;

                progress_display[tid_to_offset[tid1]] = progress_display[base_offset % numthreads];
                progress_display[base_offset % numthreads] = data;

                tid_to_offset[tid0] = tid_to_offset[tid1];
                tid_to_offset[tid1] = base_offset % numthreads;

                print_bar(&data);
                base_offset++, nrunning--;
                break;
            }
            case vpkg_progress::INIT: {
                assert(nrunning < numthreads);

                tid_to_offset[data.tid_local] = (base_offset + nrunning) % numthreads;
                progress_display[tid_to_offset[data.tid_local]] = data;
                nrunning++;
                break;
            }
            case vpkg_progress::ERROR: {
                assert(nrunning > 0);

                progress_display[tid_to_offset[data.tid_local]] = progress_display[(base_offset + nrunning - 1) % numthreads];
                progress_display[(base_offset + nrunning - 1) % numthreads] = data;
                break;
            }
            default: {
                progress_display[tid_to_offset[data.tid_local]] = data;
                break;
            }
            }

            for (size_t j = base_offset; j < base_offset + nrunning; j++) {
                if (print_bar(&progress_display[j % numthreads]) == vpkg_progress::ERROR) {
                    free(progress_display[j % numthreads].error_message);
                    anyerr = true;
                }
            }

            if (anyerr) {
                break;
            }
        }

        if (anyerr) {
            for (unsigned long j = 0; j < numthreads; j++) {
                if ((errno = pthread_kill(threads[j], SIGTERM))) {
                    perror("pthread_kill");
                }
            }
        }

        for (unsigned long j = 0; j < numthreads; j++) {
            if ((errno = pthread_join(threads[j], NULL))) {
                perror("pthread_join");
            }
        }
    }

    repodata_commit(shared.xhp, VPKG_BINPKGS, shared.idx, shared.idxmeta, shared.idxstage, NULL);

    for (auto &binpkgd : shared.install_xbps) {
        const char *pkgver;

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
            rv = 0;
            continue;
        case ENOENT:
            fprintf(stderr, "%s: Not found in repository pool\n", pkgver);
            xbps_object_release(binpkgd);
            goto out_destroy_queue;
        default:
            fprintf(stderr, "%s: Unexpected error: %d\n", pkgver, rv);
            xbps_object_release(binpkgd);
            goto out_destroy_queue;
        }
    }

    if (!install) {
        goto out_destroy_queue;
    }

    rv = xbps_transaction_prepare(xhp);
    switch (rv) {
    case 0:
        break;
    case ENODEV: {
        xbps_object_t obj;
        xbps_object_iterator_t it = xbps_array_iter_from_dict(xhp->transd, "missing_deps");

        while ((obj = xbps_object_iterator_next(it)) != NULL) {
            xbps_string_t str = static_cast<xbps_string_t>(obj);
            fprintf(stderr, "%s\n", xbps_string_cstring_nocopy(str));
        }

        xbps_object_iterator_release(it);
        goto out_destroy_queue;
    }
    default:
        fprintf(stderr, "transaction_prepare: unexpected error: %d\n", rv);
        goto out_destroy_queue;
    }

    if (xhp->transd) {
        xbps_object_t obj;
        xbps_object_iterator_t it = xbps_array_iter_from_dict(xhp->transd, "packages");

        while ((obj = xbps_object_iterator_next(it)) != NULL) {
            if (npackagesmodified++ == 0) {
                printf("Summary of changes:\n");
            }

            xbps_dictionary_t dict = static_cast<xbps_dictionary_t>(obj);
            const char *pkgname;
            const char *pkgver;

            assert(xbps_dictionary_get_cstring_nocopy(dict, "pkgname", &pkgname));
            assert(xbps_dictionary_get_cstring_nocopy(dict, "pkgver", &pkgver));

            pkgver = xbps_pkg_version(pkgver);

            printf("%s -> %s\n", pkgname, pkgver);
        }

        xbps_object_iterator_release(it);
    }

    if (npackagesmodified == 0) {
        fprintf(stderr, "Nothing to do.\n");
        goto out_destroy_queue;
    }

    rv = yes_no_prompt() ? 0 : -1;
    if (rv != 0) {
        fprintf(stderr, "Aborting!\n");
        goto out_destroy_queue;
    }

    rv = xbps_transaction_commit(xhp);
    switch (rv) {
    case 0:
        break;
    default:
        fprintf(stderr, "Transaction failed: %d\n", rv);
        break;
    }

out_destroy_queue:
    assert(tqueue_fini(&shared.progress_queue) == 0);

out_destroy_sem_progress_limit:
    assert(sem_destroy(&shared.sem_progress_limit) == 0);

out_destroy_sem_prod_cons:
    assert(sem_destroy(&shared.sem_prod_cons) == 0);

out_destroy_sem_data:
    assert(sem_destroy(&shared.sem_data) == 0);

out_close_repo:
    xbps_object_release(shared.idx);
    xbps_object_release(shared.idxstage);

    if (shared.idxmeta)
        xbps_object_release(shared.idxmeta);

    if (shared.repo)
        xbps_repo_close(shared.repo);

    return rv;
}

int main(int argc, char **argv)
{
    bool sync = false;
    bool force = false;
    bool update = false;
    bool install = true;

    const char *config_path = VPKG_CONFIG_PATH;
    vpkg::config config;
    xbps_handle xh;
    std::error_code ec;
    std::vector<::vpkg::packages::iterator> to_install;

    int rv = EXIT_FAILURE;
    int opt;

    memset(&xh, 0, sizeof(xh));
    curl_global_init(CURL_GLOBAL_ALL);

    while ((opt = getopt(argc, argv, ":c:vfuNS")) != -1) {
        switch (opt) {
        case 'N':
            install = false;
            break;
        case 'u':
            update = true;
            break;
        case 'c':
            config_path = optarg;
            break;
        case 'v':
            fprintf(stderr, "vpkg-%s\n", VPKG_REVISION);
            exit(EXIT_FAILURE);
            break;
        case 'S':
            sync = true;
            break;
        case 'f':
            force = true;
            break;
        default:
            usage(EXIT_FAILURE);
        }
    }

    argc -= optind, argv += optind;

    if (sync) {
        pid_t pid;
        switch ((pid = fork())) {
        int status;

        case 0:
            execlp("vpkg-sync", "vpkg-sync", NULL);
            perror("failed to execute vpkg-sync");
            exit(EXIT_FAILURE);
        case -1:
            perror("failed to fork");
            goto end_curl;
        default:
            if (RETRY_EINTR(waitpid(pid, &status, 0)) < 0) {
                perror("failed to waitpid");
                goto end_curl;
            }

            if (!WIFEXITED(status) || WEXITSTATUS(status) != EXIT_SUCCESS) {
                rv = WIFEXITED(status) ? WEXITSTATUS(status) : EXIT_FAILURE;
                goto end_curl;
            }
            break;
        }
    }

    if (vpkg::config_init(&config, config_path) != 0) {
        perror("failed to parse config file");
        goto end_munmap;
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
        auto it = config.packages.find(argv[i]);
        if (it == config.packages.end()) {
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
        cbd.packages = &config.packages;
        cbd.force = force;
        cbd.sem_data = &sem_data;
        cbd.packages_to_update = &to_install;

        xbps_pkgdb_foreach_cb_multi(&xh, vpkg_check_update_cb, &cbd);
    } else if (argc == 0) {
        fprintf(stderr, "usage: vpkg-install <package...>\n");
        goto end_xbps_lock;
    }

    if (to_install.size() == 0) {
        fprintf(stderr, "Nothing to do.\n");
        goto end_xbps_lock;
    }

    if (::download_and_install_multi(&xh, &config.packages, &to_install, force, update, install) != 0) {
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
    vpkg::config_fini(&config);

end_curl:
    curl_global_cleanup();

out:
    return rv;
}
