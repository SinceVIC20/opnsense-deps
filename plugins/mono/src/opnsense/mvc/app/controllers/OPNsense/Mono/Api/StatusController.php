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
}
