<?php

namespace OPNsense\Mono;

use OPNsense\Base\IndexController;

class SfpController extends IndexController
{
    public function indexAction()
    {
        $this->view->pick('OPNsense/Mono/sfp');
    }
}
