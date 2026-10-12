<?php

/*
 * Dashboard notice after an upgrade was rolled back.
 */

namespace OPNsense\System\Status;

use OPNsense\System\AbstractStatus;
use OPNsense\System\SystemStatusCode;

class MonoUpgradeStatus extends AbstractStatus
{
    public function __construct()
    {
        $this->internalPriority = 10;
        $this->internalTitle = gettext('Mono upgrade');
        $this->internalLocation = '/ui/core/firmware#updates';
    }

    public function collectStatus()
    {
        $flag = '/var/db/mono-upgrade/notice';

        if (file_exists($flag)) {
            $this->internalStatus = SystemStatusCode::WARNING;
            $this->internalTimestamp = filemtime($flag);
            $this->internalMessage = gettext(
                'The last upgrade failed and the router went back to its previous version. ' .
                'An anonymous bug report is sent automatically to improve the upgrade code; ' .
                'it is in the firmware Log tab and in /root/upgrade-failed.txt, with its ID. ' .
                'When a fix is posted, Check for updates offers it.'
            );
        }
    }

    public function dismissStatus()
    {
        @unlink('/var/db/mono-upgrade/notice');
    }
}
