<?php
// Database access (SQLite through PDO).

function aura_config(): array
{
    static $config = null;
    if ($config === null) {
        $config = require __DIR__ . '/../config.php';
    }
    return $config;
}

function aura_db(): PDO
{
    static $pdo = null;
    if ($pdo !== null) {
        return $pdo;
    }

    if (!extension_loaded('pdo_sqlite')) {
        throw new RuntimeException(
            'The PHP extension pdo_sqlite is not enabled. On Windows, open php.ini, '
            . 'remove the ";" in front of "extension=pdo_sqlite", save, and restart the server. '
            . 'See backend-php/README.md.'
        );
    }

    $path = aura_config()['db_path'];
    $dir = dirname($path);
    if (!is_dir($dir)) {
        mkdir($dir, 0777, true);
    }

    $pdo = new PDO('sqlite:' . $path);
    $pdo->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);
    $pdo->setAttribute(PDO::ATTR_DEFAULT_FETCH_MODE, PDO::FETCH_ASSOC);
    $pdo->exec('PRAGMA foreign_keys = ON');
    aura_create_schema($pdo);
    return $pdo;
}

function aura_create_schema(PDO $pdo): void
{
    $pdo->exec("
        CREATE TABLE IF NOT EXISTS meta (
            key   TEXT PRIMARY KEY,
            value TEXT
        );

        CREATE TABLE IF NOT EXISTS astronauts (
            astronaut_id TEXT PRIMARY KEY,
            display_name TEXT NOT NULL,
            sex          TEXT NOT NULL CHECK (sex IN ('M', 'F')),
            role         TEXT,
            mission      TEXT
        );

        CREATE TABLE IF NOT EXISTS vitals (
            vital_id         TEXT PRIMARY KEY,
            zone             TEXT NOT NULL,
            label            TEXT NOT NULL,
            unit             TEXT NOT NULL,
            critical_low     REAL,
            warning_low      REAL,
            warning_high     REAL,
            critical_high    REAL,
            nominal_message  TEXT,
            countermeasure   TEXT,
            threshold_source TEXT
        );

        -- source = 'scenario' for the seeded demo data, 'watch' for readings sent by the watch.
        CREATE TABLE IF NOT EXISTS readings (
            id           INTEGER PRIMARY KEY AUTOINCREMENT,
            reading_id   TEXT UNIQUE,
            recorded_at  TEXT NOT NULL,
            astronaut_id TEXT NOT NULL REFERENCES astronauts(astronaut_id),
            vital_id     TEXT NOT NULL REFERENCES vitals(vital_id),
            value        REAL NOT NULL,
            status       TEXT NOT NULL CHECK (status IN ('nominal', 'warning', 'critical')),
            source       TEXT NOT NULL DEFAULT 'scenario'
        );

        CREATE INDEX IF NOT EXISTS idx_readings_lookup
            ON readings (astronaut_id, vital_id, recorded_at);
    ");
}

// Status from the thresholds. Must stay identical to computeStatus() in frontend/frontend.js.
function aura_compute_status(array $vital, float $value): string
{
    if (($vital['critical_low'] !== null && $value < $vital['critical_low'])
        || ($vital['critical_high'] !== null && $value > $vital['critical_high'])) {
        return 'critical';
    }
    if (($vital['warning_low'] !== null && $value < $vital['warning_low'])
        || ($vital['warning_high'] !== null && $value > $vital['warning_high'])) {
        return 'warning';
    }
    return 'nominal';
}

// Returns 64 instead of 64.0 for whole numbers, so the JSON looks like data.json.
function aura_number($value)
{
    if ($value === null) {
        return null;
    }
    $f = (float) $value;
    return (floor($f) == $f && abs($f) < 1e15) ? (int) $f : $f;
}

function aura_vitals_by_id(PDO $pdo): array
{
    $out = [];
    foreach ($pdo->query('SELECT * FROM vitals') as $row) {
        foreach (['critical_low', 'warning_low', 'warning_high', 'critical_high'] as $k) {
            $row[$k] = aura_number($row[$k]);
        }
        $out[$row['vital_id']] = $row;
    }
    return $out;
}
