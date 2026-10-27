<?php

namespace OPNsense\Mono\Api;

use OPNsense\Base\ApiControllerBase;
use OPNsense\Core\Backend;
use OPNsense\Core\Config;

class StatusController extends ApiControllerBase
{
    /* cmm rewrites this every 5 seconds; older than this, cmm has stopped. */
    private const CONN_FILE = '/var/run/cmm.conn';
    private const CONN_FRESH_S = 15;
    private const RELEASE_FILE = '/usr/local/share/mono-gateway/release';

    /**
     * Connection counts from the file cmm keeps, so a refresh starts no
     * process. Null while offload isn't running: then status.py counts the
     * connections from PF instead.
     */
    private function cmmCounts()
    {
        $mtime = @filemtime(self::CONN_FILE);
        if ($mtime === false || time() - $mtime > self::CONN_FRESH_S) {
            return null;
        }
        $words = preg_split('/\s+/', trim((string)@file_get_contents(self::CONN_FILE)));
        $counts = [];
        for ($i = 0; $i + 1 < count($words); $i += 2) {
            $counts[$words[$i]] = (int)$words[$i + 1];
        }
        if (!isset($counts['hw_active'])) {
            return null;
        }
        return [
            'offload' => 'on',
            'connections' => $counts['hw_active'],
            /* same as status.py: retry, kept and other are on the CPU */
            'cpu_connections' => ($counts['retry'] ?? 0) + ($counts['kept'] ?? 0) + ($counts['other'] ?? 0),
        ];
    }

    private function updateServer()
    {
        $mirror = trim((string)(Config::getInstance()->object()->system->firmware->mirror ?? ''));
        if ($mirror === '') {
            return ['', ''];
        }
        $url = parse_url($mirror);
        if (!in_array($url['scheme'] ?? '', ['http', 'https']) || empty($url['host'])) {
            return [$mirror, ''];
        }
        $port = isset($url['port']) ? ':' . $url['port'] : '';
        return [$url['host'], "{$url['scheme']}://{$url['host']}{$port}/"];
    }

    public function infoAction()
    {
        $counts = $this->cmmCounts();
        $release = preg_split('/\s+/', trim((string)@file_get_contents(self::RELEASE_FILE)));
        if ($counts !== null && count($release) == 2) {
            list($server, $server_url) = $this->updateServer();
            return [
                'release' => $release[0],
                'commit' => $release[1],
                'server' => $server,
                'server_url' => $server_url,
            ] + $counts;
        }

        $backend = new Backend();
        $data = json_decode($backend->configdRun('mono status'), true);
        if (!is_array($data)) {
            return ['release' => '', 'commit' => '', 'offload' => 'unknown', 'connections' => 0, 'server' => '', 'server_url' => ''];
        }
        return $data;
    }

    public function connectionsAction()
    {
        $counts = $this->cmmCounts();
        if ($counts !== null) {
            return $counts;
        }

        $backend = new Backend();
        $data = json_decode($backend->configdRun('mono connections'), true);
        if (!is_array($data)) {
            return ['offload' => 'unknown', 'connections' => 0, 'cpu_connections' => 0];
        }
        return $data;
    }

    public function cpuStreamAction()
    {
        return $this->configdStream(
            'mono cpu stream',
            ['1'],
            [
                'Content-Type: text/event-stream',
                'Cache-Control: no-cache'
            ]
        );
    }
}
