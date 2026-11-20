<?php

namespace OPNsense\Mono\FieldTypes;

use OPNsense\Base\FieldTypes\BaseListField;
use OPNsense\Core\Backend;

/*
 * Temperature sensors under hw.temperature on this router, labelled with
 * their current reading, e.g. "Core cluster (53.0 °C)".
 */
class SensorField extends BaseListField
{
    private static $sensors = null;

    /* Sensor name => label with the current reading */
    public static function readings()
    {
        if (self::$sensors === null) {
            self::$sensors = [];
            $out = (new Backend())->configdRun('mono fand sensors');
            foreach (explode("\n", (string)$out) as $line) {
                if (preg_match('/^hw\.temperature\.([A-Za-z0-9_-]+):\s*(-?[0-9.]+)C/', trim($line), $m)) {
                    $label = ucfirst(str_replace(['-', '_'], ' ', $m[1]));
                    self::$sensors[$m[1]] = sprintf('%s (%.1f °C)', $label, $m[2]);
                }
            }
        }
        return self::$sensors;
    }

    public static function label($name)
    {
        return self::readings()[$name] ?? sprintf(gettext('%s (no reading)'), $name);
    }

    protected function actionPostLoadingEvent()
    {
        $this->internalOptionList = self::readings();

        /* Keep sensors that are chosen but not readable right now, so a saved curve stays valid */
        foreach (explode(',', (string)$this->internalValue) as $name) {
            if ($name !== '' && !isset($this->internalOptionList[$name])) {
                $this->internalOptionList[$name] = self::label($name);
            }
        }
        parent::actionPostLoadingEvent();
    }
}
