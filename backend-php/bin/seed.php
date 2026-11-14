<?php
// Creates the SQLite database and imports frontend/data.json (astronauts, vitals, scenario readings).
//
//   php bin/seed.php           → import (keeps readings already sent by the watch)
//   php bin/seed.php --reset   → delete everything, including watch readings, then import

require_once __DIR__ . '/../src/db.php';

$reset = in_array('--reset', $argv, true);
$config = aura_config();

$json = @file_get_contents($config['seed_path']);
if ($json === false) {
    fwrite(STDERR, "Cannot read {$config['seed_path']}\n");
    exit(1);
}
$data = json_decode($json, true);
if (!is_array($data) || !isset($data['astronauts'], $data['vitals'], $data['readings'])) {
    fwrite(STDERR, "data.json does not have the expected shape (astronauts, vitals, readings).\n");
    exit(1);
}

try {
    $pdo = aura_db();
} catch (RuntimeException $e) {
    fwrite(STDERR, $e->getMessage() . "\n");
    exit(1);
}

$pdo->beginTransaction();

if ($reset) {
    $pdo->exec('DELETE FROM readings');
} else {
    $pdo->exec("DELETE FROM readings WHERE source = 'scenario'");
}
$pdo->exec('DELETE FROM meta');

$meta = $pdo->prepare('INSERT INTO meta (key, value) VALUES (?, ?)');
foreach (($data['meta'] ?? []) as $k => $v) {
    $meta->execute([$k, is_scalar($v) ? (string) $v : json_encode($v)]);
}

$astro = $pdo->prepare(
    'INSERT INTO astronauts (astronaut_id, display_name, sex, role, mission) VALUES (?, ?, ?, ?, ?)
     ON CONFLICT(astronaut_id) DO UPDATE SET display_name = excluded.display_name, sex = excluded.sex,
         role = excluded.role, mission = excluded.mission'
);
foreach ($data['astronauts'] as $a) {
    $astro->execute([$a['astronaut_id'], $a['display_name'], $a['sex'], $a['role'] ?? null, $a['mission'] ?? null]);
}

$vital = $pdo->prepare(
    'INSERT INTO vitals (vital_id, zone, label, unit, critical_low, warning_low, warning_high, critical_high,
                         nominal_message, countermeasure, threshold_source)
     VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
     ON CONFLICT(vital_id) DO UPDATE SET zone = excluded.zone, label = excluded.label, unit = excluded.unit,
         critical_low = excluded.critical_low, warning_low = excluded.warning_low,
         warning_high = excluded.warning_high, critical_high = excluded.critical_high,
         nominal_message = excluded.nominal_message, countermeasure = excluded.countermeasure,
         threshold_source = excluded.threshold_source'
);
foreach ($data['vitals'] as $v) {
    $vital->execute([
        $v['vital_id'], $v['zone'], $v['label'], $v['unit'],
        $v['critical_low'], $v['warning_low'], $v['warning_high'], $v['critical_high'],
        $v['nominal_message'] ?? null, $v['countermeasure'] ?? null, $v['threshold_source'] ?? null,
    ]);
}

// Statuses are recomputed from the thresholds so they always match the rule in frontend.js.
$vitals = aura_vitals_by_id($pdo);
$reading = $pdo->prepare(
    "INSERT INTO readings (reading_id, recorded_at, astronaut_id, vital_id, value, status, source)
     VALUES (?, ?, ?, ?, ?, ?, 'scenario')"
);
$mismatch = 0;
foreach ($data['readings'] as $r) {
    $status = aura_compute_status($vitals[$r['vital_id']], (float) $r['value']);
    if (isset($r['status']) && $r['status'] !== $status) {
        $mismatch++;
    }
    $reading->execute([$r['reading_id'], $r['recorded_at'], $r['astronaut_id'], $r['vital_id'], $r['value'], $status]);
}

$pdo->commit();

$watch = (int) $pdo->query("SELECT COUNT(*) FROM readings WHERE source = 'watch'")->fetchColumn();
echo "Database: {$config['db_path']}\n";
echo count($data['astronauts']) . " astronauts, " . count($data['vitals']) . " vitals, "
    . count($data['readings']) . " scenario readings imported.\n";
echo "Watch readings kept: $watch" . ($reset ? " (reset)" : "") . "\n";
if ($mismatch) {
    echo "Note: $mismatch readings had a status in data.json that did not match the thresholds; recomputed.\n";
}
