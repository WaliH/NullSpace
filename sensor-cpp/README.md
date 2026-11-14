# Project AURA — code C++ de la montre

En attendant le vrai matériel, ce dossier contient un **simulateur de montre** en C++. Il génère des mesures pour les deux astronautes et les envoie au backend (`POST /api/readings`), comme le fera la vraie montre. C'est l'équivalent C++ de `backend-php\simulate-watch.bat` : mêmes options, mêmes valeurs, même API.

En plus, il fait ce que ferait une vraie montre loin de la Terre :
- **vérification à bord** (`checkOnBoard()`) : chaque mesure hors seuil déclenche une alerte locale sur la montre, même sans liaison avec la Terre ;
- **file d'attente locale** (`std::deque`) : un paquet ne quitte la montre qu'une fois enregistré par le serveur. Si le serveur ne répond pas, les paquets attendent et partent dans l'ordre au prochain envoi (500 paquets max) ;
- **coupure de liaison simulée** : `--outage=A:B` coupe la liaison de l'heure A à l'heure B ;
- **éruption solaire** : un envoi = une heure de mission. Pendant les heures 30 à 36, la dose de radiation des deux astronautes monte (warning au-dessus de 1,3 mSv/jour, critique au-dessus de 1,8 : seuils tirés de NASA-STD-3001 et des mesures de Curiosity/RAD, voir `frontend/data.json`). Avec le bouton du site (40 envois), l'éruption arrive donc vers la fin.

Aucune bibliothèque externe : C++17 standard + Winsock (inclus dans Windows). Tout est dans `simulator.cpp`.

## Fichiers

```
sensor-cpp/
  simulator.cpp     le simulateur (programme principal)
  aura_client.hpp   client HTTP minimal, réutilisable dans un autre programme
                    (simulator.cpp ne l'utilise plus : il a sa propre classe Link)
  build.bat         compile simulator.exe sous Windows
  run-simulator.bat lance simulator.exe (double-clic)
  run-crisis.bat    lance simulator.exe --crisis=all : crise pour les deux astronautes (double-clic)
```

## Compiler sous Windows

Il faut un compilateur C++ :
- **MinGW-w64** (`g++`), par exemple via MSYS2 ou w64devkit, avec son dossier `bin` dans le Path ;
- ou **Visual Studio** (avec « Développement Desktop en C++ »). Ouvrez alors « x64 Native Tools Command Prompt for VS ».

Puis double-cliquez sur **`build.bat`**, ou lancez-le depuis le terminal. Il produit `simulator.exe`.

Commandes équivalentes :
```
g++ -std=c++17 -O2 simulator.cpp -o simulator.exe -lws2_32 -static
cl /EHsc /std:c++17 /O2 simulator.cpp /Fe:simulator.exe
```
Avec MinGW, `-lws2_32` est obligatoire (bibliothèque réseau de Windows). `-static` évite les erreurs de DLL manquantes au lancement.

## Utiliser

1. Lancez d'abord le backend : `backend-php\start-server.bat`.
2. **Le plus simple** : double-cliquez sur **`run-simulator.bat`** (valeurs normales) ou sur **`run-crisis.bat`** (crise pour les deux astronautes).

   **Ou dans un terminal**, depuis `sensor-cpp`. `--crisis` n'est pas un fichier, c'est une option ajoutée après `simulator.exe`. Dans PowerShell (le terminal de VS Code), écrivez `.\simulator.exe --crisis` ; dans cmd, `simulator.exe --crisis`.
   ```
   simulator.exe                   un envoi toutes les 5 s, valeurs normales (Ctrl+C pour arrêter)
   simulator.exe --crisis          Bravo (AST-02, profil MALE) : fréquence cardiaque et tension montent jusqu'au critique
   simulator.exe --crisis=alpha    Alpha (AST-01, profil FEMALE) : fièvre, la température monte jusqu'au critique
   simulator.exe --crisis=all      les deux crises en même temps
   simulator.exe --interval=2      un envoi toutes les 2 s
   simulator.exe --count=10        s'arrête après 10 envois
   simulator.exe --count=40 --outage=28:31   liaison coupée des heures 28 à 31, puis la file est vidée
   simulator.exe --url=http://localhost:8787 --key=aura-demo-key
   ```
