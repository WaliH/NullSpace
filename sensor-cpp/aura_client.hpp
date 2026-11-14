// Project AURA — minimal HTTP client used to send measurements to the backend.
//
// Header-only, standard C++17 + sockets: Winsock on Windows, BSD sockets elsewhere.
// No external library (no curl, no JSON library), so it compiles on any team PC.
//
// It sends:  POST /api/readings
//            X-Device-Key: <key>
//            { "astronaut_id": "...", "recorded_at": "...", "readings": [ {"vital_id": "...", "value": ...}, ... ] }
// (see backend-php/README.md for the API).

#pragma once

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX            // windows.h min/max macros would break std::min / std::max
  #endif
  #ifndef _WIN32_WINNT
    #define _WIN32_WINNT 0x0601 // Windows 7+: needed by older MinGW for getaddrinfo
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #ifdef _MSC_VER
    #pragma comment(lib, "ws2_32.lib")   // MSVC links Winsock automatically; MinGW needs -lws2_32
  #endif
  using aura_socket_t = SOCKET;
  static const aura_socket_t AURA_BAD_SOCKET = INVALID_SOCKET;
  inline void aura_close_socket(aura_socket_t s) { closesocket(s); }
#else
  #include <netdb.h>
  #include <sys/socket.h>
  #include <unistd.h>
  using aura_socket_t = int;
  static const aura_socket_t AURA_BAD_SOCKET = -1;
  inline void aura_close_socket(aura_socket_t s) { close(s); }
#endif

namespace aura {

struct Reading {
    std::string vital_id;   // heart_rate, systolic_bp, diastolic_bp, blood_glucose, body_temp,
                            // sleep_hours, bone_load, radiation_dose
    double value;
};

struct Response {
    int status = 0;         // HTTP status: 201 = saved. 0 = server unreachable.
    std::string body;       // JSON answer of the server
};

// Current UTC time in the format used by the backend: 2026-11-14T12:00:00Z
inline std::string utc_now_iso() {
    std::time_t now = std::time(nullptr);
    std::tm tm{};
#if defined(_MSC_VER)
    gmtime_s(&tm, &now);
#elif defined(_WIN32)
    tm = *std::gmtime(&now);   // MinGW: gmtime_s is not always declared
#else
    gmtime_r(&now, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

inline std::string json_escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (c == '\n') out += "\\n";
        else out += c;
    }
    return out;
}

inline std::string format_value(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.2f", v);
    std::string s = buf;                                   // "72.00" -> "72", "36.80" -> "36.8"
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

inline std::string build_payload(const std::string& astronaut_id, const std::string& recorded_at,
                                 const std::vector<Reading>& readings) {
    std::ostringstream js;
    js << "{\"astronaut_id\":\"" << json_escape(astronaut_id) << "\","
       << "\"recorded_at\":\"" << json_escape(recorded_at) << "\","
       << "\"readings\":[";
    for (size_t i = 0; i < readings.size(); ++i) {
        if (i) js << ',';
        js << "{\"vital_id\":\"" << json_escape(readings[i].vital_id) << "\",\"value\":"
           << format_value(readings[i].value) << '}';
    }
    js << "]}";
    return js.str();
}

class Client {
public:
    Client(std::string host, int port, std::string device_key)
        : host_(std::move(host)), port_(port), key_(std::move(device_key)) {
#ifdef _WIN32
        WSADATA wsa;
        ok_ = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
#else
        ok_ = true;
#endif
    }
    ~Client() {
#ifdef _WIN32
        if (ok_) WSACleanup();
#endif
    }
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    const std::string& host() const { return host_; }
    int port() const { return port_; }

    Response send_readings(const std::string& astronaut_id, const std::string& recorded_at,
                           const std::vector<Reading>& readings) {
        return post("/api/readings", build_payload(astronaut_id, recorded_at, readings));
    }

    Response post(const std::string& path, const std::string& json) {
        Response res;
        if (!ok_) return res;

        aura_socket_t sock = connect_to_server();
        if (sock == AURA_BAD_SOCKET) return res;

        std::ostringstream req;
        req << "POST " << path << " HTTP/1.1\r\n"
            << "Host: " << host_ << ':' << port_ << "\r\n"
            << "Content-Type: application/json\r\n"
            << "X-Device-Key: " << key_ << "\r\n"
            << "Content-Length: " << json.size() << "\r\n"
            << "Connection: close\r\n\r\n"
            << json;
        const std::string data = req.str();

        size_t sent = 0;
        while (sent < data.size()) {
            int n = ::send(sock, data.c_str() + sent, static_cast<int>(data.size() - sent), 0);
            if (n <= 0) { aura_close_socket(sock); return res; }
            sent += static_cast<size_t>(n);
        }

        std::string raw;
        char buf[4096];
        int n;
        while ((n = ::recv(sock, buf, sizeof buf, 0)) > 0) raw.append(buf, static_cast<size_t>(n));
        aura_close_socket(sock);

        // "HTTP/1.1 201 Created\r\n...headers...\r\n\r\nbody"
        size_t sp = raw.find(' ');
        if (sp != std::string::npos) res.status = std::atoi(raw.c_str() + sp + 1);
        size_t header_end = raw.find("\r\n\r\n");
        if (header_end != std::string::npos) res.body = raw.substr(header_end + 4);
        return res;
    }

private:
    // Tries every address of the host (IPv4 and IPv6): "localhost" can be either on Windows.
    aura_socket_t connect_to_server() {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* list = nullptr;
        if (getaddrinfo(host_.c_str(), std::to_string(port_).c_str(), &hints, &list) != 0) {
            return AURA_BAD_SOCKET;
        }
        aura_socket_t sock = AURA_BAD_SOCKET;
        for (addrinfo* a = list; a; a = a->ai_next) {
            sock = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
            if (sock == AURA_BAD_SOCKET) continue;
            if (::connect(sock, a->ai_addr, static_cast<int>(a->ai_addrlen)) == 0) break;
            aura_close_socket(sock);
            sock = AURA_BAD_SOCKET;
        }
        freeaddrinfo(list);
        return sock;
    }

    std::string host_;
    int port_;
    std::string key_;
    bool ok_ = false;
};

// Reads the "alerts" list of the server's answer and returns lines like "heart_rate=112 critical".
// Tiny ad-hoc parser: enough for the backend's own answer, not a general JSON parser.
inline std::vector<std::string> extract_alerts(const std::string& body) {
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

}  // namespace aura
