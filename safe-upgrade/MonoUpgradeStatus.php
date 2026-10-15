<?php

/*
 * Dashboard notice after an upgrade was rolled back, or while updates wait for room on the boot partition.
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
        $space = '/var/db/mono-upgrade/space-notice';
        $messages = [];

        if (file_exists($space)) {
            $lines = file($space, FILE_IGNORE_NEW_LINES | FILE_SKIP_EMPTY_LINES);
            $mb = (int)array_shift($lines);
            $msg = sprintf(
                gettext('The boot partition (/boot/msdos) needs %d MB more free space for the next kernel update. ' .
                'Kernel, base and offload module updates wait until then; other updates still install.'),
                $mb
            );
            if (!empty($lines)) {
                $msg .= ' ' . sprintf(gettext('Files there that updates don\'t use: %s.'), implode(', ', $lines));
                // Ready-to-run command with this router's own file names.
                $paths = array_map(function ($line) {
                    return '/boot/msdos/' . preg_replace('/ \(\d+ MB\)$/', '', $line);
                }, $lines);
                $msg .= ' ' . sprintf(
                    gettext('To move them to the main disk and keep a copy, log in as root (SSH or the console) and run: mv %s /root/ ' .
                    '(or use rm instead of mv to delete them).'),
                    implode(' ', $paths)
                );
            }
            $msg .= ' ' . gettext('Updates resume by themselves within 5 minutes after there is enough room.');
            $messages[] = $msg;
            $this->internalTimestamp = filemtime($space);
        }

        if (file_exists($flag)) {
            $messages[] = gettext(
                'The last upgrade failed and the router went back to its previous version. ' .
                'Some anonymous details were sent to improve the upgrade code; ' .
                'they are in the firmware Log tab and in /root/upgrade-failed.txt, with their ID. ' .
                'When a fix is posted, Check for updates offers it.'
            );
            $this->internalTimestamp = filemtime($flag);
        }

        if (!empty($messages)) {
            $this->internalStatus = SystemStatusCode::WARNING;
            $this->internalMessage = implode(' ', $messages);
        }
    }

    public function dismissStatus()
    {
        @unlink('/var/db/mono-upgrade/notice');
    }
}
