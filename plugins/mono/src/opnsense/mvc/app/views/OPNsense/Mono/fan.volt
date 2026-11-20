<script>
$(document).ready(function () {
    const fanSelects = {
        fan0: $('#fan\\.fans\\.fan0'),
        fan1: $('#fan\\.fans\\.fan1')
    };

    /* Fills the fan dropdowns again, keeping choices not saved yet while that curve still exists */
    function loadFans() {
        const chosen = {};
        Object.keys(fanSelects).forEach(function (fan) { chosen[fan] = fanSelects[fan].val(); });
        mapDataToFormUI({'frm_fans': '/api/mono/fan/get'}).done(function () {
            Object.keys(fanSelects).forEach(function (fan) {
                const select = fanSelects[fan];
                if (chosen[fan] && select.find('option').filter(function () { return this.value === chosen[fan]; }).length) {
                    select.val(chosen[fan]);
                }
            });
            $('.selectpicker').selectpicker('refresh');
        });
    }

    /* Writes fand.conf from the saved settings and has fand read it */
    function apply(done) {
        ajaxCall('/api/mono/fan/reconfigure', {}, function (data) {
            if (data.status !== 'ok') {
                BootstrapDialog.show({
                    type: BootstrapDialog.TYPE_DANGER,
                    title: "{{ lang._('Fan control') }}",
                    message: "{{ lang._('The settings are saved, but fand could not be updated. See the system log.') }}"
                });
            }
            setTimeout(refreshSpeeds, 3000);
            if (done) {
                done();
            }
        });
    }

    /* Live speed of each fan, next to its dropdown */
    const speedFields = {};
    ['fan0', 'fan1'].forEach(function (fan) {
        speedFields[fan] = $('<span class="form-control-static" style="display: inline-block;">');
        $('#row_fan\\.fans\\.' + fan + ' > td:last').prepend(speedFields[fan]);
    });

    function refreshSpeeds() {
        ajaxGet('/api/hwmon/sensors/status', {}, function (data) {
            const spinning = {};
            (data.fans || []).forEach(function (f) { spinning[f.label] = f; });
            Object.keys(speedFields).forEach(function (fan) {
                const f = spinning[fan];
                speedFields[fan].text(f ? f.pwm + ' %   ' + f.rpm + ' RPM' : "{{ lang._('Not spinning') }}")
                    .css('white-space', 'pre');
            });
        });
    }

    /*
     * Points editor in the curve dialog.  The points are saved in the
     * hidden curve.points field as "40:20,80:100"; the rows below are
     * only a way to edit that text.
     */
    const pointsField = $('#curve\\.points');
    const editor = $('<div id="points_editor"></div>');
    pointsField.hide().after(editor);

    function syncPoints() {
        const pairs = [];
        editor.find('tr.point').each(function () {
            const t = $(this).find('.p-temp').val().trim();
            const s = $(this).find('.p-speed').val().trim();
            if (t !== '' || s !== '') {
                pairs.push(t + ':' + s);
            }
        });
        pointsField.val(pairs.join(','));
    }

    function addRow(body, temp, speed) {
        const cell = () => $('<td style="vertical-align: middle;">');
        const row = $('<tr class="point">').append(
            cell().append($('<input type="text" class="p-temp" style="width: 90px;">').val(temp)),
            cell().append($('<input type="text" class="p-speed" style="width: 90px;">').val(speed)),
            cell().append($('<a href="#">').text("{{ lang._('Remove') }}").click(function (e) {
                e.preventDefault();
                row.remove();
                syncPoints();
            }))
        );
        row.find('input').on('input', syncPoints);
        body.append(row);
    }

    function buildEditor() {
        const body = $('<tbody>');
        const pairs = (pointsField.val() || '').split(',').filter(function (p) { return p.indexOf(':') > 0; })
            .map(function (p) { return p.split(':'); })
            .sort(function (a, b) { return parseInt(a[0], 10) - parseInt(b[0], 10); });
        pairs.forEach(function (p) { addRow(body, p[0], p[1]); });
        if (pairs.length === 0) {
            addRow(body, '', '');
        }
        const add = $('<button type="button" class="btn btn-xs btn-default">').text("{{ lang._('Add point') }}").click(function () {
            addRow(body, '', '');
            body.find('tr.point:last .p-temp').focus();
        });
        editor.empty().append(
            $('<table class="table table-condensed" style="max-width: 360px; margin-bottom: 6px;">').append(
                $('<thead>').append($('<tr>').append(
                    $('<th>').text("{{ lang._('Temperature (°C)') }}"),
                    $('<th>').text("{{ lang._('Speed (%)') }}"),
                    $('<th>')
                )),
                body
            ),
            add
        );
    }

    $('#DialogCurve').on('shown.bs.modal', buildEditor);

    loadFans();
    refreshSpeeds();
    setInterval(refreshSpeeds, 5000);

    $('#grid-curves').UIBootgrid({
        search: '/api/mono/fan/search_curve',
        get: '/api/mono/fan/get_curve/',
        set: '/api/mono/fan/set_curve/',
        add: '/api/mono/fan/add_curve/',
        del: '/api/mono/fan/del_curve/',
        commands: {
            edit: { filter: (cell) => cell.getData().uuid.includes('-') },
            copy: { filter: (cell) => cell.getData().uuid.includes('-') },
            delete: { filter: (cell) => cell.getData().uuid.includes('-') }
        },
        options: {
            formatters: {
                /* One sensor or point per line */
                lines: function (column, row) {
                    return $('<div>').text(row['%' + column.id] ?? row[column.id] ?? '').html().replace(/\n/g, '<br>');
                }
            }
        }
    });

    /* The grid sends this after a curve is added, changed or deleted, not on sort or search */
    $(document).on('settings-changed', function () {
        loadFans();
        apply();
    });

    $('#saveAct').click(function () {
        const btn = $(this).prop('disabled', true);
        saveFormToEndpoint('/api/mono/fan/set', 'frm_fans', function () {
            apply(function () {
                btn.prop('disabled', false);
            });
        }, true, function () {
            btn.prop('disabled', false);
        });
    });
});
</script>

