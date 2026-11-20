<?php

namespace OPNsense\Mono;

use OPNsense\Base\IndexController;

class FanController extends IndexController
{
    public function indexAction()
    {
        $this->view->fansForm = $this->getForm('fans');
        $this->view->formDialogCurve = $this->getForm('dialogCurve');
        $this->view->pick('OPNsense/Mono/fan');
    }
}
