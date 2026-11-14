<?php
// Entry point for PHP's built-in web server (works the same on Windows and Linux):
//
//   php -S localhost:8080 -t ../frontend router.php
//
// - /api/...  → handled by src/api.php
// - anything else → served from the frontend folder (index.html, CSS, JS, data.json)

require_once __DIR__ . '/src/api.php';

$path = parse_url($_SERVER['REQUEST_URI'], PHP_URL_PATH) ?: '/';

if (strpos($path, '/api/') === 0 || $path === '/api') {
    aura_handle_api($_SERVER['REQUEST_METHOD'], $path);
    return true;
}

// Let the built-in server serve the frontend's static files.
return false;
