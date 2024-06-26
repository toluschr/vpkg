#include "version.hh"

#include "simdjson.h"

#include <curl/curl.h>

#include <string.h>

#if 0
static size_t simdjson_write_callback(void *data, size_t size, size_t nmemb, void *user_)
{
    std::string *user = static_cast<std::string *>(user_);
    size_t realsize = size * nmemb;
    user->append(static_cast<char *>(data), realsize);
    return realsize;
}

static bool wildcard_match(const std::string_view &wildcard, const std::string_view &match)
{
    if (wildcard.size() == 0) {
        return (match.size() == 0);
    }

    switch (wildcard.at(0)) {
    case '*':
        if (wildcard.size() == 1) return true;

        for (size_t j = 0; j < match.size(); j++) {
            if (wildcard_match(wildcard.substr(1), match.substr(j))) {
                return true;
            }
        }

        return false;
    default:
        if (match.size() == 0 || wildcard.at(0) != match.at(0)) return false;
        return wildcard_match(wildcard.substr(1), match.substr(1));
    }
}

static int get_latest_url_github(CURLU *url, const vpkg::package *e, std::string *out_url, time_t *last_modified)
{
    std::string json_str;
    CURLcode code = CURLE_OK;
    simdjson::ondemand::parser p;
    CURL *curl = curl_easy_init();

    if (curl == NULL) {
        return -1;
    }

    code = (code == CURLE_OK) ? curl_easy_setopt(curl, CURLOPT_CURLU, url) : code;
    code = (code == CURLE_OK) ? curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L) : code;
    code = (code == CURLE_OK) ? curl_easy_setopt(curl, CURLOPT_USERAGENT, "curl") : code;
    code = (code == CURLE_OK) ? curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, simdjson_write_callback) : code;
    code = (code == CURLE_OK) ? curl_easy_setopt(curl, CURLOPT_WRITEDATA, &json_str) : code;
    code = (code == CURLE_OK) ? curl_easy_perform(curl) : code;
    curl_easy_cleanup(curl);

    if (code != CURLE_OK) {
        return -1;
    }

    simdjson::ondemand::document json = p.iterate(json_str);

    auto document = json.get_object();
    if (document.error()) {
        fprintf(stderr, "github: Response is not an object\n");
        return -1;
    }

    auto assets = document["assets"].get_array();
    if (assets.error()) {
        fprintf(stderr, "github: Response does not contain array 'assets'\n");
        return -1;
    }

    for (auto asset : assets) {
        auto name = asset["name"].get_string();
        if (name.error()) {
            fprintf(stderr, "github: Response does not contain string 'name'\n");
            return -1;
        }

        if (wildcard_match(e->filename, name.value())) {
            std::string updated_at_str;
            struct tm tm;

            if (last_modified) {
                if (asset["updated_at"].get_string(updated_at_str)) {
                    fprintf(stderr, "github: Response does not contain string 'updated_at'\n");
                    return -1;
                }

                if (strptime(updated_at_str.c_str(), "%Y-%M-%dT%H:%M:%SZ", &tm) == NULL) {
                    fprintf(stderr, "unable to parse timestamp\n");
                    return -1;
                }
            }

            if (out_url) {
                if (asset["browser_download_url"].get_string(*out_url)) {
                    fprintf(stderr, "github: Response does not contain string 'browser_download_url'\n");
                    return -1;
                }
            }

            if (last_modified) {
                *last_modified = mktime(&tm);
            }
        }
    }

    return 0;
}

static int get_latest_url_generic(CURLU *url, const vpkg::package *e, std::string *out_url, time_t *last_modified)
{
    long filetime = 0;
    CURLcode code = CURLE_OK;
    CURL *curl = curl_easy_init();

    if (curl == NULL) {
        return -1;
    }

    code = (code == CURLE_OK) ? curl_easy_setopt(curl, CURLOPT_CURLU, url) : code;
    code = (code == CURLE_OK) ? curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L) : code;
    code = (code == CURLE_OK) ? curl_easy_setopt(curl, CURLOPT_NOBODY, 1L) : code;
    code = (code == CURLE_OK) ? curl_easy_setopt(curl, CURLOPT_FILETIME, 1L) : code;
    code = (code == CURLE_OK) ? curl_easy_perform(curl) : code;
    code = (code == CURLE_OK) ? curl_easy_getinfo(curl, CURLINFO_FILETIME, &filetime) : code;

    curl_easy_cleanup(curl);

    if (code != CURLE_OK) {
        return -1;
    }

    if (last_modified) {
        *last_modified = filetime;
    }

    return (code == CURLE_OK) ? 0 : -1;
}

int vpkg::package::resolve_url(time_t *out)
{
    int rv = 0;
    char *path = NULL;
    char *host = NULL;
    CURLUcode code = CURLUE_OK;

    CURLU *h = curl_url();
    if (h == NULL) {
        return -1;
    }

    if (this->base_url.size() == 0) {
        if (this->url.size() == 0) {
            return -1;
        }

        if (out == NULL) {
            return 0;
        }

        if (curl_url_set(h, CURLUPART_URL, this->url.c_str(), 0) != CURLUE_OK) {
            return -1;
        }

        if (get_latest_url_generic(h, this, NULL, out) != 0) {
            return -1;
        }

        return 0;
    }

    code = (code == CURLUE_OK) ? curl_url_set(h, CURLUPART_URL, this->base_url.c_str(), 0) : code;
    code = (code == CURLUE_OK) ? curl_url_get(h, CURLUPART_HOST, &host, 0) : code;

    if (code == CURLUE_OK && strcmp(host, "github.com") == 0) {
        free(host);

        if (curl_url_get(h, CURLUPART_PATH, &path, 0) != CURLUE_OK) {
            return -1;
        }

        std::string strpath = "";
        strpath += "/repos";
        strpath += path;
        if (strpath.back() != '/') {
            strpath += "/";
        }
        strpath += "releases/latest";
        free(path);

        code = (code == CURLUE_OK) ? curl_url_set(h, CURLUPART_HOST, "api.github.com", 0) : code;
        code = (code == CURLUE_OK) ? curl_url_set(h, CURLUPART_PATH, strpath.c_str(), 0) : code;

        if (code != CURLUE_OK) {
            return -1;
        }

        rv = get_latest_url_github(h, this, &this->url, out);
        goto out_cleanup_url;
    }

    free(host);

out_cleanup_url:
    curl_url_cleanup(h);

    return rv;
}
#endif
