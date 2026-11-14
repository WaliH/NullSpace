// AuraWatch — watch simulator in C++.
//
// Plays the role of the watch until the real hardware is ready: it generates measurements
// for the two astronauts, checks them on board, keeps them in a local queue and sends them
// to the backend (POST /api/readings). Same options and same values as
// backend-php/bin/simulate-watch.php.
//
// One batch = one simulated mission hour (batch 0 = hour 0).
//
//   Sensors::sample()         → measurements (random walk + crisis) + solar eruption, hours 30–36
//   computeStatus()           → nominal / warning / critical (same rule as frontend.js and db.php)
//   checkOnBoard()            → local alert on the watch, works without the ground link
//   std::deque in main()      → local queue: packets wait there until the ground has them
//   --outage=A:B              → simulates a link outage from hour A to hour B (inclusive)
//   Link::send()              → HTTP POST + device token, plain sockets (no libcurl)
//   Packet::toJson(), isoTime() → JSON body + ISO-8601 UTC timestamp
//
// ALL DATA IS SIMULATED. Thresholds are illustrative, NOT NASA limits.
//
// Build on Windows (see sensor-cpp/README.md):
//   build.bat
//   or MinGW:   g++ -std=c++17 -O2 simulator.cpp -o simulator.exe -lws2_32 -static
//   or MSVC:    cl /EHsc /std:c++17 /O2 simulator.cpp /Fe:simulator.exe
//
// Usage (the backend must be running: backend-php\start-server.bat):
//   simulator.exe                         one batch every 5 s, nominal values, until Ctrl+C
//   simulator.exe --crisis                Bravo: heart rate and blood pressure drift up to critical
//   simulator.exe --crisis=alpha          Alpha: fever (temperature up to critical, heart rate up)
//   simulator.exe --crisis=all            both at the same time
//   simulator.exe --outage=28:31          link down from hour 28 to 31, the queue is sent afterwards
//   simulator.exe --interval=2 --count=10
//   simulator.exe --url=http://localhost:8787 --key=aura-demo-key

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #ifdef _MSC_VER
    #pragma comment(lib, "ws2_32.lib")
  #endif
  using socket_t = SOCKET;
  static const socket_t BAD_SOCKET = INVALID_SOCKET;
  static void close_socket(socket_t s) { closesocket(s); }
#else
  #include <netdb.h>
  #include <sys/socket.h>
  #include <sys/time.h>
  #include <sys/types.h>
  #include <unistd.h>
  using socket_t = int;
  static const socket_t BAD_SOCKET = -1;
  static void close_socket(socket_t s) { close(s); }
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <iostream>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// Options (same as bin/simulate-watch.php; --outage is C++ only)
// ---------------------------------------------------------------------------

struct Options {
    std::string host = "localhost";
    int port = 8787;                        // same default as backend-php/config.php
    std::string key = "aura-demo-key";      // same default as backend-php/config.php
    int interval_s = 5;
    int count = 0;                          // 0 = run until Ctrl+C
    bool crisis_alpha = false;              // AST-01: fever
    bool crisis_bravo = false;              // AST-02: heart rate + blood pressure
    int outage_from = -1, outage_to = -1;   // link down between these hours (inclusive)
};

void print_help() {
    std::cout <<
        "AuraWatch watch simulator (simulated data, illustrative thresholds)\n"
        "  --url=http://HOST:PORT   backend address (default http://localhost:8787)\n"
        "  --key=KEY                device key, X-Device-Key header (default aura-demo-key)\n"
        "  --interval=N             seconds between two batches (default 5)\n"
        "  --count=N                number of batches, 0 = until Ctrl+C (default 0)\n"
        "  --crisis                 Bravo (AST-02): heart rate and blood pressure rise to critical\n"
        "  --crisis=alpha           Alpha (AST-01): fever, temperature rises to critical\n"
        "  --crisis=all             both crises at the same time\n"
        "  --outage=A:B             link down from hour A to hour B: packets stay in the watch queue\n"
        "One batch = one mission hour. Solar eruption (radiation) during hours 30-36.\n";
}