<div class="content-box __mb">
    <div class="col-md-12">
        <h2>{{ lang._('Fans') }}</h2>
    </div>
    {{ partial("layout_partials/base_form", ['fields': fansForm, 'id': 'frm_fans']) }}
</div>

<div class="content-box __mb">
    <div class="col-md-12">
        <h2>{{ lang._('Curves') }}</h2>
    </div>
    <table id="grid-curves" class="table table-condensed table-hover table-striped" data-editDialog="DialogCurve">
        <thead>
            <tr>
                <th data-column-id="uuid" data-type="string" data-identifier="true" data-visible="false">{{ lang._('ID') }}</th>
                <th data-column-id="name" data-type="string">{{ lang._('Name') }}</th>
                <th data-column-id="sensors" data-type="string" data-formatter="lines">{{ lang._('Sensors') }}</th>
                <th data-column-id="points" data-type="string" data-formatter="lines" data-sortable="false">{{ lang._('Points') }}</th>
                <th data-column-id="commands" data-width="7em" data-formatter="commands" data-sortable="false">{{ lang._('Commands') }}</th>
            </tr>
        </thead>
        <tbody></tbody>
        <tfoot>
            <tr>
                <td></td>
                <td>
                    <button data-action="add" type="button" class="btn btn-xs btn-primary"><span class="fa fa-fw fa-plus"></span></button>
                </td>
            </tr>
        </tfoot>
    </table>
</div>

<section class="page-content-main">
    <div class="content-box">
        <div class="col-md-12">
            <br/>
            <button class="btn btn-primary" id="saveAct" type="button"><b>{{ lang._('Save') }}</b></button>
            <br/><br/>
        </div>
    </div>
</section>

{{ partial("layout_partials/base_dialog", ['fields': formDialogCurve, 'id': 'DialogCurve', 'label': lang._('Edit curve')]) }}
