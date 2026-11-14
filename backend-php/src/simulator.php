<?php
// Starts / stops the watch simulator from the dashboard ("Run watch simulator" button).
//
//   GET  /api/simulator        → is it running? which engine, which crisis
//   POST /api/simulator/start  → {"crisis": "all" | "alpha" | "bravo" | "none"}
//   POST /api/simulator/stop
//
// Engine: sensor-cpp/simulator.exe if it has been built (build.bat), otherwise the PHP
// simulator (bin/simulate-watch.php), which sends exactly the same values.
//
// The simulator runs as a separate background process and sends its readings to this same
// server, like the real watch. Its PID is kept in data/simulator.json to stop it later.
// Each run stops by itself after SIM_COUNT batches, so a forgotten run cannot go on forever.
//
// Only allowed from the PC running the server (localhost): a web page must not be able to
// start programs on the machine from elsewhere on the network.

require_once __DIR__ . '/db.php';

const AURA_SIM_COUNT = 40;      // batches per run
const AURA_SIM_INTERVAL = 3;    // seconds between batches → about 2 minutes per run
const AURA_SIM_CRISES = ['all', 'alpha', 'bravo', 'none'];

function aura_sim_state_file(): string
{
    return dirname(aura_config()['db_path']) . DIRECTORY_SEPARATOR . 'simulator.json';
}

function aura_sim_is_windows(): bool
{
    return PHP_OS_FAMILY === 'Windows';
}

function aura_sim_cpp_path(): string
{
    $exe = aura_sim_is_windows() ? 'simulator.exe' : 'simulator';
    return dirname(__DIR__, 2) . DIRECTORY_SEPARATOR . 'sensor-cpp' . DIRECTORY_SEPARATOR . $exe;
}

function aura_sim_request_is_local(): bool
{
    $ip = $_SERVER['REMOTE_ADDR'] ?? '';
    return in_array($ip, ['127.0.0.1', '::1'], true);
}

function aura_sim_pid_running(int $pid): bool
{
    if ($pid <= 0) {
        return false;
    }
    if (aura_sim_is_windows()) {
        $out = (string) shell_exec('tasklist /FI "PID eq ' . $pid . '" /NH 2>NUL');
        return (bool) preg_match('/\b' . $pid . '\b/', $out);
    }
    return file_exists("/proc/$pid");
}

function aura_sim_status(): array
{
    $file = aura_sim_state_file();
    $state = is_file($file) ? json_decode((string) file_get_contents($file), true) : null;
    if (!is_array($state) || !aura_sim_pid_running((int) ($state['pid'] ?? 0))) {
        return [
            'running' => false,
            'cpp_available' => is_file(aura_sim_cpp_path()),
        ];
    }
    return $state + ['running' => true, 'cpp_available' => is_file(aura_sim_cpp_path())];
}

// UTF-16LE text for PowerShell -EncodedCommand. mbstring is often disabled on Windows,
// so fall back to iconv, then to a plain conversion (enough for ASCII paths).
function aura_sim_utf16le(string $s): string
{
    if (function_exists('mb_convert_encoding')) {
        return mb_convert_encoding($s, 'UTF-16LE', 'UTF-8');
    }
    if (function_exists('iconv')) {
        $r = iconv('UTF-8', 'UTF-16LE', $s);
        if ($r !== false) {
            return $r;
        }
    }
    return implode("\0", str_split($s)) . "\0";
}

// Starts $program with $args in the background and returns its PID (0 on failure).
function aura_sim_launch(string $program, array $args): int
{
    if (aura_sim_is_windows()) {
        // PowerShell's Start-Process returns the PID and does not keep the request waiting.
        // Every argument is wrapped in double quotes (paths may contain spaces), and the whole
        // command is base64-encoded so cmd.exe never has to deal with the quotes.
        $q = fn(string $s) => "'" . str_replace("'", "''", $s) . "'";
        $argList = implode(',', array_map(fn($a) => $q('"' . $a . '"'), $args));
        $ps = '$p = Start-Process -FilePath ' . $q($program)
            . ' -ArgumentList ' . $argList
            . ' -WindowStyle Hidden -PassThru; $p.Id';
        $cmd = 'powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -EncodedCommand '
            . base64_encode(aura_sim_utf16le($ps));
        return (int) trim((string) shell_exec($cmd));
    }

    $cmd = 'nohup ' . escapeshellarg($program) . ' '
        . implode(' ', array_map('escapeshellarg', $args))
        . ' > /dev/null 2>&1 & echo $!';
    return (int) trim((string) shell_exec($cmd));
}

function aura_sim_kill(int $pid): void
{
    if ($pid <= 0) {
        return;
    }
    if (aura_sim_is_windows()) {
        shell_exec('taskkill /PID ' . $pid . ' /T /F >NUL 2>&1');
    } else {
        shell_exec('kill ' . $pid . ' 2>/dev/null');
    }
}

function aura_sim_stop(): void
{
    $status = aura_sim_status();
    if ($status['running']) {
        aura_sim_kill((int) $status['pid']);
    }
    @unlink(aura_sim_state_file());
}

function aura_sim_start(string $crisis): array
{
    if (!function_exists('shell_exec')) {
        throw new RuntimeException('shell_exec is disabled in php.ini (disable_functions): the simulator cannot be started from the website.');
    }
    aura_sim_stop();

    $url = 'http://localhost:' . aura_config()['port'];
    $common = ['--count=' . AURA_SIM_COUNT, '--interval=' . AURA_SIM_INTERVAL, '--url=' . $url];
    if ($crisis !== 'none') {
        $common[] = '--crisis=' . $crisis;
    }

    $cpp = aura_sim_cpp_path();
    if (is_file($cpp)) {
        $engine = 'cpp';
        $pid = aura_sim_launch($cpp, $common);
    } else {
        $engine = 'php';
        $script = dirname(__DIR__) . DIRECTORY_SEPARATOR . 'bin' . DIRECTORY_SEPARATOR . 'simulate-watch.php';
        $pid = aura_sim_launch(PHP_BINARY, array_merge([$script], $common));
    }

    if ($pid <= 0) {
        throw new RuntimeException('The simulator could not be started (no process id returned).');
    }

    $state = [
        'pid' => $pid,
        'engine' => $engine,
        'crisis' => $crisis,
        'started_at' => gmdate('Y-m-d\TH:i:s\Z'),
        'ends_after_s' => AURA_SIM_COUNT * AURA_SIM_INTERVAL,
    ];
    file_put_contents(aura_sim_state_file(), json_encode($state));
    return $state + ['running' => true, 'cpp_available' => $engine === 'cpp'];
}

function aura_api_simulator(string $action): void
{
    if ($action !== 'status' && !aura_sim_request_is_local()) {
        aura_error(403, 'The simulator can only be started or stopped from the PC running the server.');
        return;
    }
    if ($action === 'status') {
        aura_send_json(200, aura_sim_status());
        return;
    }
    if ($action === 'stop') {
        aura_sim_stop();
        aura_send_json(200, aura_sim_status());
        return;
    }

    $body = json_decode(file_get_contents('php://input') ?: '{}', true);
    $crisis = strtolower((string) (is_array($body) ? ($body['crisis'] ?? 'all') : 'all'));
    if (!in_array($crisis, AURA_SIM_CRISES, true)) {
        aura_error(422, 'crisis must be one of: ' . implode(', ', AURA_SIM_CRISES));
        return;
    }
    aura_send_json(200, aura_sim_start($crisis));
}
