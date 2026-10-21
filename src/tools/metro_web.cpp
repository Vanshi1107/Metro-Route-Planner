// metro_web: the web server. It connects the Api (src/web/api.cpp) to the cpp-httplib library.
//   metro_web [--db data/metro.db] [--web web] [--port 8080] [--bind 127.0.0.1]
//
// Everything that matters (routing, security checks, cookies, headers) is in Api and is tested without a network.
// This file only converts between the library's request/response types and ours, and sets timeouts and size limits.
// By default it listens on 127.0.0.1 only, so only this computer can reach it.
#include <cstdlib>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif
#include "httplib.h"

#include "../common/db.h"
#include "../web/api.h"
#include "../web/http.h"

namespace {

void usage() {
    std::cout << "Usage: metro_web [--db FILE] [--web FOLDER] [--port N] [--bind ADDRESS]\n"
                 "  --db FILE      database made by metro_db and metro_import (default data/metro.db)\n"
                 "  --web FOLDER   the web pages (default web)\n"
                 "  --port N       port to listen on (default 8080)\n"
                 "  --bind ADDR    address to listen on (default 127.0.0.1: this computer only)\n"
                 "There is no HTTPS: do not expose this to a network you do not trust, and never use real passwords on a shared network.\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    std::string dbPath = "data/metro.db", webRoot = "web", bind = "127.0.0.1";
    int port = 8080;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        const bool hasValue = i + 1 < args.size();
        if (a == "--db" && hasValue) dbPath = args[++i];
        else if (a == "--web" && hasValue) webRoot = args[++i];
        else if (a == "--bind" && hasValue) bind = args[++i];
        else if (a == "--port" && hasValue) {
            port = std::atoi(args[++i].c_str());
            if (port < 1 || port > 65535) {
                std::cerr << "--port must be between 1 and 65535\n";
                return 2;
            }
        } else if (a == "--help" || a == "-h") {
            usage();
            return 0;
        } else {
            std::cerr << "Unknown or incomplete option: " << a << "\n\n";
            usage();
            return 2;
        }
    }

    try {
        metro::Database db(dbPath, false);
        metro::web::ApiConfig cfg;
        cfg.webRoot = webRoot;
        cfg.log = [](const std::string& m) { std::cerr << "[error] " << m << "\n"; };
        metro::web::Api api(db, cfg);

        httplib::Server server;
        server.set_read_timeout(5, 0);
        server.set_write_timeout(5, 0);
        server.set_idle_interval(1, 0);
        server.set_payload_max_length(16 * 1024);

        auto handler = [&api](const httplib::Request& req, httplib::Response& res) {
            std::map<std::string, std::string> headers;
            for (const auto& h : req.headers) headers[h.first] = h.second;
            const metro::web::Request converted = metro::web::makeRequest(req.method, req.target, headers, req.body);
            const metro::web::Response out = api.handle(converted);
            res.status = out.status;
            for (const auto& h : out.headers) res.set_header(h.first, h.second);
            res.set_content(out.body, out.contentType);
        };
        server.Get(".*", handler);
        server.Post(".*", handler);
        server.Delete(".*", handler);
        server.Put(".*", handler);
        server.Patch(".*", handler);
        server.Options(".*", handler);

        std::cout << "Delhi Metro Planner is running at http://" << (bind == "0.0.0.0" ? std::string("localhost") : bind) << ":" << port << "/\n"
                  << "Data version " << api.dataVersion() << ". Press Ctrl+C to stop.\n";
        if (!server.listen(bind.c_str(), port)) {
            std::cerr << "Could not listen on " << bind << ":" << port << ". Is another program using that port?\n";
            return 1;
        }
    } catch (const std::exception& e) {
        std::cerr << "ERROR: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
