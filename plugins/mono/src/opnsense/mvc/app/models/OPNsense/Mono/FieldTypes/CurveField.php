<?php

namespace OPNsense\Mono\FieldTypes;

use OPNsense\Base\FieldTypes\BaseListField;
use OPNsense\Mono\Fan;

/*
 * Curve choice for a fan: not connected, one of the built-in curves, or
 * one of the curves defined in this model (by uuid).
 */
class CurveField extends BaseListField
{
    protected function actionPostLoadingEvent()
    {
        $this->internalOptionList = ['off' => gettext('Not connected')] + Fan::presetNames();
        foreach ($this->getParentModel()->curves->curve->iterateItems() as $uuid => $curve) {
            $this->internalOptionList[$uuid] = (string)$curve->name;
        }
        parent::actionPostLoadingEvent();
    }
}