bool starts_with(const std::string& s, const std::string& prefix) {
    return s.compare(0, prefix.size(), prefix) == 0;
}

// "http://localhost:8787" -> host "localhost", port 8787
bool parse_url(std::string url, Options& o) {
    if (starts_with(url, "http://")) url = url.substr(7);
    else if (starts_with(url, "https://")) {
        std::cerr << "HTTPS is not supported by this simulator: use http://\n";
        return false;
    }
    url = url.substr(0, url.find('/'));
    size_t colon = url.rfind(':');
    if (colon != std::string::npos) {
        o.host = url.substr(0, colon);
        o.port = std::atoi(url.c_str() + colon + 1);
    } else {
        o.host = url;
        o.port = 80;
    }
    return !o.host.empty() && o.port > 0;
}

// "28:31" -> 28, 31
bool parse_outage(const std::string& range, Options& o) {
    size_t colon = range.find(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= range.size()) {
        std::cerr << "--outage expects A:B, for example --outage=28:31\n";
        return false;
    }
    o.outage_from = std::max(0, std::atoi(range.c_str()));
    o.outage_to = std::max(0, std::atoi(range.c_str() + colon + 1));
    if (o.outage_to < o.outage_from) std::swap(o.outage_from, o.outage_to);
    return true;
}

bool parse_args(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--help" || a == "-h") { print_help(); return false; }
        else if (a == "--crisis" || a == "--crisis=bravo") o.crisis_bravo = true;
        else if (a == "--crisis=alpha") o.crisis_alpha = true;
        else if (a == "--crisis=all") o.crisis_alpha = o.crisis_bravo = true;
        else if (a == "--crisis=none") o.crisis_alpha = o.crisis_bravo = false;
        else if (starts_with(a, "--url=")) { if (!parse_url(a.substr(6), o)) return false; }
        else if (starts_with(a, "--key=")) o.key = a.substr(6);
        else if (starts_with(a, "--interval=")) o.interval_s = std::max(1, std::atoi(a.c_str() + 11));
        else if (starts_with(a, "--count=")) o.count = std::max(0, std::atoi(a.c_str() + 8));
        else if (starts_with(a, "--outage=")) { if (!parse_outage(a.substr(9), o)) return false; }
        else if (a == "--outage" && i + 1 < argc) { if (!parse_outage(argv[++i], o)) return false; }
        else { std::cerr << "Unknown option: " << a << "\n"; print_help(); return false; }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Thresholds (same as frontend/data.json "vitals") and status rule
// ---------------------------------------------------------------------------

struct Vital {
    const char* id;
    const char* label;
    const char* unit;
    double critical_low, warning_low, warning_high, critical_high;   // NAN = no limit
};

const Vital VITALS[] = {
    {"heart_rate",     "Heart Rate",        "BPM",     40,  60,   90,   110},
    {"systolic_bp",    "Systolic BP",       "mmHg",    80,  90,   135,  150},
    {"diastolic_bp",   "Diastolic BP",      "mmHg",    50,  60,   85,   95},
    {"blood_glucose",  "Blood Glucose",     "mg/dL",   65,  70,   100,  125},
    {"body_temp",      "Body Temperature",  "C",       35,  36.5, 37.5, 38.3},
    {"sleep_hours",    "Sleep Duration",    "hours",   5,   7,    NAN,  NAN},
    {"bone_load",      "Bone-Loss Exercise","min/day", 30,  60,   NAN,  NAN},
    {"radiation_dose", "Radiation Dose",    "mSv/day", NAN, NAN,  1,    2},
};

const Vital* find_vital(const std::string& id) {
    for (const Vital& v : VITALS)
        if (id == v.id) return &v;
    return nullptr;
}

enum class Status { nominal = 0, warning = 1, critical = 2 };

// Same rule as computeStatus() in frontend.js and aura_compute_status() in backend-php/src/db.php.
Status computeStatus(const std::string& vital_id, double value) {
    const Vital* v = find_vital(vital_id);
    if (!v) return Status::nominal;
    auto below = [](double limit, double x) { return !std::isnan(limit) && x < limit; };
    auto above = [](double limit, double x) { return !std::isnan(limit) && x > limit; };
    if (below(v->critical_low, value) || above(v->critical_high, value)) return Status::critical;
    if (below(v->warning_low, value) || above(v->warning_high, value)) return Status::warning;
    return Status::nominal;
}

// ---------------------------------------------------------------------------
// Packet: one POST /api/readings
// ---------------------------------------------------------------------------

// Current UTC time in the format used by the backend: 2026-11-14T12:00:00Z
std::string isoTime(std::time_t t = std::time(nullptr)) {
    std::tm tm{};
#if defined(_MSC_VER)
    gmtime_s(&tm, &t);
#elif defined(_WIN32)
    tm = *std::gmtime(&t);   // MinGW: gmtime_s is not always declared
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

std::string json_escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else out += c;
    }
    return out;
}

