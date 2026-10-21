// metro_user: try out accounts and saved journeys from the command line (the web pages come later).
//   metro_user register NAME                 create an account (asks for the password twice)
//   metro_user login NAME                    log in; remembers the session in a small file
//   metro_user whoami | logout
//   metro_user save --from A --to B [--mode fastest|fewest] [--day weekday|sunday] [--label TEXT]
//   metro_user list
//   metro_user open ID
//   metro_user favourite ID [--off]
//   metro_user delete ID
// Options: --db FILE (default data/metro.db), --session FILE (default data/session.token)
//
// The session file holds a secret token. Anyone who can read it can act as you, so keep it private.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "../auth/accounts.h"
#include "../auth/journeys.h"
#include "../common/db.h"
#include "../route/fare.h"
#include "../route/network.h"
#include "../route/planner.h"
#include "../route/router.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <termios.h>
#include <unistd.h>
#endif

using namespace metro;
using namespace metro::auth;
using namespace metro::route;

namespace {

void usage() {
    std::cout << "Usage:\n"
                 "  metro_user register NAME\n"
                 "  metro_user login NAME\n"
                 "  metro_user whoami\n"
                 "  metro_user logout\n"
                 "  metro_user save --from NAME --to NAME [--mode fastest|cheapest|fewest] [--day weekday|sunday] [--label TEXT]\n"
                 "  metro_user list\n"
                 "  metro_user open ID\n"
                 "  metro_user favourite ID [--off]\n"
                 "  metro_user delete ID\n"
                 "Options: --db FILE (default data/metro.db)  --session FILE (default data/session.token)\n"
                 "Usernames: 3-20 letters or digits. Passwords: at least 8 characters.\n"
                 "In this tool, type passwords with ASCII characters (the web version will accept any language).\n";
}

std::string readPassword(const std::string& prompt) {
    std::cout << prompt << std::flush;
    std::string pw;
#ifdef _WIN32
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    const bool tty = GetConsoleMode(h, &mode) != 0;
    if (tty) SetConsoleMode(h, mode & ~static_cast<DWORD>(ENABLE_ECHO_INPUT));
    std::getline(std::cin, pw);
    if (tty) {
        SetConsoleMode(h, mode);
        std::cout << "\n";
    }
#else
    termios old;
    const bool tty = isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &old) == 0;
    if (tty) {
        termios quiet = old;
        quiet.c_lflag &= static_cast<tcflag_t>(~ECHO);
        tcsetattr(STDIN_FILENO, TCSANOW, &quiet);
    }
    std::getline(std::cin, pw);
    if (tty) {
        tcsetattr(STDIN_FILENO, TCSANOW, &old);
        std::cout << "\n";
    }
#endif
    while (!pw.empty() && (pw.back() == '\r' || pw.back() == '\n')) pw.pop_back();
    return pw;
}

std::string readToken(const std::string& path) {
    std::ifstream f(path);
    std::string t;
    if (f) std::getline(f, t);
    while (!t.empty() && (t.back() == '\r' || t.back() == '\n' || t.back() == ' ')) t.pop_back();
    return t;
}

int resolveOrExplain(const Network& n, const std::string& what, const std::string& query) {
    Resolution r = resolveStation(n, query);
    if (r.status == Resolution::Status::Found) return r.station;
    if (r.status == Resolution::Status::Ambiguous) {
        std::cerr << "'" << query << "' (" << what << ") matches several stations. Be more specific, or use id:<number>:\n";
        for (size_t i = 0; i < r.candidates.size() && i < 10; ++i) std::cerr << "    " << n.station(r.candidates[i]).name << " (id:" << n.station(r.candidates[i]).gtfsId << ")\n";
    } else {
        std::cerr << "No station matches '" << query << "' (" << what << ").";
        if (!r.candidates.empty()) {
            std::cerr << " Did you mean:\n";
            for (int c : r.candidates) std::cerr << "    " << n.station(c).name << " (id:" << n.station(c).gtfsId << ")\n";
        } else {
            std::cerr << "\n";
        }
    }
    return 0;
}

