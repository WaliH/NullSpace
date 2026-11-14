<?php
// HTTP API of the AuraWatch backend.
//
//   GET  /api/health      → server and database check
//   GET  /api/telemetry   → same JSON shape as frontend/data.json
//                           optional filters: ?astronaut_id=AST-01&since=2026-11-14T00:00:00Z&source=watch
//   POST /api/readings    → the watch sends its measurements (header X-Device-Key required)

require_once __DIR__ . '/db.php';

function aura_send_json(int $code, $payload): void
{
    http_response_code($code);
    header('Content-Type: application/json; charset=utf-8');
    echo json_encode($payload, JSON_PRETTY_PRINT | JSON_UNESCAPED_SLASHES | JSON_UNESCAPED_UNICODE);
}

function aura_error(int $code, string $message): void
{
    aura_send_json($code, ['error' => $message]);
}

function aura_handle_api(string $method, string $path): void
{
    // Lets the frontend call the API even when it is served from another port.
    header('Access-Control-Allow-Origin: *');
    header('Access-Control-Allow-Methods: GET, POST, OPTIONS');
    header('Access-Control-Allow-Headers: Content-Type, X-Device-Key');

    if ($method === 'OPTIONS') {
        http_response_code(204);
        return;
    }

    try {
        $route = $method . ' ' . rtrim($path, '/');
        switch ($route) {
            case 'GET /api/health':
                aura_api_health();
                break;
            case 'GET /api/telemetry':
                aura_api_telemetry();
                break;
            case 'POST /api/readings':
                aura_api_post_readings();
                break;
            default:
                aura_error(404, "Unknown route: $method $path");
        }
    } catch (InvalidArgumentException $e) {
        aura_error(400, $e->getMessage());
    } catch (Throwable $e) {
        aura_error(500, $e->getMessage());
    }
}

function aura_api_health(): void
{
    $pdo = aura_db();
    $count = (int) $pdo->query('SELECT COUNT(*) FROM readings')->fetchColumn();
    $watch = (int) $pdo->query("SELECT COUNT(*) FROM readings WHERE source = 'watch'")->fetchColumn();
    aura_send_json(200, [
        'ok' => true,
        'php' => PHP_VERSION,
        'os' => PHP_OS_FAMILY,
        'readings' => $count,
        'watch_readings' => $watch,
        'seeded' => $count > 0,
    ]);
}

function aura_api_telemetry(): void
{
    $pdo = aura_db();

    $meta = [];
    foreach ($pdo->query('SELECT key, value FROM meta') as $row) {
        $meta[$row['key']] = $row['value'];
    }
    $meta['generated_at'] = gmdate('Y-m-d\TH:i:s\Z');

    $astronauts = $pdo->query('SELECT * FROM astronauts ORDER BY astronaut_id')->fetchAll();
    $vitals = array_values(aura_vitals_by_id($pdo));

    $where = [];
    $params = [];
    if (!empty($_GET['astronaut_id'])) {
        $where[] = 'astronaut_id = :astronaut_id';
        $params[':astronaut_id'] = $_GET['astronaut_id'];
    }
    if (!empty($_GET['since'])) {
        $where[] = 'recorded_at >= :since';
        $params[':since'] = aura_normalize_time($_GET['since']);
    }
    if (!empty($_GET['source'])) {
        $where[] = 'source = :source';
        $params[':source'] = $_GET['source'];
    }
    $sql = 'SELECT reading_id, recorded_at, astronaut_id, vital_id, value, status, source FROM readings'
        . ($where ? ' WHERE ' . implode(' AND ', $where) : '')
        . ' ORDER BY recorded_at, id';
    $stmt = $pdo->prepare($sql);
    $stmt->execute($params);

    $readings = [];
    foreach ($stmt as $row) {
        $row['value'] = aura_number($row['value']);
        $readings[] = $row;
    }

    aura_send_json(200, [
        'meta' => $meta,
        'astronauts' => $astronauts,
        'vitals' => $vitals,
        'readings' => $readings,
    ]);
}