// 72.00 -> "72", 36.80 -> "36.8"
std::string format_value(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.2f", v);
    std::string s = buf;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

struct Measure {
    std::string vital_id;
    double value;
};

struct Packet {
    std::string astronaut_id;
    std::string recorded_at;    // time of the measurement: kept while the packet waits in the queue
    int hour = 0;               // mission hour, for the console only
    std::vector<Measure> readings;

    // {"astronaut_id":"AST-02","recorded_at":"...","readings":[{"vital_id":"heart_rate","value":72}]}
    std::string toJson() const {
        std::ostringstream o;
        o << "{\"astronaut_id\":\"" << json_escape(astronaut_id) << "\","
          << "\"recorded_at\":\"" << json_escape(recorded_at) << "\",\"readings\":[";
        for (size_t i = 0; i < readings.size(); ++i) {
            o << (i ? "," : "") << "{\"vital_id\":\"" << json_escape(readings[i].vital_id)
              << "\",\"value\":" << format_value(readings[i].value) << "}";
        }
        o << "]}";
        return o.str();
    }
};

// ---------------------------------------------------------------------------
// Sensors: simulated measurements for one astronaut
// ---------------------------------------------------------------------------

const int FLARE_START = 30;   // solar particle event: mission hours 30–36
const int FLARE_END = 36;
// Radiation added to the daily dose during each hour of the eruption (bell shape, total 1.8 mSv).
const double FLARE_DOSE[FLARE_END - FLARE_START + 1] = {0.10, 0.22, 0.35, 0.42, 0.35, 0.22, 0.14};

// Allowed range and max step of the random walk, close to the nominal baseline of data.json.
struct Bounds { double min, max, step; };
const std::map<std::string, Bounds> BOUNDS = {
    {"heart_rate",    {60, 85, 2}},
    {"systolic_bp",   {105, 130, 2}},
    {"diastolic_bp",  {65, 82, 1.5}},
    {"blood_glucose", {80, 98, 2}},
    {"body_temp",     {36.5, 37.3, 0.1}},
};

// Order in which the frequent vitals are sent.
const std::vector<std::string> FREQUENT = {"heart_rate", "systolic_bp", "diastolic_bp", "blood_glucose", "body_temp"};

double round_to(double v, int decimals) {
    double f = std::pow(10.0, decimals);
    return std::round(v * f) / f;
}

class Sensors {
public:
    Sensors(std::string id, std::map<std::string, double> values, std::map<std::string, double> daily,
            bool crisis, unsigned seed)
        : id_(std::move(id)), values_(std::move(values)), daily_(std::move(daily)), crisis_(crisis),
          rng_(seed), dose_(daily_["radiation_dose"]) {}

    const std::string& id() const { return id_; }

    // Measurements of mission hour `hour` (= batch number). Same values as bin/simulate-watch.php.
    std::vector<Measure> sample(int hour) {
        std::uniform_real_distribution<double> unit(-1.0, 1.0);
        for (const std::string& vital : FREQUENT) {
            const Bounds& b = BOUNDS.at(vital);
            double& v = values_[vital];
            v = std::clamp(v + unit(rng_) * b.step, b.min, b.max);
        }

        // Crisis scenarios.
        if (crisis_ && id_ == "AST-02") {          // Bravo: cardiovascular
            values_["heart_rate"] = std::min(130.0, 88.0 + hour * 6);
            values_["systolic_bp"] = std::min(160.0, 128.0 + hour * 5);
        }
        if (crisis_ && id_ == "AST-01") {          // Alpha: fever
            values_["body_temp"] = std::min(39.0, 37.2 + hour * 0.25);
            values_["heart_rate"] = std::min(105.0, 74.0 + hour * 3);
        }

        std::vector<Measure> out;
        for (const std::string& vital : FREQUENT) {
            out.push_back({vital, round_to(values_[vital], vital == "body_temp" ? 1 : 0)});
        }

        // Daily values: sent with the first batch only.
        if (hour == 0) {
            for (const auto& [vital, value] : daily_) out.push_back({vital, value});
        }

        // Solar eruption: the whole crew is exposed, the daily dose rises every hour
        // (warning above 1 mSv/day, critical above 2) and stays at its new level afterwards.
        if (hour >= FLARE_START && hour <= FLARE_END) {
            dose_ += FLARE_DOSE[hour - FLARE_START];
            out.push_back({"radiation_dose", round_to(dose_, 2)});
        }
        return out;
    }

private:
    std::string id_;
    std::map<std::string, double> values_;   // frequent vitals, updated every batch
    std::map<std::string, double> daily_;    // sleep, exercise, radiation at hour 0
    bool crisis_;
    std::mt19937 rng_;
    double dose_;                            // daily radiation dose, mSv/day
};

// ---------------------------------------------------------------------------
// On-board check: the watch warns the astronaut itself, even with no link to Earth
// ---------------------------------------------------------------------------

Status checkOnBoard(const Packet& p) {
    Status worst = Status::nominal;
    for (const Measure& m : p.readings) {
        Status s = computeStatus(m.vital_id, m.value);
        if (s == Status::nominal) continue;
        worst = std::max(worst, s);
        const Vital* v = find_vital(m.vital_id);
        std::cout << "    " << (s == Status::critical ? "!! WATCH ALERT CRITICAL" : " ! watch alert warning ")
                  << "  " << p.astronaut_id << " " << v->label << " = " << format_value(m.value) << " " << v->unit
                  << (s == Status::critical ? "  (watch vibrates)" : "") << "\n";
    }
    return worst;
}

// ---------------------------------------------------------------------------
// Link: HTTP/1.1 over a plain TCP socket (Winsock on Windows), no libcurl
// ---------------------------------------------------------------------------

struct Response {
    int status = 0;         // HTTP status: 201 = saved. 0 = server unreachable.
    std::string body;       // JSON answer of the server
};

class Link {
public:
    Link(std::string host, int port, std::string token)
        : host_(std::move(host)), port_(std::to_string(port)), token_(std::move(token)) {}

    Response send(const std::string& json, const std::string& path = "/api/readings") {
        Response res;
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* list = nullptr;
        if (getaddrinfo(host_.c_str(), port_.c_str(), &hints, &list) != 0 || !list) return res;

        socket_t s = BAD_SOCKET;
        for (addrinfo* a = list; a; a = a->ai_next) {          // IPv6 or IPv4
            s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
            if (s == BAD_SOCKET) continue;
            set_timeouts(s, 5000);
            if (connect(s, a->ai_addr, (int)a->ai_addrlen) == 0) break;
            close_socket(s);
            s = BAD_SOCKET;
        }
        freeaddrinfo(list);
        if (s == BAD_SOCKET) return res;

        std::ostringstream req;
        req << "POST " << path << " HTTP/1.1\r\n"
            << "Host: " << host_ << ":" << port_ << "\r\n"
            << "User-Agent: AuraWatch-Simulator\r\n"
            << "Content-Type: application/json\r\n"
            << "X-Device-Key: " << token_ << "\r\n"
            << "Content-Length: " << json.size() << "\r\n"
            << "Connection: close\r\n\r\n"
            << json;
        const std::string data = req.str();

        for (size_t sent = 0; sent < data.size();) {
            int n = ::send(s, data.data() + sent, (int)(data.size() - sent), 0);
            if (n <= 0) { close_socket(s); return res; }
            sent += (size_t)n;
        }

        std::string raw;
        char buf[2048];
        int n;
        while ((n = recv(s, buf, sizeof buf, 0)) > 0) raw.append(buf, (size_t)n);
        close_socket(s);

        // "HTTP/1.1 201 Created\r\n...\r\n\r\n{...}"
        size_t sp = raw.find(' ');
        if (sp != std::string::npos) res.status = std::atoi(raw.c_str() + sp + 1);
        size_t body = raw.find("\r\n\r\n");
        if (body != std::string::npos) res.body = raw.substr(body + 4);
        return res;
    }

private:
    static void set_timeouts(socket_t s, int ms) {
#ifdef _WIN32
        DWORD t = (DWORD)ms;
#else
        timeval t{ms / 1000, (ms % 1000) * 1000};
#endif
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&t, sizeof t);
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&t, sizeof t);
    }

    std::string host_, port_, token_;
};

