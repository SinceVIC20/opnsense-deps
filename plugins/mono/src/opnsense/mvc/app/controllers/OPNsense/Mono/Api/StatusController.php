<?php

namespace OPNsense\Mono\Api;

use OPNsense\Base\ApiControllerBase;
use OPNsense\Core\Backend;

class StatusController extends ApiControllerBase
{
    public function infoAction()
    {
        $backend = new Backend();
        $data = json_decode($backend->configdRun('mono status'), true);
        if (!is_array($data)) {
            return ['release' => '', 'commit' => '', 'offload' => 'unknown', 'connections' => 0, 'server' => '', 'server_url' => ''];
        }
        return $data;
    }

    public function connectionsAction()
    {
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
