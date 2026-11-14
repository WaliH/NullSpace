<?php
// Alerts: created automatically when a reading is out of its thresholds,
// then acknowledged by mission control from the dashboard.
//
// One alert = one problem (astronaut + vital), not one reading: while an alert is still
// open, new out-of-range readings for the same vital update it instead of creating a new one.
// That way the watch, which sends every few seconds, does not flood the list.

require_once __DIR__ . '/db.php';

const AURA_STATUS_RANK = ['nominal' => 0, 'warning' => 1, 'critical' => 2];

function aura_create_alerts_schema(PDO $pdo): void
{
    $pdo->exec("
        CREATE TABLE IF NOT EXISTS alerts (
            id               INTEGER PRIMARY KEY AUTOINCREMENT,
            astronaut_id     TEXT NOT NULL REFERENCES astronauts(astronaut_id),
            vital_id         TEXT NOT NULL REFERENCES vitals(vital_id),
            source           TEXT NOT NULL,              -- 'scenario' or 'watch', like readings.source
            status           TEXT NOT NULL CHECK (status IN ('warning', 'critical')),  -- worst status seen
            first_reading_id TEXT NOT NULL,
            first_value      REAL NOT NULL,
            recorded_at      TEXT NOT NULL,              -- time of the first out-of-range reading
            peak_value       REAL NOT NULL,              -- value of the latest reading with the worst status
            last_value       REAL NOT NULL,
            last_status      TEXT NOT NULL,              -- status of the latest reading (can be back to 'nominal')
            last_seen_at     TEXT NOT NULL,
            readings_count   INTEGER NOT NULL DEFAULT 1, -- out-of-range readings grouped in this alert
            countermeasure   TEXT,
            acknowledged_at  TEXT,                       -- NULL = still open
            acknowledged_by  TEXT,
            ack_note         TEXT
        );

        CREATE INDEX IF NOT EXISTS idx_alerts_open
            ON alerts (astronaut_id, vital_id, source, acknowledged_at);
    ");
}

// To call for every new reading (watch readings and seeded scenario readings).
// Returns the alert id when the reading created or updated an alert, null otherwise.
function aura_track_alert(PDO $pdo, array $reading, array $vital): ?int
{
    $find = $pdo->prepare(
        'SELECT * FROM alerts
         WHERE astronaut_id = ? AND vital_id = ? AND source = ? AND acknowledged_at IS NULL
         ORDER BY id DESC LIMIT 1'
    );
    $find->execute([$reading['astronaut_id'], $reading['vital_id'], $reading['source']]);
    $open = $find->fetch();

    if ($open) {
        // Same problem still open: keep its history up to date, even when the value is back to nominal.
        $isPeak = AURA_STATUS_RANK[$reading['status']] >= AURA_STATUS_RANK[$open['status']];
        $update = $pdo->prepare(
            'UPDATE alerts SET status = ?, peak_value = ?, last_value = ?, last_status = ?, last_seen_at = ?,
                    readings_count = readings_count + ?
             WHERE id = ?'
        );
        $update->execute([
            $isPeak ? $reading['status'] : $open['status'],
            $isPeak ? $reading['value'] : $open['peak_value'],
            $reading['value'], $reading['status'], $reading['recorded_at'],
            $reading['status'] === 'nominal' ? 0 : 1, $open['id'],
        ]);
        return (int) $open['id'];
    }

    if ($reading['status'] === 'nominal') {
        return null;
    }

    $insert = $pdo->prepare(
        'INSERT INTO alerts (astronaut_id, vital_id, source, status, first_reading_id, first_value, recorded_at,
                             peak_value, last_value, last_status, last_seen_at, countermeasure)
         VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)'
    );
    $insert->execute([
        $reading['astronaut_id'], $reading['vital_id'], $reading['source'], $reading['status'],
        $reading['reading_id'], $reading['value'], $reading['recorded_at'],
        $reading['value'], $reading['value'], $reading['status'], $reading['recorded_at'], $vital['countermeasure'],
    ]);
    return (int) $pdo->lastInsertId();
}

function aura_format_alert(array $row): array
{
    foreach (['first_value', 'peak_value', 'last_value'] as $k) {
        $row[$k] = aura_number($row[$k]);
    }
    $row['id'] = (int) $row['id'];
    $row['readings_count'] = (int) $row['readings_count'];
    $row['acknowledged'] = $row['acknowledged_at'] !== null;
    return $row;
}

function aura_list_alerts(PDO $pdo, array $filters): array
{
    $where = [];
    $params = [];
    $state = $filters['state'] ?? 'all';
    if ($state === 'open') {
        $where[] = 'a.acknowledged_at IS NULL';
    } elseif ($state === 'acknowledged') {
        $where[] = 'a.acknowledged_at IS NOT NULL';
    } elseif ($state !== 'all') {
        throw new InvalidArgumentException("state must be open, acknowledged or all (got '$state').");
    }
    foreach (['astronaut_id', 'source'] as $k) {
        if (!empty($filters[$k])) {
            $where[] = "a.$k = :$k";
            $params[":$k"] = $filters[$k];
        }
    }
    $limit = max(1, min(500, (int) ($filters['limit'] ?? 200)));

    $sql = 'SELECT a.*, v.label AS vital_label, v.unit, s.display_name
            FROM alerts a
            JOIN vitals v ON v.vital_id = a.vital_id
            JOIN astronauts s ON s.astronaut_id = a.astronaut_id'
        . ($where ? ' WHERE ' . implode(' AND ', $where) : '')
        . " ORDER BY a.recorded_at DESC, a.id DESC LIMIT $limit";
    $stmt = $pdo->prepare($sql);
    $stmt->execute($params);
    return array_map('aura_format_alert', $stmt->fetchAll());
}

function aura_get_alert(PDO $pdo, int $id): ?array
{
    $stmt = $pdo->prepare(
        'SELECT a.*, v.label AS vital_label, v.unit, s.display_name
         FROM alerts a
         JOIN vitals v ON v.vital_id = a.vital_id
         JOIN astronauts s ON s.astronaut_id = a.astronaut_id
         WHERE a.id = ?'
    );
    $stmt->execute([$id]);
    $row = $stmt->fetch();
    return $row ? aura_format_alert($row) : null;
}
