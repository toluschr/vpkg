#include "install.hh"

#include <string>
#include <vector>

#include <semaphore.h>
#include <pthread.h>

#include <curl/curl.h>
#include <sys/wait.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <xbps.h>

#include <atomic>
#include <queue>

#include "config.hh"
#include "update.hh"
#include "version.hh"

#include "repodata.h"
#include "util.h"

struct vpkg_status_entry {
    vpkg_status_entry() = delete;

    vpkg_status_entry(const std::string_view &name_, int offset_,
                      curl_off_t dltotal_, curl_off_t dlnow_,
                      curl_off_t ultotal_, curl_off_t ulnow_) :
        name{name_},
        offset{offset_},
        dltotal{dltotal_},
        dlnow{dlnow_},
        ultotal{ultotal_},
        ulnow{ulnow_} {}

    std::string_view name;
    int offset;
    curl_off_t dltotal;
    curl_off_t dlnow;
    curl_off_t ultotal;
    curl_off_t ulnow;
};

struct vpkg_do_update_thread_data {
    const std::vector<vpkg_config::iterator> *packages_to_update;

    std::atomic<bool> *anyerr;
    std::atomic<int> *cur_status_offset;
    std::atomic<int> *threads_done;

    std::queue<struct vpkg_status_entry> *status;

    std::vector<char *> *install_xbps;
    sem_t *sem_continue;

    sem_t *sem_data;
    sem_t *sem_notify;

    size_t start;
    size_t size;
    int status_offset;

    size_t i;
};

static int progressfn(void *arg_, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow)
{
    vpkg_do_update_thread_data *arg = static_cast<vpkg_do_update_thread_data *>(arg_);

    sem_wait(arg->sem_data);
    arg->status->emplace(
        arg->packages_to_update->at(arg->i)->first,
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

static int download(const char *url, FILE *file, vpkg_do_update_thread_data *arg)
{
    CURLcode code = CURLE_OK;

    CURL *curl = curl_easy_init();
    if (curl == NULL) {
        return -1;
    }

    code = (code == CURLE_OK) ? curl_easy_setopt(curl, CURLOPT_URL, url) : code;
    code = (code == CURLE_OK) ? curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L) : code;
    code = (code == CURLE_OK) ? curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progressfn) : code;
    code = (code == CURLE_OK) ? curl_easy_setopt(curl, CURLOPT_XFERINFODATA, arg) : code;
    code = (code == CURLE_OK) ? curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L) : code;
    code = (code == CURLE_OK) ? curl_easy_setopt(curl, CURLOPT_WRITEDATA, file) : code;
    code = (code == CURLE_OK) ? curl_easy_perform(curl) : code;
    curl_easy_cleanup(curl);

    return (code == CURLE_OK) ? 0 : -1;
}

