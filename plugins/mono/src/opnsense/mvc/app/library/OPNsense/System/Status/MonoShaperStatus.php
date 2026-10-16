<?php

namespace OPNsense\System\Status;

use OPNsense\System\AbstractStatus;
use OPNsense\System\SystemStatusCode;

/* Banner on the shaper pages: shaped traffic can't use hardware offload. */
class MonoShaperStatus extends AbstractStatus
{
    public function __construct()
    {
        $this->internalPriority = 2;
        $this->internalPersistent = true;
        $this->internalTitle = gettext('Hardware offload');
        $this->internalIsBanner = true;
        $this->internalScope[] = '/ui/trafficshaper*';
    }

    public function collectStatus()
    {
        $this->internalMessage = gettext(
            'Hardware offload cannot shape traffic. Connections on any interface used by a shaper rule ' .
            'are not offloaded and are handled by the CPU cores.'
        );
        $this->internalStatus = SystemStatusCode::NOTICE;
    }
}
