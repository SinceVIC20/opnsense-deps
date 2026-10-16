<?php

/*
 * List cmm on the Services page so it can be started and stopped there.
 */

function mono_services()
{
    if (!file_exists('/usr/local/etc/rc.d/cmm')) {
        return [];
    }

    return [[
        'name' => 'cmm',
        'description' => gettext('Hardware offload (cmm)'),
        'pidfile' => '/var/run/cmm.pid',
        'configd' => [
            'start' => ['mono start'],
            'stop' => ['mono stop'],
            'restart' => ['mono restart'],
        ],
    ]];
}
