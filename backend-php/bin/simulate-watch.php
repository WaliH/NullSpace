<?php
// Watch simulator: sends fake measurements to the API, like the real AuraWatch will.
// Useful for the demo while the hardware (sensor-cpp/) is not ready.
//
//   php bin/simulate-watch.php                 → one batch every 5 s, nominal values
//   php bin/simulate-watch.php --crisis        → Bravo: heart rate and blood pressure drift up to critical
//   php bin/simulate-watch.php --crisis=alpha  → Alpha: fever (temperature up to critical, heart rate up)
//   php bin/simulate-watch.php --crisis=all    → both at the same time
//   php bin/simulate-watch.php --interval=2 --count=10 --url=http://localhost:8787
//
// One batch = one mission hour. Solar eruption (radiation) during batches 30-36.
//
// Stop with Ctrl+C. Needs only PHP (no curl extension required).

$config = require __DIR__ . '/../config.php';

$opts = getopt('', ['url::', 'interval::', 'count::', 'crisis::']);
$baseUrl = rtrim($opts['url'] ?? ('http://localhost:' . $config['port']), '/');
$interval = max(1, (int) ($opts['interval'] ?? 5));
$count = (int) ($opts['count'] ?? 0); // 0 = run until Ctrl+C
// --crisis alone (getopt gives false) means Bravo.
$crisisArg = array_key_exists('crisis', $opts) ? ($opts['crisis'] === false ? 'bravo' : strtolower($opts['crisis'])) : '';
if ($crisisArg !== '' && !in_array($crisisArg, ['alpha', 'bravo', 'all'], true)) {
    fwrite(STDERR, "Unknown crisis '$crisisArg': use --crisis, --crisis=alpha or --crisis=all\n");
    exit(1);
}
$crisisAlpha = in_array($crisisArg, ['alpha', 'all'], true); // AST-01: fever
$crisisBravo = in_array($crisisArg, ['bravo', 'all'], true); // AST-02: heart rate + blood pressure

// Starting values, close to the nominal baseline of data.json.
$state = [
    'AST-01' => ['heart_rate' => 68, 'systolic_bp' => 118, 'diastolic_bp' => 75, 'blood_glucose' => 90, 'body_temp' => 36.8],
    'AST-02' => ['heart_rate' => 72, 'systolic_bp' => 124, 'diastolic_bp' => 79, 'blood_glucose' => 94, 'body_temp' => 36.9],
];
// Daily values, sent with the first batch only.
$daily = [
    'AST-01' => ['sleep_hours' => 7.3, 'bone_load' => 75, 'radiation_dose' => 0.6],
    'AST-02' => ['sleep_hours' => 7.6, 'bone_load' => 80, 'radiation_dose' => 0.62],
];
// Solar eruption during batches 30-36 (one batch = one mission hour): radiation added to the
// daily dose each hour (warning above 1 mSv/day, critical above 2).
const FLARE_START = 30;
const FLARE_END = 36;
const FLARE_DOSE = [0.10, 0.22, 0.35, 0.42, 0.35, 0.22, 0.14];
$dose = array_map(fn($d) => $d['radiation_dose'], $daily);
// Allowed range for the random walk in nominal mode.
$bounds = [
    'heart_rate' => [60, 85, 2], 'systolic_bp' => [105, 130, 2], 'diastolic_bp' => [65, 82, 1.5],
    'blood_glucose' => [80, 98, 2], 'body_temp' => [36.5, 37.3, 0.1],
];

function walk(float $v, array $b): float
{
    [$min, $max, $step] = $b;
    $v += (mt_rand() / mt_getrandmax() * 2 - 1) * $step;
    return max($min, min($max, $v));
}

function post_json(string $url, array $payload, string $key): array
{
    $ctx = stream_context_create(['http' => [
        'method' => 'POST',
        'header' => "Content-Type: application/json\r\nX-Device-Key: $key\r\n",
        'content' => json_encode($payload),
        'ignore_errors' => true,
        'timeout' => 10,
    ]]);
    $body = @file_get_contents($url, false, $ctx);
    if ($body === false) {
        return [0, null];
    }
    $code = 0;
    if (isset($http_response_header[0]) && preg_match('/\s(\d{3})\s/', $http_response_header[0], $m)) {
        $code = (int) $m[1];
    }
    return [$code, json_decode($body, true)];
}

$mode = $crisisAlpha && $crisisBravo ? ' (crisis: Alpha fever + Bravo cardiovascular)'
    : ($crisisAlpha ? ' (crisis: Alpha fever)' : ($crisisBravo ? ' (crisis: Bravo cardiovascular)' : ''));
echo "Sending to $baseUrl/api/readings every {$interval}s$mode. Ctrl+C to stop.\n";

for ($i = 0; $count === 0 || $i < $count; $i++) {
    $time = gmdate('Y-m-d\TH:i:s\Z');
    foreach ($state as $astro => &$values) {
        foreach ($values as $vital => &$v) {
            $v = walk($v, $bounds[$vital]);
        }
        unset($v);
        // Crisis scenarios. Same values as sensor-cpp/simulator.cpp.
        if ($crisisBravo && $astro === 'AST-02') {
            $values['heart_rate'] = min(130, 88 + $i * 6);
            $values['systolic_bp'] = min(160, 128 + $i * 5);
        }
        if ($crisisAlpha && $astro === 'AST-01') {
            $values['body_temp'] = min(39.0, 37.2 + $i * 0.25);
            $values['heart_rate'] = min(105, 74 + $i * 3);
        }

        $readings = [];
        foreach ($values as $vital => $v) {
            $readings[] = ['vital_id' => $vital, 'value' => round($v, $vital === 'body_temp' ? 1 : 0)];
        }
        if ($i === 0) {
            foreach ($daily[$astro] as $vital => $v) {
                $readings[] = ['vital_id' => $vital, 'value' => $v];
            }
        }
        // Solar eruption (batch = mission hour 30-36): the daily radiation dose rises every hour.
        // Same values as sensor-cpp/simulator.cpp.
        if ($i >= FLARE_START && $i <= FLARE_END) {
            $dose[$astro] += FLARE_DOSE[$i - FLARE_START];
            $readings[] = ['vital_id' => 'radiation_dose', 'value' => round($dose[$astro], 2)];
        }

        [$code, $res] = post_json("$baseUrl/api/readings", [
            'astronaut_id' => $astro,
            'recorded_at' => $time,
            'readings' => $readings,
        ], $config['device_key']);

        if ($code !== 201) {
            $msg = $res['error'] ?? 'no answer — is the server running? (start-server.bat)';
            echo "[$time] $astro: error $code — $msg\n";
            continue;
        }
        $alerts = array_map(fn($a) => "{$a['vital_id']}={$a['value']} {$a['status']}", $res['alerts']);
        echo "[$time] $astro: " . count($res['saved']) . ' readings'
            . ($alerts ? ' — ALERT: ' . implode(', ', $alerts) : '') . "\n";
    }
    unset($values);

    if ($count === 0 || $i < $count - 1) {
        sleep($interval);
    }
}