std::string stationNameByGtfsId(const Network& n, const std::string& gtfsId) {
    for (const auto& s : n.stations)
        if (s.gtfsId == gtfsId) return s.name;
    return gtfsId + " (no longer in the network)";
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty() || args[0] == "--help" || args[0] == "-h") {
        usage();
        return args.empty() ? 2 : 0;
    }
    const std::string command = args[0];
    std::string dbPath = "data/metro.db", sessionPath = "data/session.token", from, to, mode = "fastest", dayName = "weekday", label, positional;
    bool off = false;
    for (size_t i = 1; i < args.size(); ++i) {
        const std::string& a = args[i];
        const bool hasValue = i + 1 < args.size();
        if (a == "--db" && hasValue) dbPath = args[++i];
        else if (a == "--session" && hasValue) sessionPath = args[++i];
        else if (a == "--from" && hasValue) from = args[++i];
        else if (a == "--to" && hasValue) to = args[++i];
        else if (a == "--mode" && hasValue) mode = args[++i];
        else if (a == "--day" && hasValue) dayName = args[++i];
        else if (a == "--label" && hasValue) label = args[++i];
        else if (a == "--off") off = true;
        else if (!a.empty() && a[0] != '-' && positional.empty()) positional = a;
        else {
            std::cerr << "Unknown or incomplete option: " << a << "\n\n";
            usage();
            return 2;
        }
    }

    try {
        Database db(dbPath, false);
        Network net = loadNetwork(db);
        AccountService accounts(db);

        if (command == "register") {
            if (positional.empty()) {
                usage();
                return 2;
            }
            const std::string pw = readPassword("Password: "), again = readPassword("Repeat the password: ");
            if (pw != again) {
                std::cerr << "The two passwords are not the same.\n";
                return 1;
            }
            RegisterResult r = accounts.registerUser(positional, pw);
            std::cout << r.message << "\n";
            return r.status == Status::Ok ? 0 : 1;
        }
        if (command == "login") {
            if (positional.empty()) {
                usage();
                return 2;
            }
            LoginResult r = accounts.login(positional, readPassword("Password: "));
            std::cout << r.message << "\n";
            if (r.status != Status::Ok) return 1;
            std::ofstream f(sessionPath, std::ios::binary | std::ios::trunc);
            if (!f) {
                std::cerr << "Logged in, but the session file " << sessionPath << " could not be written.\n";
                return 1;
            }
            f << r.token << "\n";
            std::cout << "Session saved to " << sessionPath << " (keep that file private).\n";
            return 0;
        }

        // every other command needs a valid session
        const std::string token = readToken(sessionPath);
        auto account = accounts.validateSession(token);
        if (command == "logout") {
            accounts.logout(token);
            std::remove(sessionPath.c_str());
            std::cout << "Logged out.\n";
            return 0;
        }
        if (!account) {
            std::cerr << "You are not logged in (or the session has expired). Run: metro_user login NAME\n";
            return 1;
        }
        if (command == "whoami") {
            std::cout << "Logged in as " << account->username << ".\n";
            return 0;
        }

        Router router(net);
        FareCalculator fares(net);
        Planner planner(net, router, fares);
        JourneyService journeys(db, net, planner, fares);

        if (command == "save") {
            if (from.empty() || to.empty()) {
                std::cerr << "save needs --from and --to.\n";
                return 2;
            }
            Preference savePrefer = Preference::Fastest;
            if (!parsePreference(mode, savePrefer) || (dayName != "weekday" && dayName != "sunday")) {
                std::cerr << "--mode must be fastest, cheapest or fewest, and --day must be weekday or sunday.\n";
                return 2;
            }
            const int a = resolveOrExplain(net, "from", from), b = resolveOrExplain(net, "to", to);
            if (a == 0 || b == 0) return 1;
            SaveResult r = journeys.save(account->id, a, b, savePrefer,
                                         dayName == "weekday" ? DayType::Weekday : DayType::SundayHoliday, label);
            std::cout << r.message;
            if (r.ok) std::cout << " (id " << r.id << ")";
            std::cout << "\n";
            return r.ok ? 0 : 1;
        }
        if (command == "list") {
            const auto all = journeys.list(account->id);
            if (all.empty()) {
                std::cout << "No saved journeys yet.\n";
                return 0;
            }
            for (const auto& j : all) {
                std::cout << (j.favourite ? "* " : "  ") << "#" << j.id << "  " << stationNameByGtfsId(net, j.fromGtfsId) << " -> " << stationNameByGtfsId(net, j.toGtfsId)
                          << "  [" << (j.mode == "fastest" ? "fastest" : (j.mode == "cheapest" ? "cheapest" : "fewest changes")) << ", " << (j.dayType == "weekday" ? "weekday" : "Sunday/holiday") << "]  about "
                          << (j.totalSeconds + 30) / 60 << " min, " << j.changes << (j.changes == 1 ? " change" : " changes")
                          << (j.fareInr >= 0 ? ", Rs " + std::to_string(j.fareInr) : std::string(", no fare")) << (j.label.empty() ? "" : "  \"" + j.label + "\"")
                          << "  (saved " << formatUtc(j.updatedAt) << ")\n";
            }
            return 0;
        }
        if (command == "open" || command == "favourite" || command == "delete") {
            char* end = nullptr;
            const long long id = std::strtoll(positional.c_str(), &end, 10);
            if (positional.empty() || !end || *end != '\0') {
                std::cerr << command << " needs a journey number, for example: metro_user " << command << " 3\n";
                return 2;
            }
            if (command == "open") {
                auto r = journeys.reopen(account->id, id);
                if (!r) {
                    std::cerr << "No saved journey with that number.\n";
                    return 1;
                }
                if (!r->note.empty()) std::cout << "NOTE: " << r->note << "\n\n";
                std::cout << r->text;
                return 0;
            }
            if (command == "favourite") {
                if (!journeys.setFavourite(account->id, id, !off)) {
                    std::cerr << "No saved journey with that number.\n";
                    return 1;
                }
                std::cout << (off ? "Removed from favourites.\n" : "Marked as a favourite.\n");
                return 0;
            }
            if (!journeys.remove(account->id, id)) {
                std::cerr << "No saved journey with that number.\n";
                return 1;
            }
            std::cout << "Deleted.\n";
            return 0;
        }
        std::cerr << "Unknown command: " << command << "\n\n";
        usage();
        return 2;
    } catch (const std::exception& e) {
        std::cerr << "ERROR: " << e.what() << "\n";
        return 1;
    }
}
