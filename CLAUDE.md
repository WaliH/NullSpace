# AuraWatch — contexte projet pour Claude Code

## Le projet
Équipe au hackathon **NASA Space Apps Challenge 2026**.
Sujet : logiciel de suivi de santé pour astronautes en mission spatiale.
Concept : une montre (AuraWatch) mesure les constantes de l'astronaute et les envoie vers la Terre ; toute la surveillance se fait sur un site web (tableau de bord « AURA // HRP·OS »).

Toutes les données sont **simulées** pour la démo. Les seuils sont **illustratifs**, ce ne sont **pas** des limites officielles NASA — ne jamais les présenter comme tels.

## Structure du dépôt (github.com/WaliH/NullSpace)
```
frontend/
  index.html     → page du tableau de bord (cartes de constantes + silhouette interactive + modale)
  frontend.css   → styles (thème sombre « verre », variables CSS dans :root)
  frontend.js    → toute la logique : chargement des données, statuts, rendu, replay
  data.json      → données simulées (doit rester À CÔTÉ de index.html)
backend-php/     → futur backend PHP (vide pour l'instant) : recevra les mesures de la montre
                   et servira une API qui renvoie le même format JSON que data.json
sensor-cpp/      → futur code C++ de la montre / des capteurs (vide pour l'instant)
```
Le frontend est 100 % statique, sans framework ni build, JavaScript vanilla.

## Lancer le site
`frontend.js` charge `data.json` avec `fetch`, donc le site doit être servi en HTTP (pas en `file://`) :
```
cd frontend
python -m http.server 8000
```
puis ouvrir http://localhost:8000 (ou utiliser l'extension Live Server de VS Code sur `frontend/index.html`).

## Données (`data.json`)
- `meta` : infos du projet et avertissement « données simulées ».
- `astronauts` : `AST-01` Astronaut Alpha (F, Commander), `AST-02` Astronaut Bravo (M, Flight Engineer).
- `vitals` : une ligne par constante, avec `zone`, `unit`, `critical_low`, `warning_low`, `warning_high`, `critical_high`, `nominal_message`, `countermeasure`.
  Constantes : `heart_rate`, `systolic_bp`, `diastolic_bp`, `blood_glucose`, `body_temp`, `sleep_hours`, `bone_load`, `radiation_dose`.
- `readings` : mesures horaires `{reading_id, recorded_at (ISO UTC), astronaut_id, vital_id, value, status}`.
  - Jour 1 (2026-11-14) = référence nominale (on affiche la dernière heure du jour 1).
  - Dernier jour (2026-11-15) = scénario d'éruption solaire (SPE), rejoué par le bouton « Simulate Solar Flare ».
- Sources d'origine (hors de ce dossier) : `AuraWatch_data.xlsx`, `schema.sql`, `seed.sql`. `computeStatus()` dans le JS applique la même règle que la vue SQL `v_readings_status` : les deux doivent rester identiques.

## Fonctionnement de `frontend.js`
- `DATA_URL = 'data.json'`. Plus tard, le remplacer par l'URL de l'API backend qui renvoie **le même format JSON**.
- `CARD_MAP` relie chaque carte de `index.html` (`data-metric-id`) aux constantes et au point chaud de la silhouette. La tension artérielle combine `systolic_bp` et `diastolic_bp`.
- `EXTRA_VITALS` : constantes sans carte qui comptent quand même pour la jauge et les points chauds (`radiation_dose`).
- Statut d'une carte = le pire statut de ses constantes. Point chaud = le pire statut des cartes qui le partagent.
- Jauge « Systemic Reserve » = moyenne des scores (nominal 100, warning 65, critical 30).
- Le bouton homme/femme change l'astronaute affiché (M → AST-02, F → AST-01).
- Le replay avance d'une heure simulée toutes les 700 ms (`REPLAY_STEP_MS`) et écrit chaque changement de statut dans le « decision log ».
- Les ids HTML suivent le motif `val-<id>`, `ref-<id>`, `status-<id>`. Tout ajout de carte doit les respecter et être ajouté à `CARD_MAP`.

## Points à vérifier / incohérences connues
- La carte glycémie a `data-category="neuro"` et la carte température `data-category="cardio"`, alors que leurs zones dans les données sont `nutrition` et `immune_radiation`. Cela joue sur les filtres.
- Le texte par défaut du terminal dans `index.html` parle de « 6 crew nodes », mais les données ne contiennent que 2 astronautes (le JS le remplace au chargement).
- Les valeurs écrites en dur dans `index.html` ne sont que des valeurs de départ : le JS les remplace.

## Conventions
- Code et textes de l'interface en anglais ; on peut discuter avec l'équipe en français.
- Pas de dépendances externes à part Google Fonts.
- Garder le style visuel existant (variables CSS de `:root`, polices Newsreader / Plus Jakarta Sans / Space Mono).
