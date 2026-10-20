<?php

namespace OPNsense\Mono\Api;

use OPNsense\Base\ApiControllerBase;
use OPNsense\Core\Backend;
use OPNsense\Core\Config;
use OPNsense\Core\Tunables;

class SfpController extends ApiControllerBase
{
    private function ports()
    {
        $data = json_decode((new Backend())->configdRun('mono sfp'), true);
        return is_array($data) && isset($data['ports']) ? $data['ports'] : [];
    }

    public function statusAction()
    {
        return ['ports' => $this->ports()];
    }

    public function logAction()
    {
        $offset = $this->request->get('offset', 'string', '-1');
        if ($offset !== 'boot' && !preg_match('/^-?\d+$/', $offset)) {
            $offset = '-1';
        }
        $data = json_decode((new Backend())->configdpRun('mono sfplog', [$offset]), true);
        return is_array($data) ? $data : ['offset' => 0, 'lines' => []];
    }

    /* Turns 1000BASE-X autonegotiation on or off and keeps it as a tunable so it survives a restart. */
    public function autonegAction()
    {
        if (!$this->request->isPost()) {
            return ['result' => 'failed'];
        }
        $dev = $this->request->getPost('device', 'string', '');
        $value = $this->request->getPost('value', 'int', -1);
        if ($value !== 0 && $value !== 1) {
            return ['result' => 'failed', 'message' => 'value must be 0 or 1'];
        }
        $port = null;
        foreach ($this->ports() as $p) {
            if ($p['device'] === $dev && $p['pcs_autoneg'] !== null) {
                $port = $p;
            }
        }
        if ($port === null) {
            return ['result' => 'failed', 'message' => 'not a 1G SFP port'];
        }

        $name = 'dev.' . str_replace('dtsec', 'dtsec.', $dev) . '.pcs_autoneg';
        Config::getInstance()->lock();
        $mdl = new Tunables();
        $node = null;
        foreach ($mdl->item->iterateItems() as $item) {
            if ((string)$item->tunable === $name) {
                $node = $item;
            }
        }
        if ($node === null) {
            $node = $mdl->item->Add();
            $node->tunable = $name;
            $node->descr = '1G SFP autonegotiation (set on System: Diagnostics: SFP+)';
        }
        $node->value = (string)$value;
        $mdl->serializeToConfig();
        Config::getInstance()->save();
        (new Backend())->configdpRun('service restart', ['sysctl']);

        return ['result' => 'saved'];
    }
}
