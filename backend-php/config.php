<?php
// AuraWatch backend configuration.
// Paths use __DIR__ so they work the same on Windows and Linux.

return [
    // SQLite database file (created automatically by bin/seed.php).
    'db_path' => __DIR__ . '/data/aurawatch.sqlite',

    // Seed file imported by bin/seed.php: the same data.json the frontend uses.
    'seed_path' => __DIR__ . '/../frontend/data.json',

    // Port of the local server. Must match PORT in start-server.bat.
    // 8787 avoids 8080, which is often taken by Tomcat (XAMPP), Oracle, Jenkins...
    'port' => (int) (getenv('AURA_PORT') ?: 8787),

    // Key the watch must send in the "X-Device-Key" header to POST readings.
    // Demo value only: change it before showing the project publicly.
    'device_key' => getenv('AURA_DEVICE_KEY') ?: 'aura-demo-key',
];
