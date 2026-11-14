# Project AURA — contexte projet pour Claude Code

## Le projet
Équipe au hackathon **NASA Space Apps Challenge 2026**.
Sujet : logiciel de suivi de santé pour astronautes en mission spatiale.
Concept : une montre (projet AURA) mesure les constantes de l'astronaute et les envoie vers la Terre ; toute la surveillance se fait sur un site web (tableau de bord « AURA // HRP·OS »).

Toutes les données sont **simulées** pour la démo. Les seuils sont **illustratifs**, ce ne sont **pas** des limites officielles NASA — ne jamais les présenter comme tels.

## Structure du dépôt (github.com/WaliH/NullSpace)
```
frontend/
  index.html     → page du tableau de bord (cartes de constantes + silhouette interactive + modale)
  frontend.css   → styles (thème sombre « verre », variables CSS dans :root)
  frontend.js    → toute la logique : chargement des données, statuts, rendu, replay
  data.json      → données simulées (doit rester À CÔTÉ de index.html)
backend-php/     → backend PHP + SQLite : reçoit les mesures de la montre, sert l'API et le site
                   (détails et installation Windows : backend-php/README.md)
sensor-cpp/      → code C++ de la montre : simulator.cpp (simulateur de montre, autonome : capteurs,
                   vérification à bord, file locale, client HTTP Winsock) + aura_client.hpp (client HTTP
                   réutilisable, plus utilisé par simulator.cpp). Compilation : build.bat
```
`README.md` (racine, en anglais) : guide pour le jury (installation Windows / macOS / Linux, visite guidée, dépannage). Le tenir à jour quand une étape ou un bouton change.

Le frontend est 100 % statique, sans framework ni build, JavaScript vanilla.

**L'équipe travaille sous Windows.** Les commandes et scripts doivent marcher sous Windows (cmd / PowerShell) :
chemins avec `\` dans les exemples, scripts `.bat` (fins de ligne CRLF, imposées par `.gitattributes`),
pas de commandes propres à Linux. En PHP, construire les chemins avec `__DIR__`.

## Lancer le site
Avec le backend (recommandé) : double-cliquer sur `backend-php\start-server.bat`, puis ouvrir http://localhost:8787.
Le simulateur de montre : `backend-php\simulate-watch.bat` (options `--crisis` = Bravo, `--crisis=alpha` = fièvre d'Alpha, `--crisis=all` = les deux). Même chose en C++ : `sensor-cpp\simulator.exe`.

Sans backend : le site doit quand même être servi en HTTP (pas en `file://`) :
```
cd frontend
python -m http.server 8000
```
puis ouvrir http://localhost:8000 (ou l'extension Live Server de VS Code). Le site lit alors `data.json`.

## Données (`data.json`)
- `meta` : infos du projet et avertissement « données simulées ».
- `astronauts` : `AST-01` Astronaut Alpha (F, Commander), `AST-02` Astronaut Bravo (M, Flight Engineer).
- `vitals` : une ligne par constante, avec `zone`, `unit`, `critical_low`, `warning_low`, `warning_high`, `critical_high`, `nominal_message`, `countermeasure`.
  Constantes : `heart_rate`, `systolic_bp`, `diastolic_bp`, `blood_glucose`, `body_temp`, `sleep_hours`, `bone_load`, `radiation_dose`.
- `readings` : mesures horaires `{reading_id, recorded_at (ISO UTC), astronaut_id, vital_id, value, status}`.
  - Jour 1 (2026-11-14) = référence nominale (on affiche la dernière heure du jour 1).
  - Dernier jour (2026-11-15) = scénario d'éruption solaire (SPE), rejoué par le bouton « Simulate Solar Flare ».
- Sources d'origine (hors de ce dossier) : `AuraWatch_data.xlsx`, `schema.sql`, `seed.sql`. `computeStatus()` dans le JS applique la même règle que la vue SQL `v_readings_status` : les deux doivent rester identiques.

## Backend (`backend-php/`)
- PHP 8 sans dépendance, base SQLite (`data/aurawatch.sqlite`, non versionnée), lancé avec le serveur intégré :
  `php -S localhost:8787 -t ..\frontend router.php` (`router.php` envoie `/api/...` vers `src/api.php`, le reste vers `frontend/`).