static void *vpkg_do_update_thread(void *arg_)
{
    int length;
    vpkg_do_update_thread_data *arg = static_cast<vpkg_do_update_thread_data *>(arg_);

    for (arg->i = arg->start; arg->i < arg->start + arg->size && arg->i < arg->packages_to_update->size(); arg->i++) {
        arg->status_offset = arg->cur_status_offset->fetch_add(1);

        std::string url{arg->packages_to_update->at(arg->i)->second.url};
        std::string pkgname{arg->packages_to_update->at(arg->i)->first};

        length = snprintf(NULL, 0, "/tmp/vpkg/%d/%s.deb", gettid(), pkgname.c_str());
        if (length < 0) {
            // @todo: Handle this
            *arg->threads_done += 1;
            sem_post(arg->sem_notify);
            *arg->anyerr = true;
            return NULL;
        }

        char buf[length + 1];

        length = snprintf(buf, sizeof(buf), "/tmp/vpkg/%d/%s.deb", gettid(), pkgname.c_str());
        if (length < 0) {
            // @todo: Handle this
            *arg->threads_done += 1;
            sem_post(arg->sem_notify);
            return NULL;
        }

        char *at = strrchr(buf, '/');
        assert(at);

        *at = '\0';
        if (mkdir(buf, 0644) < 0) {
            // @todo: Handle this
            *arg->threads_done += 1;
            sem_post(arg->sem_notify);
            return NULL;
        }

        *at = '/';
        FILE *f = fopen(buf, "w");
        if (f == NULL) {
            // @todo: Handle this
            *arg->threads_done += 1;
            sem_post(arg->sem_notify);
            return NULL;
        }

        // download all packages first
        if (download(url.c_str(), f, arg) != CURLE_OK) {
            *arg->threads_done += 1;
            sem_post(arg->sem_notify);
            return NULL;
        }

        fclose(f);
    }

    // Post "done" after successful download
    *arg->threads_done += 1;
    sem_post(arg->sem_notify);

    sem_wait(arg->sem_continue);
    // @todo: return if error
    sem_post(arg->sem_continue);

    for (arg->i = arg->start; arg->i < arg->start + arg->size && arg->i < arg->packages_to_update->size(); arg->i++) {
        auto &cur = arg->packages_to_update->at(arg->i);

        int pipefd[2];
        if (pipe(pipefd) < 0) {
            *arg->threads_done += 1;
            sem_post(arg->sem_notify);
            return NULL;
        }

        char *deb_package_path;
        length = asprintf(&deb_package_path, "/tmp/vpkg/%d/%.*s.deb", gettid(), (int)cur->first.size(), cur->first.data());
        if (length < 0) {
            *arg->threads_done += 1;
            sem_post(arg->sem_notify);
            return NULL;
        }

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
            if (dup2(pipefd[1], STDOUT_FILENO) < 0) {
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

            if (close(pipefd[0]) < 0) {
                // @todo message
                exit(EXIT_FAILURE);
            }

            fprintf(stderr, "%s\n", deb_package_path);

            not_deps = "--not-deps=";
            not_deps += cur->second.not_deps;

            deps = "--deps=";
            deps += cur->second.deps;

            execlp("xdeb", "xdeb", "-edR", not_deps.c_str(), deps.c_str(), "--", deb_package_path, NULL);
            break;
        default:
            // ignore error, @todo: handle EINTR
            close(pipefd[1]);

            if (waitpid(pid, &status, 0) < 0) {
                perror("waitpid");
                *arg->threads_done += 1;
                sem_post(arg->sem_notify);
                return NULL;
            }

            if (!WIFEXITED(status) || WEXITSTATUS(status) != EXIT_SUCCESS) {
                fprintf(stderr, "Failed to build %.*s\n", (int)cur->first.size(), cur->first.data());
                *arg->threads_done += 1;
                sem_post(arg->sem_notify);
                return NULL;
            }

            FILE *f = fdopen(pipefd[0], "r");
            // @todo: inval ignored

            char *line = NULL;
            size_t len = 0;

            ssize_t nread = getline(&line, &len, f);
            if (nread == -1) {
                fprintf(stderr, "failed to read output path\n");
                *arg->threads_done += 1;
                sem_post(arg->sem_notify);
                return NULL;
            }

            if (nread > 0 && line[nread - 1] == '\n') {
                line[--nread] = '\0';
            }

            sem_wait(arg->sem_data);
            arg->install_xbps->push_back(line);
            sem_post(arg->sem_data);

            fclose(f);
            break;
        }


        // @todo: assert size < INT_MAX
        // @todo: assert no special chars in pkgname
        /*
        length = snprintf(NULL, 0, "xdeb -Sed -- /tmp/vpkg/%d/%.*s.deb", gettid(), (int)cur->first.size(), cur->first.data());
        if (length < 0) {
            sem_post(arg->sem_notify);
            return NULL;
        }

        char buf[length + 1];
        length = snprintf(buf, sizeof(buf), "xdeb -Sed -- /tmp/vpkg/%d/%.*s.deb", gettid(), (int)cur->first.size(), cur->first.data());
        if (length < 0) {
            sem_post(arg->sem_notify);
            return NULL;
        }

        int retval = system(buf);
        if (retval < 0) {
            perror("xdeb");
        }

        if (retval != 0) {
            fprintf(stderr, "xdeb failed\n");
        }
        */

        // printf("%d\n", retval);
    }


    sem_post(arg->sem_notify);
    return NULL;
}

