// AuraWatch — watch simulator in C++.
//
// Plays the role of the watch until the real hardware is ready: it generates measurements
// for the two astronauts and sends them to the backend (POST /api/readings), exactly like
// backend-php/bin/simulate-watch.php.
//
// Build on Windows (see sensor-cpp/README.md):
//   build.bat
//   or MinGW:   g++ -std=c++17 -O2 simulator.cpp -o simulator.exe -lws2_32
//   or MSVC:    cl /EHsc /std:c++17 /O2 simulator.cpp
//
// Usage (the backend must be running: backend-php\start-server.bat):
//   simulator.exe                         one batch every 5 s, nominal values, until Ctrl+C
//   simulator.exe --crisis                Bravo: heart rate and blood pressure drift up to critical
//   simulator.exe --crisis=alpha          Alpha: fever (temperature up to critical, heart rate up)
//   simulator.exe --crisis=all            both at the same time
//   simulator.exe --interval=2 --count=10
//   simulator.exe --url=http://localhost:8787 --key=aura-demo-key

#include "aura_client.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <map>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Options {
    std::string host = "localhost";
    int port = 8787;                        // same default as backend-php/config.php
    std::string key = "aura-demo-key";      // same default as backend-php/config.php
    int interval_s = 5;
    int count = 0;                          // 0 = run until Ctrl+C
    bool crisis_alpha = false;              // AST-01: fever
    bool crisis_bravo = false;              // AST-02: heart rate + blood pressure
};

void print_help() {
    std::cout <<
        "AuraWatch watch simulator\n"
        "  --url=http://HOST:PORT   backend address (default http://localhost:8787)\n"
        "  --key=KEY                device key, X-Device-Key header (default aura-demo-key)\n"
        "  --interval=N             seconds between two batches (default 5)\n"
        "  --count=N                number of batches, 0 = until Ctrl+C (default 0)\n"
        "  --crisis                 Bravo (AST-02): heart rate and blood pressure rise to critical\n"
        "  --crisis=alpha           Alpha (AST-01): fever, temperature rises to critical\n"
        "  --crisis=all             both crises at the same time\n";
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

bool parse_args(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--help" || a == "-h") { print_help(); return false; }
        else if (a == "--crisis" || a == "--crisis=bravo") o.crisis_bravo = true;
        else if (a == "--crisis=alpha") o.crisis_alpha = true;
        else if (a == "--crisis=all") o.crisis_alpha = o.crisis_bravo = true;
        else if (starts_with(a, "--url=")) { if (!parse_url(a.substr(6), o)) return false; }
        else if (starts_with(a, "--key=")) o.key = a.substr(6);
        else if (starts_with(a, "--interval=")) o.interval_s = std::max(1, std::atoi(a.c_str() + 11));
        else if (starts_with(a, "--count=")) o.count = std::max(0, std::atoi(a.c_str() + 8));
        else { std::cerr << "Unknown option: " << a << "\n"; print_help(); return false; }
    }
    return true;
}

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

struct Astronaut {
    std::string id;
    std::map<std::string, double> values;   // frequent vitals, updated every batch
    std::map<std::string, double> daily;    // sent with the first batch only
};

double round_to(double v, int decimals) {
    double f = std::pow(10.0, decimals);
    return std::round(v * f) / f;
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parse_args(argc, argv, opt)) return 1;

    std::vector<Astronaut> crew = {
        {"AST-01",
         {{"heart_rate", 68}, {"systolic_bp", 118}, {"diastolic_bp", 75}, {"blood_glucose", 90}, {"body_temp", 36.8}},
         {{"sleep_hours", 7.3}, {"bone_load", 75}, {"radiation_dose", 0.6}}},
        {"AST-02",
         {{"heart_rate", 72}, {"systolic_bp", 124}, {"diastolic_bp", 79}, {"blood_glucose", 94}, {"body_temp", 36.9}},
         {{"sleep_hours", 7.6}, {"bone_load", 80}, {"radiation_dose", 0.62}}},
    };

    std::mt19937 rng(std::random_device{}());
    std::uniform_real_distribution<double> unit(-1.0, 1.0);

    aura::Client client(opt.host, opt.port, opt.key);

    std::string mode;
    if (opt.crisis_alpha && opt.crisis_bravo) mode = " (crisis: Alpha fever + Bravo cardiovascular)";
    else if (opt.crisis_alpha) mode = " (crisis: Alpha fever)";
    else if (opt.crisis_bravo) mode = " (crisis: Bravo cardiovascular)";

    std::cout << "Sending to http://" << opt.host << ':' << opt.port << "/api/readings every "
              << opt.interval_s << "s" << mode << ". Ctrl+C to stop.\n";

    for (int i = 0; opt.count == 0 || i < opt.count; ++i) {
        const std::string time = aura::utc_now_iso();

        for (Astronaut& astro : crew) {
            for (const std::string& vital : FREQUENT) {
                const Bounds& b = BOUNDS.at(vital);
                double& v = astro.values[vital];
                v = std::clamp(v + unit(rng) * b.step, b.min, b.max);
            }
            // Crisis scenarios. Same values as backend-php/bin/simulate-watch.php.
            if (opt.crisis_bravo && astro.id == "AST-02") {
                astro.values["heart_rate"] = std::min(130.0, 88.0 + i * 6);
                astro.values["systolic_bp"] = std::min(160.0, 128.0 + i * 5);
            }
            if (opt.crisis_alpha && astro.id == "AST-01") {
                astro.values["body_temp"] = std::min(39.0, 37.2 + i * 0.25);
                astro.values["heart_rate"] = std::min(105.0, 74.0 + i * 3);
            }

            std::vector<aura::Reading> readings;
            for (const std::string& vital : FREQUENT) {
                readings.push_back({vital, round_to(astro.values[vital], vital == "body_temp" ? 1 : 0)});
            }
            if (i == 0) {
                for (const auto& [vital, value] : astro.daily) readings.push_back({vital, value});
            }

            aura::Response res = client.send_readings(astro.id, time, readings);

            std::cout << '[' << time << "] " << astro.id << ": ";
            if (res.status == 0) {
                std::cout << "no answer - is the server running? (backend-php\\start-server.bat)\n";
            } else if (res.status != 201) {
                std::cout << "error " << res.status << " - " << res.body << "\n";
            } else {
                std::cout << readings.size() << " readings";
                std::vector<std::string> alerts = aura::extract_alerts(res.body);
                if (!alerts.empty()) {
                    std::cout << " - ALERT: ";
                    for (size_t k = 0; k < alerts.size(); ++k) std::cout << (k ? ", " : "") << alerts[k];
                }
                std::cout << "\n";
            }
        }
        std::cout.flush();

        if (opt.count == 0 || i < opt.count - 1) {
            std::this_thread::sleep_for(std::chrono::seconds(opt.interval_s));
        }
    }
    return 0;
}
