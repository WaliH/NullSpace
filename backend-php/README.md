# AuraWatch — backend PHP (Windows)

Le backend reçoit les mesures envoyées par la montre, les enregistre dans une base SQLite et les renvoie au tableau de bord. Il sert aussi le site lui-même : un seul serveur à lancer.

Aucune installation de MySQL, Apache ni Composer n'est nécessaire : PHP suffit.

## 1. Installer PHP (une seule fois)

**Option A — XAMPP (le plus simple si vous l'avez déjà)**
Rien à faire : les scripts `.bat` trouvent automatiquement `C:\xampp\php\php.exe`.

**Option B — PHP seul**
1. Sur https://windows.php.net/download/, téléchargez le zip **PHP 8.x, x64 Non Thread Safe** (VS16 ou VS17 selon la version). Si PHP signale une DLL manquante (`VCRUNTIME140.dll`), installez le « Visual C++ Redistributable x64 » de Microsoft.
2. Décompressez-le dans `C:\php`.
3. Ajoutez `C:\php` à la variable d'environnement **Path** (Démarrer → « Modifier les variables d'environnement système » → Variables d'environnement → Path → Modifier → Nouveau).
4. Dans `C:\php`, copiez `php.ini-development` et renommez la copie en `php.ini`.
5. Ouvrez `php.ini` et retirez le `;` au début de ces lignes :
   ```
   extension_dir = "ext"
   extension=pdo_sqlite
   ```
6. Ouvrez un **nouveau** terminal et vérifiez : `php -v` puis `php -m` (la liste doit contenir `pdo_sqlite`).

## 2. Lancer le serveur

Double-cliquez sur **`start-server.bat`** (ou lancez-le dans le terminal de VS Code).

- Au premier lancement, il crée la base `data\aurawatch.sqlite` à partir de `frontend\data.json`.
- Tableau de bord : http://localhost:8080
- Vérification de l'API : http://localhost:8080/api/health

Arrêt : `Ctrl+C` dans la fenêtre du serveur.

Équivalent en ligne de commande (PowerShell ou cmd, depuis `backend-php`) :
```
php bin\seed.php
php -S localhost:8080 -t ..\frontend router.php
```

## 3. Simuler la montre (en attendant le matériel)

Avec le serveur lancé, double-cliquez sur **`simulate-watch.bat`**. Il envoie toutes les 5 secondes des mesures pour les deux astronautes. Le tableau de bord passe en mode **live** et se met à jour tout seul.

Options (depuis un terminal) :
```
simulate-watch.bat --crisis            la fréquence cardiaque et la tension de Bravo montent jusqu'au seuil critique
simulate-watch.bat --interval=2        un envoi toutes les 2 secondes
simulate-watch.bat --count=10          s'arrête après 10 envois
```

Pour revenir aux seules données de démonstration : `seed.bat --reset` (efface les mesures de la montre).

## 4. L'API

| Méthode | URL | Rôle |
|---|---|---|
| GET | `/api/health` | état du serveur et nombre de mesures |
| GET | `/api/telemetry` | toutes les données, **même format que `data.json`** (+ champ `source` : `scenario` ou `watch`) |
| POST | `/api/readings` | la montre envoie ses mesures |

Filtres possibles sur `/api/telemetry` : `?astronaut_id=AST-01`, `?since=2026-11-14T12:00:00Z`, `?source=watch`.

### Envoi de mesures par la montre

En-tête obligatoire : `X-Device-Key: aura-demo-key` (modifiable dans `config.php` ou par la variable d'environnement `AURA_DEVICE_KEY`).

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
- `recorded_at` est facultatif : sans lui, l'heure du serveur (UTC) est utilisée.
- Le serveur calcule lui-même le statut (`nominal` / `warning` / `critical`) avec les seuils de la table `vitals`. C'est la même règle que `computeStatus()` dans `frontend.js`.
- Réponse `201` : les mesures enregistrées et la liste `alerts` (mesures hors seuils, avec la contre-mesure à appliquer).

Test rapide dans **PowerShell** :
```powershell
$body = '{"astronaut_id":"AST-02","readings":[{"vital_id":"heart_rate","value":120}]}'
Invoke-RestMethod -Method Post -Uri http://localhost:8080/api/readings `
  -Headers @{ "X-Device-Key" = "aura-demo-key" } -ContentType "application/json" -Body $body
```

## 5. Organisation des fichiers

```
backend-php/
  start-server.bat       lance le serveur (crée la base au premier lancement)
  seed.bat               ré-importe frontend\data.json (--reset efface aussi les mesures de la montre)
  simulate-watch.bat     simulateur de montre
  config.php             chemin de la base, clé de la montre
  router.php             point d'entrée du serveur PHP : /api/... → API, le reste → dossier frontend
  src/db.php             base SQLite, création des tables, calcul des statuts
  src/api.php            les routes de l'API
  bin/seed.php           import de data.json
  bin/simulate-watch.php simulateur de montre
  data/                  la base aurawatch.sqlite (non versionnée)
```

## Problèmes fréquents

- **« PHP was not found »** : PHP n'est pas dans le Path, ou le terminal a été ouvert avant la modification. Fermez puis rouvrez VS Code.
- **« pdo_sqlite is not enabled »** : voir l'étape 1.5. La commande `php --ini` indique quel `php.ini` est utilisé.
- **Le port 8080 est déjà pris** : remplacez `8080` par `8090` dans `start-server.bat` (et utilisez `--url=http://localhost:8090` pour le simulateur).
- **Le pare-feu Windows demande une autorisation** : acceptez pour les réseaux privés. C'est nécessaire si la vraie montre envoie ses données depuis un autre appareil. Dans ce cas, lancez le serveur avec `php -S 0.0.0.0:8080 ...` et utilisez l'adresse IP du PC.

Les données sont **simulées** et les seuils sont **illustratifs** : ce ne sont pas des limites officielles de la NASA.
