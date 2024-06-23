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

#include "config.hh"

#include "repodata.h"
#include "util.h"

struct vpkg_do_update_thread_data {
    const std::vector<vpkg_config::iterator> *packages_to_update;

    std::atomic<bool> *anyerr;

    std::vector<char *> *install_xbps;
    sem_t *sem_continue;

    sem_t *sem_data;
    sem_t *sem_done;

    size_t start;
    size_t size;

    size_t i;
    std::string *pkgname;
};

static int progressfn(void *arg_, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow)
{
    vpkg_do_update_thread_data *arg = static_cast<vpkg_do_update_thread_data *>(arg_);

    fprintf(stderr, "%s: %ld\n", arg->pkgname->c_str(), dlnow);
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
        std::string url{arg->packages_to_update->at(arg->i)->second.url};
        std::string pkgname{arg->packages_to_update->at(arg->i)->first};

        arg->pkgname = &pkgname;

        length = snprintf(NULL, 0, "/tmp/vpkg/%d/%s.deb", gettid(), pkgname.c_str());
        if (length < 0) {
            // @todo: Handle this
            sem_post(arg->sem_done);
            *arg->anyerr = true;
            return NULL;
        }

        char buf[length + 1];

        length = snprintf(buf, sizeof(buf), "/tmp/vpkg/%d/%s.deb", gettid(), pkgname.c_str());
        if (length < 0) {
            // @todo: Handle this
            sem_post(arg->sem_done);
            return NULL;
        }

        char *at = strrchr(buf, '/');
        assert(at);

        *at = '\0';
        if (mkdir(buf, 0644) < 0) {
            // @todo: Handle this
            sem_post(arg->sem_done);
            return NULL;
        }

        *at = '/';
        FILE *f = fopen(buf, "w");
        if (f == NULL) {
            // @todo: Handle this
            sem_post(arg->sem_done);
            return NULL;
        }

        // download all packages first
        if (download(url.c_str(), f, arg) != CURLE_OK) {
            sem_post(arg->sem_done);
            return NULL;
        }

        fclose(f);
    }

    // Post "done" after successful download
    sem_post(arg->sem_done);

    sem_wait(arg->sem_continue);
    // @todo: return if error
    sem_post(arg->sem_continue);

    for (arg->i = arg->start; arg->i < arg->start + arg->size && arg->i < arg->packages_to_update->size(); arg->i++) {
        auto &cur = arg->packages_to_update->at(arg->i);

        int pipefd[2];
        if (pipe(pipefd) < 0) {
            sem_post(arg->sem_done);
            return NULL;
        }

        char *deb_package_path;
        length = asprintf(&deb_package_path, "/tmp/vpkg/%d/%.*s.deb", gettid(), (int)cur->first.size(), cur->first.data());
        if (length < 0) {
            sem_post(arg->sem_done);
            return NULL;
        }


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

            execlp("xdeb", "xdeb", "-SedR", "--", deb_package_path, NULL);
            break;
        default:
            // ignore error, @todo: handle EINTR
            close(pipefd[1]);

            if (waitpid(pid, &status, 0) < 0) {
                perror("waitpid");
                sem_post(arg->sem_done);
                return NULL;
            }

            if (!WIFEXITED(status) || WEXITSTATUS(status) != EXIT_SUCCESS) {
                fprintf(stderr, "failed to call xdeb\n");
                sem_post(arg->sem_done);
                return NULL;
            }

            FILE *f = fdopen(pipefd[0], "r");
            // @todo: inval ignored

            char *line = NULL;
            size_t len = 0;

            ssize_t nread = getline(&line, &len, f);
            if (nread == -1) {
                fprintf(stderr, "failed to read output path\n");
                sem_post(arg->sem_done);
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
            sem_post(arg->sem_done);
            return NULL;
        }

        char buf[length + 1];
        length = snprintf(buf, sizeof(buf), "xdeb -Sed -- /tmp/vpkg/%d/%.*s.deb", gettid(), (int)cur->first.size(), cur->first.data());
        if (length < 0) {
            sem_post(arg->sem_done);
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


    // system("xdeb -Sed -- buf\n");

    return NULL;
}

int vpkg_download_and_install_multi(struct xbps_handle *xhp, const std::vector<vpkg_config::iterator> &packages_to_update)
{
    int rv;
    sem_t sem_done;
    sem_t sem_data;
    sem_t sem_continue;
    std::atomic<bool> anyerr;
    std::vector<char *> install_xbps;
    long maxthreads = sysconf(_SC_NPROCESSORS_ONLN);

    if (sem_init(&sem_done, 0, 0) < 0) {
        return -1;
    }

    if (sem_init(&sem_data, 0, 1) < 0) {
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

    printf("The following packages may be updated:\n");
    for (auto &it : packages_to_update) {
        printf("%s\n", std::string{it->first}.c_str());
    }

    if (!yes_no_prompt()) {
        fprintf(stderr, "Aborting!\n");
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
        thread_data[i].sem_done = &sem_done;
        thread_data[i].sem_data = &sem_data;
        thread_data[i].anyerr = &anyerr;

        if ((errno = pthread_create(&threads[i], NULL, vpkg_do_update_thread, &thread_data[i]))) {
            // @todo: handle this
            perror("pthread_create");
        }
    }

    int threads_done = 0;
    while (threads_done < i) {
        sem_wait(&sem_done);
        threads_done++;
    }

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

        if (!xbps_dictionary_get_cstring_nocopy(binpkgd, "pkgver", &pkgver)) {
            fprintf(stderr, "pkgver unset\n");
            xbps_object_release(binpkgd);
            continue;
        }

        rv = xbps_transaction_install_pkg(xhp, pkgver, false);
        // @todo: free vector
        switch (rv) {
        case EEXIST:
            // ignore already installed "packages may be updated"
            fprintf(stderr, "Package %s will not be updated\n", pkgver);
            continue;
        case ENOENT:
            fprintf(stderr, "Package %s not found\n", pkgver);
            return -1;
        case 0:
            break;
        default:
            fprintf(stderr, "Unexpected error: %d\n", rv);
            return -1;
        }

        rv = xbps_transaction_prepare(xhp);
        switch (rv) {
        case 0:
            break;
        default:
            fprintf(stderr, "Unexpected error: %d\n", rv);
            return -1;
        }
    }

    if (xhp->transd == NULL) {
        fprintf(stderr, "Nothing to do\n");
        return 0;
    }

    xbps_object_t obj;
    xbps_object_iterator_t it = xbps_array_iter_from_dict(xhp->transd, "packages");

    while ((obj = xbps_object_iterator_next(it)) != NULL) {
        xbps_dictionary_t dict = static_cast<xbps_dictionary_t>(obj);
        const char *pkgname;
        const char *pkgver;

        xbps_dictionary_get_cstring_nocopy(dict, "pkgname", &pkgname);
        xbps_dictionary_get_cstring_nocopy(dict, "pkgver", &pkgver);

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