// Reads the "alerts" list of the server's answer and returns lines like "heart_rate=112 critical".
// Tiny ad-hoc parser: enough for the backend's own answer, not a general JSON parser.
std::vector<std::string> extract_alerts(const std::string& body) {
    std::vector<std::string> out;
    size_t pos = body.find("\"alerts\"");
    if (pos == std::string::npos) return out;

    auto field = [&](const std::string& name, size_t from, size_t& end) -> std::string {
        size_t k = body.find("\"" + name + "\"", from);
        if (k == std::string::npos) return "";
        size_t colon = body.find(':', k);
        size_t start = body.find_first_not_of(" \t\r\n\"", colon + 1);
        end = body.find_first_of(",}\"\r\n", start);
        return body.substr(start, end - start);
    };

    while ((pos = body.find("\"vital_id\"", pos)) != std::string::npos) {
        size_t end = pos;
        std::string vital = field("vital_id", pos, end);
        std::string value = field("value", end, end);
        std::string status = field("status", end, end);
        out.push_back(vital + "=" + value + " " + status);
        pos = end;
    }
    return out;
}

// Error message of the server, e.g. {"error":"Unknown astronaut_id"}
std::string error_message(const Response& r) {
    size_t k = r.body.find("\"error\"");
    if (k == std::string::npos) return r.body.substr(0, 120);
    size_t start = r.body.find('"', r.body.find(':', k) + 1);
    size_t end = r.body.find('"', start + 1);
    return start == std::string::npos ? "" : r.body.substr(start + 1, end - start - 1);
}

}  // namespace

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    Options opt;
    if (!parse_args(argc, argv, opt)) return 1;

