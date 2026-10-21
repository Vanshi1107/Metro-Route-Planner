// The web API: every request the browser can make, as one function from Request to Response.
//
// It knows nothing about sockets. A thin adapter (src/tools/metro_web.cpp) connects it to a real HTTP server, and
// the tests call handle() directly, so all of this is tested without a network.
//
// Security built in:
//  * every change (POST/DELETE) must carry the header  X-Requested-With: metro-web  (a browser will not add that
//    header to a request sent from another site without permission), and its Origin, if present, must be this site
//  * session cookie: HttpOnly, SameSite=Strict (and Secure when ApiConfig::secureCookies is set)
//  * strict Content-Security-Policy on every response; only OpenStreetMap's tile servers may be used for images
//  * request size limits; static files are served only from the web folder, by a strict name check
//  * the server asks the database for the data version on each request and reloads the network when
//    metro_import has run, so no restart is needed after an import
//  * requests are handled one at a time (a mutex), which keeps the single database connection safe
#pragma once
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "../auth/accounts.h"
#include "../common/db.h"
#include "http.h"

namespace metro {
namespace web {

struct ApiConfig {
    std::string webRoot = "web";
    bool secureCookies = false;               // set when the site is served over HTTPS
    auth::AccountOptions account;             // its clock (if set) is also used for saved journeys
    std::function<void(const std::string&)> log;   // told about internal errors (never sent to the browser)
    size_t maxBodyBytes = 8 * 1024;
};

class Api {
public:
    explicit Api(Database& db, ApiConfig config = ApiConfig());
    ~Api();
    Api(const Api&) = delete;
    Api& operator=(const Api&) = delete;

    Response handle(const Request& request);   // never throws
    int dataVersion();

private:
    struct World;
    Response dispatch(const Request& request);
    Response handleApi(const Request& request);
    Response serveStatic(const std::string& path);
    std::optional<auth::Account> currentAccount(const Request& request);
    void reloadIfDataChanged();

    Database& db_;
    ApiConfig cfg_;
    auth::AccountService accounts_;
    std::unique_ptr<World> world_;
    std::string loadedVersion_;
    std::mutex mutex_;
};

}  // namespace web
}  // namespace metro
