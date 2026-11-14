# AuraWatch — health monitoring for astronauts

**NASA Space Apps Challenge 2026 · Team Null Space**

AuraWatch is a watch worn by each astronaut. It measures their vital signs and sends them to Earth, where mission control follows the crew's health on a web dashboard (**AURA // HRP·OS**). The dashboard flags readings that leave their safe range, shows the countermeasure to apply, and keeps an archive of alerts that ground control acknowledges.

> ⚠️ **All data is simulated** for this prototype, and the thresholds are **illustrative**: they are **not** official NASA limits.

This repository contains:

| Folder | What it is |
|---|---|
| `frontend/` | the dashboard (HTML, CSS, JavaScript, no framework) and the demo data `data.json` |
| `backend-php/` | the ground server (PHP + SQLite): receives the watch readings, stores alerts, serves the dashboard |
| `sensor-cpp/` | the watch simulator in C++ (stands in for the hardware): sensors, on-board alerts, local queue when the link to Earth is down |

---

## Option 1 — Full version (recommended, about 5 minutes)

This version includes everything: live watch data, the alert archive, and the button that runs the watch simulator.

### What you need

- **PHP 8** with the `pdo_sqlite` extension. That's the only requirement: no database server, no Apache, no Composer.
- *Optional:* a C++17 compiler, to build the C++ watch simulator. Without it, the dashboard automatically uses an equivalent PHP simulator that sends the same values.

### Windows

1. **Install PHP**, using one of these:
   - **XAMPP** (https://www.apachefriends.org): nothing else to do, the scripts find `C:\xampp\php\php.exe` on their own.
   - **PHP alone**: download the *x64 Non Thread Safe* zip from https://windows.php.net/download/ and unzip it to `C:\php`. Add `C:\php` to the `Path` environment variable. Copy `php.ini-development` to `php.ini`, then remove the `;` in front of `extension_dir = "ext"` and `extension=pdo_sqlite`.
2. **Download the project**: on GitHub, click **Code → Download ZIP** and unzip it, or run `git clone https://github.com/WaliH/NullSpace.git`.
3. **Start the server**: double-click **`backend-php\start-server.bat`**.
   - The first time, it creates the database from the demo data.
   - The browser opens on **http://localhost:8787**. Keep the black window open while you use the dashboard.
4. *Optional:* build the C++ simulator by double-clicking **`sensor-cpp\build.bat`**. This needs MinGW-w64 (`g++`) or Visual Studio.

### macOS / Linux

```bash
# 1. PHP 8 with SQLite
#    macOS:          brew install php
#    Ubuntu/Debian:  sudo apt install php-cli php-sqlite3

# 2. Get the project
git clone https://github.com/WaliH/NullSpace.git
cd NullSpace/backend-php

# 3. Create the database (first time only) and start the server
php bin/seed.php
php -S localhost:8787 -t ../frontend router.php
```
Then open **http://localhost:8787**.

*Optional:* build the C++ simulator (from the `NullSpace` folder):
```bash
cd sensor-cpp && g++ -std=c++17 -O2 simulator.cpp -o simulator
```

---

## Option 2 — Quick look, without PHP

If you only want to see the dashboard with the demo scenario, Python is enough. With this option there's no live watch data, no alert archive, and no simulator button.

```bash
cd frontend
python -m http.server 8000      # macOS / Linux: python3 -m http.server 8000
```
Open **http://localhost:8000**. Opening `index.html` directly (`file://`) doesn't work, because the page has to load `data.json`.

---

## Guided tour (≈ 5 minutes)

1. **Mission baseline.** The page opens on the crew's normal state (pill *Mission Baseline · Demo Data* at the top). Each card is one vital sign: heart rate, blood pressure, glucose, core temperature, exercise against bone loss, sleep, and radiation dose. Each card shows its safe range and the last 12 hours as a small graph.
2. **Switch astronaut.** **MALE** shows Astronaut Bravo (flight engineer) and **FEMALE** shows Astronaut Alpha (commander). The coloured dot on each button summarizes that astronaut's state, so you can spot a problem without switching.
3. **Open a card.** Click any card, or a point on the body silhouette, to see the reading and the **countermeasure** to apply (for example "Rest and do guided breathing"). The filters (*Cardio, Metabolic, Immuno, Neuro, Musculo*) group the cards by body system.
4. **Replay a solar storm.** Click **⚡ Simulate Solar Flare (SPE)**. The dashboard replays a mission day hour by hour: Bravo's heart rate and blood pressure spike, Alpha develops a fever, and both radiation doses become critical. The graphs turn orange (warning) and red (critical), the decision log narrates each change, and alerts appear in **Module 02 // Alert Archive**.
5. **Acknowledge an alert.** In the archive, click **ACKNOWLEDGE**, or use **Acknowledge & Close** in a card's window. The server records who acknowledged it and when.
6. **Go live with the watch.** In **Live Watch Simulator**, choose a crisis and click **▶ Run**. The server starts the watch simulator (C++ `simulator.exe` if built, PHP otherwise). Within seconds the pill turns **Live Watch Downlink**, and the values update as packets arrive from the "watch" (one every 3 s). It stops after 2 minutes, or when you click **■ Stop**.

### From a terminal: the C++ watch simulator

With the server running, the C++ simulator can also be used directly. It shows what the watch itself does:

```bash
cd sensor-cpp
simulator.exe --crisis=all                    # Windows (in PowerShell: .\simulator.exe --crisis=all)
./simulator --crisis=all                      # macOS / Linux
./simulator --count=40 --interval=1 --outage=28:31
```
- Each batch represents one mission hour. A **solar eruption** happens during hours 30–36, and the radiation dose rises to critical.
- **On-board check:** the watch raises its own alerts (`WATCH ALERT`) even without a link to Earth.
- **Store and forward:** with `--outage=A:B`, the link to Earth is cut from hour A to hour B. Readings stay in the watch's queue and are delivered in order, with their original time, once the link is back.

---

## Troubleshooting

| Problem | Fix |
|---|---|
| The browser asks for a **username and password** | Another program (Tomcat, Oracle…) answers on that port. Close it, or change `PORT` at the top of `start-server.bat` **and** `port` in `backend-php\config.php`. |
| "PHP was not found" | PHP is not in the `Path`. Install XAMPP, or reopen the terminal after editing `Path`. |
| "pdo_sqlite is not enabled" | In `php.ini`, remove the `;` in front of `extension=pdo_sqlite`. `php --ini` shows which file is used. |
| The page shows "TELEMETRY LINK DOWN" | The page was opened as a file. Use http://localhost:8787 (Option 1) or http://localhost:8000 (Option 2). |
| You want the original demo values back | Run `backend-php\seed.bat --reset` (macOS/Linux: `php bin/seed.php --reset`). This deletes the readings sent by the simulator. |

---

## How it works (for the curious)

```
 Watch (sensor-cpp)                Ground server (backend-php)            Dashboard (frontend)
 ─────────────────                 ───────────────────────────            ────────────────────
 sensors → on-board check   ──►   POST /api/readings                ──►   GET /api/telemetry (every 5 s)
 local queue if link down          status computed from thresholds         cards, graphs, silhouette
                                   alerts grouped per problem       ──►   GET /api/alerts
                                   SQLite database                  ◄──   POST /api/alerts/{id}/ack
```

- The same status rule (nominal / warning / critical, from the thresholds in `data.json`) is applied on the watch, on the server, and on the dashboard.
- The API is documented in [`backend-php/README.md`](backend-php/README.md), and the simulator in [`sensor-cpp/README.md`](sensor-cpp/README.md). Both are in French.