3. Le tableau de bord (http://localhost:8787) passe en mode live et se met à jour tout seul.

Exemple de sortie (crise + coupure de liaison) :
```
[2026-11-14T14:09:29Z] hour 31  *** SOLAR ERUPTION ***  [LINK DOWN]
    !! WATCH ALERT CRITICAL  AST-02 Heart Rate = 130 BPM  (watch vibrates)
    link down: 8 packet(s) kept in the watch queue
[2026-11-14T14:09:30Z] hour 32  *** SOLAR ERUPTION ***
     ! watch alert warning   AST-02 Radiation Dose = 1.29 mSv/day
    AST-02: 5 readings (queued since hour 28) - ALERT: heart_rate=130 critical, systolic_bp=160 critical
    AST-02: 6 readings - ALERT: heart_rate=130 critical, systolic_bp=160 critical, radiation_dose=1.29 warning
```
Les lignes `WATCH ALERT` viennent de la montre elle-même ; les lignes `ALERT:` sont la réponse du serveur.

## Ce que le programme envoie

```json
{
  "astronaut_id": "AST-02",
  "recorded_at": "2026-11-14T12:00:00Z",
  "readings": [
    { "vital_id": "heart_rate", "value": 72 },
    { "vital_id": "body_temp", "value": 36.8 }
  ]
}
```
Avec l'en-tête `X-Device-Key: aura-demo-key`. Les constantes possibles sont `heart_rate`, `systolic_bp`, `diastolic_bp`, `blood_glucose`, `body_temp`, `sleep_hours`, `bone_load` et `radiation_dose`. Les mesures de sommeil, d'exercice et de radiation ne sont envoyées qu'au premier envoi, car ce sont des valeurs quotidiennes ; la radiation est renvoyée à chaque heure de l'éruption solaire. `recorded_at` est l'heure de la mesure : un paquet resté dans la file garde son heure d'origine.

## Organisation de `simulator.cpp`

| Élément | Rôle |
|---|---|
| `Sensors::sample()` | génère les mesures d'une heure (marche aléatoire, crises, éruption solaire heures 30–36) |
| `computeStatus()` | nominal / warning / critical, même règle que `frontend.js` et `db.php` |
| `checkOnBoard()` | alerte locale sur la montre |
| `std::deque` dans `main()` | file d'attente locale (store-and-forward) |
| `--outage=A:B` | coupure de liaison simulée |
| `Link::send()` | POST HTTP + clé `X-Device-Key`, sockets (Winsock), sans libcurl |
| `Packet::toJson()` / `isoTime()` | corps JSON + horodatage ISO UTC |

## Réutiliser `aura_client.hpp` dans un autre programme

```cpp
#include "aura_client.hpp"

aura::Client client("localhost", 8787, "aura-demo-key");
aura::Response res = client.send_readings("AST-01", aura::utc_now_iso(), {
    {"heart_rate", 71},
    {"body_temp", 36.9},
});
// res.status == 201 : enregistré. res.status == 0 : serveur injoignable.
// aura::extract_alerts(res.body) : les mesures hors seuils.
```

Si la vraie montre est un microcontrôleur (ESP32, Arduino…), ce fichier ne s'y compile pas tel quel : elle utilisera la bibliothèque HTTP de sa carte. Le format JSON et l'en-tête restent les mêmes.

## Problèmes fréquents

- **« no answer - is the server running? »** : le backend n'est pas lancé, ou il tourne sur un autre port. Utilisez `--url=http://localhost:PORT`.
- **« error 401 »** : la clé ne correspond pas à `device_key` dans `backend-php\config.php`.
- **« error 422 … Unknown astronaut_id »** : la base est vide. Lancez `backend-php\seed.bat`.
- **`undefined reference to __imp_WSAStartup`** (MinGW) : il manque `-lws2_32` à la fin de la commande.

Les données sont **simulées** et les seuils sont **illustratifs** : ce ne sont pas des limites officielles de la NASA.