// Accepts "2026-11-14T12:00:00Z", "2026-11-14 12:00:00", "2026-11-14T13:00:00+01:00"...
// and always stores UTC in the format used by data.json.
function aura_normalize_time(string $value): string
{
    try {
        $dt = new DateTimeImmutable($value, new DateTimeZone('UTC'));
    } catch (Exception $e) {
        throw new InvalidArgumentException("Invalid date: $value");
    }
    return $dt->setTimezone(new DateTimeZone('UTC'))->format('Y-m-d\TH:i:s\Z');
}

// Body expected from the watch:
// {
//   "astronaut_id": "AST-02",
//   "recorded_at": "2026-11-14T12:00:00Z",        (optional: server time if missing)
//   "readings": [ { "vital_id": "heart_rate", "value": 72 }, ... ]
// }
function aura_api_post_readings(): void
{
    $key = $_SERVER['HTTP_X_DEVICE_KEY'] ?? '';
    if (!hash_equals(aura_config()['device_key'], $key)) {
        aura_error(401, 'Missing or wrong X-Device-Key header.');
        return;
    }

    $body = json_decode(file_get_contents('php://input'), true);
    if (!is_array($body)) {
        aura_error(400, 'The body must be JSON.');
        return;
    }

    $pdo = aura_db();
    $astronautId = $body['astronaut_id'] ?? '';
    $exists = $pdo->prepare('SELECT 1 FROM astronauts WHERE astronaut_id = ?');
    $exists->execute([$astronautId]);
    if (!$exists->fetchColumn()) {
        aura_error(422, "Unknown astronaut_id: '$astronautId'. Run bin/seed.php first if the database is empty.");
        return;
    }

    try {
        $recordedAt = aura_normalize_time($body['recorded_at'] ?? 'now');
    } catch (InvalidArgumentException $e) {
        aura_error(422, $e->getMessage());
        return;
    }

    $items = $body['readings'] ?? null;
    if (!is_array($items) || !$items) {
        aura_error(422, '"readings" must be a non-empty list of {vital_id, value}.');
        return;
    }

    $vitals = aura_vitals_by_id($pdo);
    foreach ($items as $i => $item) {
        $vitalId = $item['vital_id'] ?? '';
        if (!isset($vitals[$vitalId])) {
            aura_error(422, "readings[$i]: unknown vital_id '$vitalId'.");
            return;
        }
        if (!isset($item['value']) || !is_numeric($item['value'])) {
            aura_error(422, "readings[$i]: value must be a number.");
            return;
        }
    }

    $insert = $pdo->prepare(
        "INSERT INTO readings (recorded_at, astronaut_id, vital_id, value, status, source)
         VALUES (?, ?, ?, ?, ?, 'watch')"
    );
    $setId = $pdo->prepare('UPDATE readings SET reading_id = ? WHERE id = ?');

    $saved = [];
    $alerts = [];
    $pdo->beginTransaction();
    foreach ($items as $item) {
        $vital = $vitals[$item['vital_id']];
        $value = (float) $item['value'];
        $status = aura_compute_status($vital, $value);
        $insert->execute([$recordedAt, $astronautId, $vital['vital_id'], $value, $status]);
        $id = (int) $pdo->lastInsertId();
        $readingId = sprintf('W%06d', $id);
        $setId->execute([$readingId, $id]);

        $row = [
            'reading_id' => $readingId,
            'recorded_at' => $recordedAt,
            'astronaut_id' => $astronautId,
            'vital_id' => $vital['vital_id'],
            'value' => aura_number($value),
            'status' => $status,
        ];
        $saved[] = $row;
        if ($status !== 'nominal') {
            $alerts[] = $row + ['countermeasure' => $vital['countermeasure']];
        }
    }
    $pdo->commit();

    aura_send_json(201, ['saved' => $saved, 'alerts' => $alerts]);
}