- `GET /api/telemetry` → même format que `data.json`, avec un champ `source` en plus sur chaque mesure : `scenario` (données de démo) ou `watch` (envoyée par la montre).
- `POST /api/readings` (en-tête `X-Device-Key`) → la montre envoie `{astronaut_id, recorded_at?, readings:[{vital_id, value}]}`. Le serveur recalcule le statut.
- `GET /api/alerts` / `POST /api/alerts/{id}/ack` → alertes (`src/alerts.php`). Créées par `aura_track_alert()` pour chaque mesure hors seuil (watch et scénario importé). Une alerte = un problème (astronaute + constante + source) : tant qu'elle est ouverte, les mesures suivantes la mettent à jour (`peak_value`, `last_value`, `readings_count`).
- `GET /api/simulator`, `POST /api/simulator/start` (`{"crisis": "all|alpha|bravo|none"}`), `POST /api/simulator/stop` → le site lance/arrête le simulateur de montre (`src/simulator.php`) : `sensor-cpp/simulator.exe` s'il est compilé, sinon `bin/simulate-watch.php`. Processus en arrière-plan (Windows : PowerShell `Start-Process` + `taskkill` ; PID dans `data/simulator.json`), 40 envois toutes les 3 s puis arrêt automatique. Start/stop refusés hors `localhost`.
- `GET /api/health` → vérification rapide.
- Port par défaut **8787** (pas 8080, souvent pris sous Windows par Tomcat/XAMPP, Oracle…) : `PORT` dans `start-server.bat` et `port` dans `config.php` doivent rester identiques.
- `bin/seed.php` importe `frontend/data.json` (`--reset` efface aussi les mesures de la montre). `bin/simulate-watch.php` simule la montre.
- Le simulateur C++ (`sensor-cpp/simulator.cpp`) envoie exactement le même format et les mêmes valeurs que `bin/simulate-watch.php` : garder les deux en accord. Un envoi = une heure de mission ; éruption solaire aux heures 30–36 (`FLARE_DOSE` : la dose de radiation monte jusqu'au critique, dans les deux simulateurs).
  En plus, le C++ seul a : `checkOnBoard()` (alerte locale), une file `std::deque` (un paquet n'en sort qu'après un 201 ; erreur réseau/5xx → on réessaie au prochain envoi ; autre 4xx → abandonné) et `--outage=A:B` (liaison coupée des heures A à B). Ses seuils (`VITALS`) et `computeStatus()` doivent rester identiques à ceux de `data.json` / `db.php`.
- `aura_compute_status()` (`src/db.php`) et `computeStatus()` (`frontend.js`) appliquent la même règle : les modifier ensemble.

## Fonctionnement de `frontend.js`
- `DATA_SOURCES = ['/api/telemetry', 'data.json']` : le premier qui répond est utilisé.
- Deux modes : `scenario` (données de démo) et `live` (mesures `source: 'watch'`). Avec le backend, l'API est interrogée toutes les 5 s (`LIVE_POLL_MS`). Le mode `live` n'est utilisé que si la dernière mesure de la montre a moins de 2 min (`LIVE_FRESH_MS`, comparée à l'heure du serveur `meta.generated_at`) : d'anciennes mesures de test restées dans la base ne remplacent donc pas les valeurs de démo. Le replay de l'éruption solaire utilise toujours le scénario ; « Restore » revient au live s'il est encore récent. La pastille `#mode-pill` en haut indique le mode affiché.
- Une carte sans mesure (la montre n'a pas encore envoyé cette constante) affiche `--` et `NO DATA`.
- Courbes (sparklines) : ligne grise sur les 12 dernières mesures, et chaque segment touchant une mesure hors seuil est redessiné en orange/rouge (`drawSparkline()`, statut de la carte à chaque instant : la tension utilise systolique ET diastolique). Un pic reste donc coloré tant qu'il est visible dans la courbe.
- Pastille de statut sur les boutons MALE / FEMALE (`updateProfileDots()`) : pire statut de toutes les constantes de cet astronaute, radiation comprise. L'infobulle liste les problèmes.
- Le « decision log » du replay donne la priorité aux changements de l'astronaute affiché.
- Panneau « Live Watch Simulator » sous le bouton Solar Flare (seulement avec le backend) : choix de la crise + bouton Run/Stop, état rafraîchi à chaque `pollLive()`. Lancer le simulateur pendant le replay arrête d'abord le replay.
- Panneau « Module 02 // Alert Archive » (seulement avec le backend) : uniquement l'astronaute affiché (l'en-tête résume les alertes ouvertes de l'autre) ; alertes `watch` en mode live, alertes `scenario` déjà survenues à l'heure affichée sinon. En mode scénario, `alertView()` recalcule pic, dernière valeur et nombre de mesures à `currentTime`, pour que le replay n'affiche jamais de valeurs futures. Le bouton du dialogue d'une carte accuse réception de l'alerte ouverte de cette carte.
- `CARD_MAP` relie chaque carte de `index.html` (`data-metric-id`) aux constantes et au point chaud de la silhouette. La tension artérielle combine `systolic_bp` et `diastolic_bp`.
- 7 cartes : fréquence cardiaque, tension, glycémie, température, exercice (`bone_load`), sommeil, radiation. La carte radiation (`radiation_dose`, classe `vital-card-wide`) occupe toute la dernière ligne de la grille.
- `EXTRA_VITALS` : constantes sans carte qui comptent quand même pour la jauge et les points chauds (vide aujourd'hui).
- Statut d'une carte = le pire statut de ses constantes. Point chaud = le pire statut des cartes qui le partagent.
- Jauge « Systemic Reserve » = moyenne des scores (nominal 100, warning 65, critical 30).
- Le bouton homme/femme change l'astronaute affiché (M → AST-02, F → AST-01).
- Le replay avance d'une heure simulée toutes les 700 ms (`REPLAY_STEP_MS`) et écrit chaque changement de statut dans le « decision log ».
- Les ids HTML suivent le motif `val-<id>`, `ref-<id>`, `status-<id>`. Tout ajout de carte doit les respecter et être ajouté à `CARD_MAP`.

## Filtres
Catégories des filtres : `cardio`, `metabolic`, `immuno`, `neuro`, `musculo`. Un élément peut en avoir plusieurs, séparées par des espaces (`data-category="immuno metabolic"`) : c'est le cas du point chaud IMMUNO·04, qui regroupe glycémie, température et radiation. Le filtre `immuno` montre les cartes température et radiation.

## À savoir
- Les valeurs écrites en dur dans `index.html` ne sont que des valeurs de départ : le JS les remplace.

## Conventions
- Code et textes de l'interface en anglais ; on peut discuter avec l'équipe en français.
- Pas de dépendances externes à part Google Fonts.
- Garder le style visuel existant (variables CSS de `:root`, polices Newsreader / Plus Jakarta Sans / Space Mono).