int vpkg_download_and_install_multi(struct xbps_handle *xhp, const std::vector<vpkg_config::iterator> &packages_to_update, bool force_reinstall)
{
    int rv;
    sem_t sem_notify;
    sem_t sem_data;
    sem_t sem_continue;
    std::queue<vpkg_status_entry> status;
    std::vector<char *> install_xbps;
    std::atomic<bool> anyerr = false;
    std::atomic<int> threads_done = 0;
    std::atomic<int> cur_status_offset = 0;
    long maxthreads = sysconf(_SC_NPROCESSORS_ONLN);

    if (sem_init(&sem_data, 0, 1) < 0) {
        return -1;
    }

    if (sem_init(&sem_notify, 0, 0) < 0) {
        return -1;
    }

    if (sem_init(&sem_continue, 0, 0) < 0) {
        return -1;
    }

    if (maxthreads < 0) {
        // @todo: handle this
        return -1;
    }

    if (maxthreads >= packages_to_update.size()) {
        maxthreads = packages_to_update.size();
    }

    printf("Install candidates:\n");
    for (auto &it : packages_to_update) {
        printf("%s\n", std::string{it->first}.c_str());
    }

    if (!yes_no_prompt()) {
        fprintf(stderr, "Aborting!\n");
        return -1;
    }

    if (system("xdeb -SQ >/dev/null 2>&1") != EXIT_SUCCESS) {
        fprintf(stderr, "failed to update shlibs\n");
        return -1;
    }

    pthread_t threads[maxthreads];
    struct vpkg_do_update_thread_data thread_data[maxthreads];

    int i;
    for (i = 0; i < maxthreads; i++) {
        thread_data[i].packages_to_update = &packages_to_update;
        thread_data[i].size = (packages_to_update.size() + (maxthreads - 1)) / maxthreads;
        thread_data[i].start = i * thread_data[i].size;
        thread_data[i].install_xbps = &install_xbps;
        thread_data[i].sem_continue = &sem_continue;
        thread_data[i].sem_notify = &sem_notify;
        thread_data[i].sem_data = &sem_data;
        thread_data[i].threads_done = &threads_done;
        thread_data[i].status = &status;
        thread_data[i].cur_status_offset = &cur_status_offset;
        thread_data[i].anyerr = &anyerr;

        if ((errno = pthread_create(&threads[i], NULL, vpkg_do_update_thread, &thread_data[i]))) {
            // @todo: handle this
            perror("pthread_create");
        }
    }

    int last_offset_printed = 0;
    while (threads_done < i) {
        sem_wait(&sem_notify);

        for (;;) {
            sem_wait(&sem_data);
            if (status.size() == 0) {
                sem_post(&sem_data);
                break;
            }

            vpkg_status_entry temp_entry = status.front();
            status.pop();
            sem_post(&sem_data);

            if (last_offset_printed < temp_entry.offset)
                fprintf(stderr, "\033[%dB", temp_entry.offset - last_offset_printed);
            if (last_offset_printed > temp_entry.offset)
                fprintf(stderr, "\033[%dA", last_offset_printed - temp_entry.offset);

            double progress;
            if (temp_entry.dltotal == 0)
                progress = 0;
            else
                progress = (100.0l * temp_entry.dlnow) / (double)temp_entry.dltotal;

            fprintf(stderr, "\r\033[K%d %.*s %g %%", temp_entry.offset, (int)temp_entry.name.size(), temp_entry.name.data(), progress);

            last_offset_printed = temp_entry.offset;
        }
    }

    printf("\n");

    if (anyerr) {
        for (int j = 0; j < i; j++) {
            pthread_kill(threads[j], SIGINT);
        }

        fprintf(stderr, "error\n");
        return -1;
    }

    sem_post(&sem_continue);

    for (int j = 0; j < i; j++) {
        if ((errno = pthread_join(threads[j], NULL))) {
            perror("pthread_join");
        }
    }

    // Adds all packages to repo
    index_add(xhp, install_xbps.data(), install_xbps.size());

    for (auto &pathname : install_xbps) {
        const char *pkgver;

        xbps_dictionary_t binpkgd = xbps_archive_fetch_plist(pathname, "/props.plist");
        free(pathname);

        if (!xbps_dictionary_get_cstring_nocopy(binpkgd, "pkgver", &pkgver)) {
            fprintf(stderr, "pkgver unset\n");
            xbps_object_release(binpkgd);
            continue;
        }

        rv = xbps_transaction_install_pkg(xhp, pkgver, force_reinstall); // false);

        // @todo: free vector
        switch (rv) {
        case EEXIST:
            // ignore already installed "packages may be updated"
            fprintf(stderr, "%s: Already installed\n", pkgver);
            xbps_object_release(binpkgd);
            continue;
        case ENOENT:
            fprintf(stderr, "%s: Not found in repository pool\n", pkgver);
            xbps_object_release(binpkgd);
            return -1;
        case 0:
            break;
        default:
            fprintf(stderr, "%s: Unespected error: %d\n", pkgver, rv);
            xbps_object_release(binpkgd);
            return -1;
        }

        rv = xbps_transaction_prepare(xhp);
        switch (rv) {
        case 0:
            xbps_object_release(binpkgd);
            break;
        case ENODEV:
            fprintf(stderr, "%s: Missing dependencies.\n", pkgver, rv);
            xbps_object_release(binpkgd);
            return -1;
        default:
            fprintf(stderr, "%s: Unexpected error: %d\n", pkgver, rv);
            xbps_object_release(binpkgd);
            return -1;
        }
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

int vpkg_do_install(vpkg_context *ctx, int argc, char **argv)
{
    if (argc == 0) {
        fprintf(stderr, "usage: vpkg install <package...>\n");
        return -1;
    }

    std::vector<vpkg_config::iterator> to_install;
    to_install.reserve(argc);

    for (int i = 0; i < argc; i++) {
        auto it = ctx->config.find(argv[i]);
        if (it == ctx->config.end()) {
            fprintf(stderr, "package %s not found\n", argv[i]);
            continue;
        }

        if (!ctx->force_reinstall) {
            if (xbps_dictionary_get(ctx->xbps_handle.pkgdb, argv[i])) {
                fprintf(stderr, "%s: Already installed.\n", argv[i]);
                continue;
            }
        }

        if (it->second.resolve_url(NULL) != 0) {
            fprintf(stderr, "%s: Unable to resolve URL.\n", argv[i]);
            continue;
        }

        to_install.push_back(it);
    }

    if (to_install.size() == 0) {
        return -1;
    }

    return vpkg_download_and_install_multi(&ctx->xbps_handle, to_install, ctx->force_reinstall);
}
