<?php

namespace OPNsense\Mono\Api;

use OPNsense\Base\ApiMutableModelControllerBase;
use OPNsense\Core\Backend;
use OPNsense\Core\Config;
use OPNsense\Mono\Fan;
use OPNsense\Mono\FieldTypes\SensorField;

class FanController extends ApiMutableModelControllerBase
{
    protected static $internalModelName = 'fan';
    protected static $internalModelClass = 'OPNsense\Mono\Fan';

    /* Built-in curves first, read only, then the user's by name; sensors and points one per line */
    public function searchCurveAction()
    {
        $rows = [];
        foreach (Fan::PRESETS as $id => $points) {
            $rows[] = $this->curveRow($id, Fan::presetNames()[$id], Fan::PRESET_SENSOR, $points);
        }
        $curves = [];
        foreach ($this->getModel()->curves->curve->iterateItems() as $uuid => $curve) {
            $curves[] = $this->curveRow($uuid, (string)$curve->name, (string)$curve->sensors, (string)$curve->points);
        }
        usort($curves, function ($a, $b) {
            return strnatcasecmp($a['name'], $b['name']);
        });
        return $this->searchRecordsetBase(array_merge($rows, $curves), ['name', '%sensors', '%points']);
    }

    /* Built-in curves have no uuid as id, so the grid offers no edit or delete for them */
    private function curveRow($id, $name, $sensors, $points)
    {
        return [
            'uuid' => $id,
            'name' => $name,
            'sensors' => $sensors,
            '%sensors' => implode("\n", array_map([SensorField::class, 'label'], explode(',', $sensors))),
            'points' => $points,
            '%points' => implode("\n", Fan::describePoints($points)),
        ];
    }

    public function getCurveAction($uuid = null)
    {
        return $this->getBase('curve', 'curves.curve', $uuid);
    }

    public function addCurveAction()
    {
        return $this->addBase('curve', 'curves.curve');
    }

    public function setCurveAction($uuid)
    {
        return $this->setBase('curve', 'curves.curve', $uuid);
    }

    /* Fans that used a deleted curve move to Standard. */
    public function delCurveAction($uuid)
    {
        if ($this->request->isPost() && $uuid !== null) {
            Config::getInstance()->lock();
            $mdl = $this->getModel();
            foreach (['fan0', 'fan1'] as $fan) {
                if (in_array((string)$mdl->fans->$fan, explode(',', $uuid), true)) {
                    $mdl->fans->$fan = 'standard';
                }
            }
        }
        return $this->delBase('curves.curve', $uuid);
    }

    /* Writes /usr/local/etc/fand.conf from the saved settings and has fand read it again. */
    public function reconfigureAction()
    {
        if (!$this->request->isPost()) {
            return ['status' => 'failed'];
        }
        $backend = new Backend();
        $template = trim($backend->configdpRun('template reload', ['OPNsense/Mono']));
        $reload = $template === 'OK' ? trim($backend->configdRun('mono fand reload')) : '';
        return ['status' => $reload === 'OK' ? 'ok' : 'failed'];
    }
}
