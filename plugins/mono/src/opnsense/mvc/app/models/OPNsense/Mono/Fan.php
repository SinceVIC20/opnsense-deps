<?php

namespace OPNsense\Mono;

use OPNsense\Base\BaseModel;
use OPNsense\Base\Messages\Message;

/*
 * Fan control for fand.  Each curve keeps its points in one field as
 * temp:speed pairs, e.g. 40:20,80:100, the same form fand.conf uses.
 */
class Fan extends BaseModel
{
    /* Built-in curves, as written to fand.conf */
    public const PRESETS = [
        'silent' => '60:20,70:35,80:70,85:100',
        'standard' => '40:20,80:100',
        'full' => '0:100',
    ];

    /* The sensor every built-in curve follows */
    public const PRESET_SENSOR = 'core-cluster';

    /* Names fand.conf already uses: the built-in curves and "off" for a fan that is not connected */
    public const RESERVED_NAMES = ['silent', 'standard', 'full', 'off'];

    /* Highest temperature a point may use; fand has the same limit */
    public const POINT_TEMP_MAX = 110;

    public static function presetNames()
    {
        return [
            'silent' => gettext('Silent (built in)'),
            'standard' => gettext('Standard (built in)'),
            'full' => gettext('Full speed (built in)'),
        ];
    }

    /* "40:20,80:100" as [[40, 20], [80, 100]], sorted by temperature */
    public static function parsePoints($text)
    {
        $points = [];
        foreach (explode(',', (string)$text) as $pair) {
            $parts = explode(':', $pair);
            if (count($parts) == 2 && ctype_digit($parts[0]) && ctype_digit($parts[1])) {
                $points[] = [(int)$parts[0], (int)$parts[1]];
            }
        }
        usort($points, function ($a, $b) {
            return $a[0] - $b[0];
        });
        return $points;
    }

    /* Points for display, one line each: "20 % at 40 °C", or "Always 100 %" for a single point */
    public static function describePoints($text)
    {
        $points = self::parsePoints($text);
        if (count($points) == 1) {
            return [sprintf(gettext('Always %d %%'), $points[0][1])];
        }
        $lines = [];
        foreach ($points as $point) {
            $lines[] = sprintf(gettext('%d %% at %d °C'), $point[1], $point[0]);
        }
        return $lines;
    }

    public function performValidation($validateFullModel = false)
    {
        $messages = parent::performValidation($validateFullModel);

        foreach ($this->curves->curve->iterateItems() as $curve) {
            if (in_array(strtolower(trim((string)$curve->name)), self::RESERVED_NAMES)) {
                $messages->appendMessage(new Message(
                    gettext('This name is taken. Choose another one.'),
                    $curve->name->__reference
                ));
            }

            /* Mask checks the form; the numbers are checked here */
            $seen = [];
            foreach (self::parsePoints($curve->points) as $point) {
                if ($point[0] > self::POINT_TEMP_MAX || $point[1] > 100) {
                    $messages->appendMessage(new Message(
                        sprintf(
                            gettext('Temperatures go from 0 to %d °C and speeds from 0 to 100 %%.'),
                            self::POINT_TEMP_MAX
                        ),
                        $curve->points->__reference
                    ));
                    break;
                }
                if (isset($seen[$point[0]])) {
                    $messages->appendMessage(new Message(
                        sprintf(gettext('There are two points at %d °C.'), $point[0]),
                        $curve->points->__reference
                    ));
                    break;
                }
                $seen[$point[0]] = true;
            }
        }

        return $messages;
    }
}