#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { std::cerr << "WSAStartup failed\n"; return 1; }
#endif

    std::random_device seed;
    std::vector<Sensors> crew = {
        Sensors("AST-01",
                {{"heart_rate", 68}, {"systolic_bp", 118}, {"diastolic_bp", 75}, {"blood_glucose", 90}, {"body_temp", 36.8}},
                {{"sleep_hours", 7.3}, {"bone_load", 75}, {"radiation_dose", 0.6}},
                opt.crisis_alpha, seed()),
        Sensors("AST-02",
                {{"heart_rate", 72}, {"systolic_bp", 124}, {"diastolic_bp", 79}, {"blood_glucose", 94}, {"body_temp", 36.9}},
                {{"sleep_hours", 7.6}, {"bone_load", 80}, {"radiation_dose", 0.62}},
                opt.crisis_bravo, seed()),
    };
    Link link(opt.host, opt.port, opt.key);

    // Local queue of the watch, oldest first: a packet leaves it only once the ground has saved it,
    // so nothing is lost during an outage and the history arrives in order afterwards.
    std::deque<Packet> queue;
    const size_t QUEUE_MAX = 500;           // limited watch memory: the oldest packets are dropped beyond this
    int dropped = 0;

    std::string mode;
    if (opt.crisis_alpha && opt.crisis_bravo) mode = " (crisis: Alpha fever + Bravo cardiovascular)";
    else if (opt.crisis_alpha) mode = " (crisis: Alpha fever)";
    else if (opt.crisis_bravo) mode = " (crisis: Bravo cardiovascular)";

    std::cout << "Sending to http://" << opt.host << ':' << opt.port << "/api/readings every "
              << opt.interval_s << "s" << mode << ". Ctrl+C to stop.\n"
              << "One batch = one mission hour. Solar eruption: hours " << FLARE_START << "-" << FLARE_END << ".";
    if (opt.outage_from >= 0) std::cout << " Link outage: hours " << opt.outage_from << "-" << opt.outage_to << ".";
    std::cout << "\nSimulated data, illustrative thresholds (not NASA limits).\n";

    for (int hour = 0; opt.count == 0 || hour < opt.count; ++hour) {
        const std::string time = isoTime();
        const bool outage = opt.outage_from >= 0 && hour >= opt.outage_from && hour <= opt.outage_to;
        const bool flare = hour >= FLARE_START && hour <= FLARE_END;

        std::cout << "[" << time << "] hour " << hour
                  << (flare ? "  *** SOLAR ERUPTION ***" : "") << (outage ? "  [LINK DOWN]" : "") << "\n";

        // 1. Measure, check on board, put in the queue.
        for (Sensors& astro : crew) {
            Packet p;
            p.astronaut_id = astro.id();
            p.recorded_at = time;
            p.hour = hour;
            p.readings = astro.sample(hour);
            checkOnBoard(p);
            queue.push_back(std::move(p));
            if (queue.size() > QUEUE_MAX) { queue.pop_front(); ++dropped; }
        }

        // 2. Send the queue, oldest first.
        if (outage) {
            std::cout << "    link down: " << queue.size() << " packet(s) kept in the watch queue\n";
        }
        while (!outage && !queue.empty()) {
            const Packet& p = queue.front();
            Response res = link.send(p.toJson());
            std::string late = p.hour != hour ? " (queued since hour " + std::to_string(p.hour) + ")" : "";

            if (res.status == 0 || res.status >= 500 || res.status == 429) {
                // Ground unreachable or busy: keep everything, retry at the next batch.
                std::cout << "    " << p.astronaut_id << ": "
                          << (res.status ? "error " + std::to_string(res.status) : "no answer - is the server running? (backend-php\\start-server.bat)")
                          << " - " << queue.size() << " packet(s) kept, retry next batch\n";
                break;
            }
            if (res.status != 201) {
                // Rejected (bad key, unknown astronaut...): retrying would not help, drop it.
                std::cout << "    " << p.astronaut_id << ": error " << res.status << " - " << error_message(res) << late << "\n";
                if (res.status == 401) std::cout << "    -> the key must match device_key in backend-php\\config.php\n";
            } else {
                std::cout << "    " << p.astronaut_id << ": " << p.readings.size() << " readings" << late;
                std::vector<std::string> alerts = extract_alerts(res.body);
                if (!alerts.empty()) {
                    std::cout << " - ALERT: ";
                    for (size_t k = 0; k < alerts.size(); ++k) std::cout << (k ? ", " : "") << alerts[k];
                }
                std::cout << "\n";
            }
            queue.pop_front();
        }
        std::cout.flush();

        if (opt.count == 0 || hour < opt.count - 1) {
            std::this_thread::sleep_for(std::chrono::seconds(opt.interval_s));
        }
    }

    if (!queue.empty() || dropped) {
        std::cout << "Stopped with " << queue.size() << " packet(s) never delivered"
                  << (dropped ? ", " + std::to_string(dropped) + " dropped (queue full)" : "") << ".\n";
    }

#ifdef _WIN32
    WSACleanup();
#endif
    return queue.empty() ? 0 : 3;
}
