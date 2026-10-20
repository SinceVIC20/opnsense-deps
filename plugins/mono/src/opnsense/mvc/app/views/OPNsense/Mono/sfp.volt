<script>
$(document).ready(function () {
    const T = {
        none: "{{ lang._('No module') }}",
        nodiag: "{{ lang._('This module has no diagnostics.') }}",
        assumed: "{{ lang._('assumed') }}",
        saved: "{{ lang._('Saved. It is kept as a tunable and applied at every start.') }}",
        failed: "{{ lang._('Could not save the setting.') }}"
    };
    const UNITS = {temp: '°C', vcc: 'V', bias: 'mA'};
    const LABELS = {temp: "{{ lang._('Temperature') }}", vcc: "{{ lang._('Supply voltage') }}",
        bias: "{{ lang._('Laser bias') }}", tx: "{{ lang._('TX power') }}", rx: "{{ lang._('RX power') }}"};

    /* The API escapes HTML in its output; turn it back into plain text, which is only ever inserted with .text(). */
    function plain(v) {
        if (typeof v === 'string') {
            return $('<textarea>').html(v).val();
        }
        if (Array.isArray(v)) {
            return v.map(plain);
        }
        if (v && typeof v === 'object') {
            Object.keys(v).forEach(function (k) { v[k] = plain(v[k]); });
        }
        return v;
    }

    function row(label, value) {
        return $('<tr>').append($('<td style="width:30%">').text(label), $('<td>').text(value === null || value === undefined || value === '' ? '-' : value));
    }

    function copyText(text, link) {
        const done = function () {
            const old = link.text();
            link.text("{{ lang._('Copied') }}");
            setTimeout(function () { link.text(old); }, 1500);
        };
        if (navigator.clipboard && window.isSecureContext) {
            navigator.clipboard.writeText(text).then(done);
        } else {
            const ta = $('<textarea style="position:fixed;top:-1000px">').val(text).appendTo('body');
            ta[0].select();
            document.execCommand('copy');
            ta.remove();
            done();
        }
    }

    /* The box as plain text for pasting into a chat; leaves out the controls and raw memory. */
    function boxText(box) {
        const lines = [];
        box.children().each(function () {
            const el = $(this);
            if (el.is('h3')) {
                lines.push(el.text());
            } else if (el.is('h4') && el.next().is('table, p')) {
                lines.push('', el.text());
            } else if (el.hasClass('alert')) {
                lines.push('! ' + el.text());
            } else if (el.is('p')) {
                lines.push(el.text());
            } else if (el.is('table')) {
                el.find('tr').each(function () {
                    const cells = $(this).children('td').map(function () { return $(this).text(); }).get();
                    lines.push(cells[0] + ': ' + cells.slice(1).join(', '));
                });
            }
        });
        return lines.join('\n');
    }

    const BADGE = {display: 'inline-block', 'min-width': '4.5em', 'line-height': '1.4', 'vertical-align': 'middle', padding: '0 6px'};

    /* Values keep the theme's text colour; a solid badge carries the state so it reads on light and dark themes. */
    function stateBadge(state) {
        const cls = state === 'alarm' ? 'label-danger' : (state === 'warning' ? 'label-warning' : 'label-success');
        const word = state === 'alarm' ? "{{ lang._('alarm') }}" : (state === 'warning' ? "{{ lang._('warning') }}" : 'OK');
        return $('<span class="label">').addClass(cls).css(BADGE).text(word);
    }

    function power(v, d) {
        if (d === null || d === undefined) {
            return v ? (v / 1000).toFixed(4) + ' mW' : "{{ lang._('no light') }}";
        }
        return d.toFixed(2) + ' dBm (' + (v / 1000).toFixed(4) + ' mW)';
    }

    function valueText(kind, e) {
        return (kind === 'tx' || kind === 'rx') ? power(e.value, e.dbm) : e.value + ' ' + UNITS[kind];
    }

    function limitText(kind, e) {
        const f = (k) => (kind === 'tx' || kind === 'rx') ? (e[k + '_dbm'] === null ? '-' : e[k + '_dbm'] + ' dBm') : e[k] + ' ' + UNITS[kind];
        return "{{ lang._('warning') }} " + f('low_warning') + ' / ' + f('high_warning') + ', ' +
            "{{ lang._('alarm') }} " + f('low_alarm') + ' / ' + f('high_alarm');
    }

    function rxWavelength(m) {
        const r = m.rx_wavelength;
        if (!r) {
            return null;
        }
        const nm = r.nm ? r.nm + ' nm' : r.choices.join(' or ') + ' nm';
        return nm + ' (' + T.assumed + ', ' + r.basis + ')';
    }

    function portPanel(p) {
        const box = $('<div class="content-box __mb" style="padding:10px;position:relative">');
        const title = p.cage + ' - ' + p.device + (p.interface ? ' (' + p.interface + ')' : '');
        const copy = $('<a href="#" style="position:absolute;top:10px;right:14px">').text("{{ lang._('Copy') }}");
        copy.prepend($('<i class="fa fa-clipboard" style="margin-right:4px">'));
        copy.click(function (e) {
            e.preventDefault();
            copyText(boxText(box), copy);
        });
        box.append(copy, $('<h3>').text(title));

        (p.advice || []).forEach(function (a) {
            box.append($('<div class="alert alert-warning" role="alert">').text(a));
        });

        const t = $('<table class="table table-condensed table-striped">');
        t.append(row("{{ lang._('Link') }}", p.status), row("{{ lang._('Media') }}", p.media));
        if (p.phy_modes) {
            t.append(row("{{ lang._('Copper rates') }}", p.phy_modes));
        }
        box.append(t);

        /* Always shown; only a port in 1G (1000BASE-X) mode can use it. */
        const applies = p.pcs_autoneg !== null && p.pcs_autoneg !== undefined;
        if (applies) {
            t.append(row("{{ lang._('1000BASE-X autonegotiation') }}", p.pcs_autoneg ? "{{ lang._('on') }}" : "{{ lang._('off (fixed link)') }}"));
        }
        const sel = $('<select class="selectpicker" data-width="auto">').append(
            $('<option value="0">').text("{{ lang._('Off - fixed 1G link (default)') }}"),
            $('<option value="1">').text("{{ lang._('On - auto-negotiate') }}"));
        sel.val(applies ? String(p.pcs_autoneg) : '0');
        if (!applies) {
            /* The list still opens to show the choices, but "On" can't be picked. */
            sel.find('option[value="1"]').prop('disabled', true);
        }
        const btn = $('<button class="btn btn-primary" style="margin-left:10px">').text("{{ lang._('Apply') }}").prop('disabled', !applies);
        const msg = $('<span class="text-muted" style="margin-left:10px">');
        if (!applies) {
            msg.text("{{ lang._('(Only for ports in 1G mode)') }}");
        }
        btn.click(function () {
            ajaxCall('/api/mono/sfp/autoneg', {device: p.device, value: sel.val()}, function (r) {
                msg.text(r.result === 'saved' ? T.saved : T.failed + (r.message ? ' ' + r.message : ''));
                load();
            });
        });
        box.append($('<h4>').toggleClass('text-muted', !applies).text("{{ lang._('1000BASE-X autonegotiation') }}"),
            $('<div>').append(sel, btn, msg));
        sel.selectpicker();

        if (!p.present) {
            box.append($('<p>').text(T.none));
            return box;
        }
        const m = p.module;
        const mt = $('<table class="table table-condensed table-striped">');
        mt.append(
            row("{{ lang._('Vendor') }}", m.vendor + ' (OUI ' + m.vendor_oui + ')'),
            row("{{ lang._('Part number') }}", m.part_number),
            row("{{ lang._('Revision') }}", m.revision),
            row("{{ lang._('Serial number') }}", m.serial),
            row("{{ lang._('Date code') }}", m.date_code),
            row("{{ lang._('Type') }}", m.identifier),
            row("{{ lang._('Connector') }}", m.connector),
            row("{{ lang._('Standards') }}", m.compliance.join(', ')),
            row("{{ lang._('Encoding') }}", m.encoding),
            row("{{ lang._('Nominal rate') }}", m.nominal_rate_mbd ? m.nominal_rate_mbd + ' MBd' : null),
            row("{{ lang._('Reach') }}", m.reach.join(', ')));
        if (m.optical) {
            mt.append(row("{{ lang._('TX wavelength') }}", m.wavelength_nm ? m.wavelength_nm + ' nm' : null));
        }
        if (m.bidi) {
            mt.append(row("{{ lang._('Expected RX wavelength') }}", rxWavelength(m)));
        }
        mt.append(row("{{ lang._('SFF-8472 revision') }}", m.sff8472_rev),
            row("{{ lang._('Checksums') }}", m.checksums_ok ? 'OK' : "{{ lang._('wrong') }}"));
        box.append($('<h4>').text("{{ lang._('Module') }}"), mt);

        box.append($('<h4>').text("{{ lang._('Diagnostics') }}"));
        if (!p.diagnostics) {
            box.append($('<p>').text(T.nodiag));
        } else {
            const d = p.diagnostics;
            const dt = $('<table class="table table-condensed table-striped">');
            ['temp', 'vcc', 'bias', 'tx', 'rx'].forEach(function (k) {
                const e = d.values[k];
                if (e.state === 'unused') {
                    return;
                }
                dt.append($('<tr>').append($('<td style="width:30%">').text(LABELS[k]),
                    $('<td style="width:1%;white-space:nowrap;padding-right:12px">').text(valueText(k, e)),
                    $('<td style="width:1%;white-space:nowrap;vertical-align:middle;padding-right:16px">').append(stateBadge(e.state)),
                    $('<td class="text-muted">').text(limitText(k, e))));
            });
            const s = d.status;
            const flags = [];
            if (s.tx_disable || s.soft_tx_disable) flags.push("{{ lang._('TX disabled') }}");
            if (s.tx_fault) flags.push("{{ lang._('TX fault') }}");
            if (s.rx_los) flags.push("{{ lang._('RX loss of signal') }}");
            if (s.rate_select) flags.push("{{ lang._('rate select') }}");
            dt.append(row("{{ lang._('Status flags') }}", flags.length ? flags.join(', ') : "{{ lang._('none') }}"));
            dt.append(row("{{ lang._('Alarms') }}", d.alarms.length ? d.alarms.join(', ') : "{{ lang._('none') }}"));
            dt.append(row("{{ lang._('Warnings') }}", d.warnings.length ? d.warnings.join(', ') : "{{ lang._('none') }}"));
            dt.append(row("{{ lang._('Calibration') }}", m.ddm.calibration + ', RX ' + m.ddm.rx_power_type));
            box.append(dt);
        }

        const rawText = title + ' ' + m.vendor + ' ' + m.part_number + '\nA0h ' + p.raw.a0 + '\nA2h ' + p.raw.a2;
        const rawCopy = $('<a href="#" style="margin-left:10px">').text("{{ lang._('Copy') }}");
        rawCopy.click(function (e) {
            e.preventDefault();
            copyText(rawText, rawCopy);
        });
        const raw = $('<details>').append($('<summary>').text("{{ lang._('Raw module memory (A0h, A2h)') }}").append(rawCopy),
            $('<pre style="white-space:pre-wrap;word-break:break-all">').text('A0h ' + p.raw.a0 + '\nA2h ' + p.raw.a2));
        box.append(raw);
        return box;
    }

    function load() {
        ajaxGet('/api/mono/sfp/status', {}, function (data) {
            data = plain(data);
            const area = $('#sfp_ports').empty();
            if (!data.ports || !data.ports.length) {
                area.append($('<p>').text("{{ lang._('No SFP ports found.') }}"));
                return;
            }
            data.ports.forEach(function (p) {
                area.append(portPanel(p));
            });
        });
    }

    /* Live log: kernel messages plus changes seen between polls. */
    let offset = 'boot', last = {}, timer = null;

    /* kind: ok, bad or info get a solid badge (readable on any theme), muted for kernel lines. */
    function logLine(time, text, kind) {
        const box = $('#sfp_log');
        const line = $('<div>').text((time ? time + '  ' : '') + text);
        const badges = {ok: ['label-success', 'OK'], bad: ['label-danger', '!'], info: ['label-info', 'i']};
        if (badges[kind]) {
            line.prepend($('<span class="label">').addClass(badges[kind][0]).css(BADGE).css({'min-width': '2.5em', 'margin-right': '6px'}).text(badges[kind][1]));
        } else if (kind === 'muted') {
            line.addClass('text-muted');
        }
        box.append(line);
        box.scrollTop(box[0].scrollHeight);
    }

    function now() {
        return new Date().toTimeString().substring(0, 8);
    }

    function summary(p) {
        const s = {present: p.present, status: p.status, media: p.media};
        if (p.present) {
            s.module = p.module.vendor + ' ' + p.module.part_number + ' (' + p.module.serial + ')';
            if (p.diagnostics) {
                s.rx = p.diagnostics.values.rx.dbm;
                s.tx = p.diagnostics.values.tx.dbm;
                s.alarms = p.diagnostics.alarms.join(', ');
                s.los = p.diagnostics.status.rx_los;
            }
        }
        return s;
    }

    function changes(p) {
        const s = summary(p), o = last[p.device], who = p.cage + ' ' + p.device + ': ';
        last[p.device] = s;
        if (o === undefined) {
            logLine(now(), who + (s.present ? "{{ lang._('module') }} " + s.module : T.none) + ', ' + s.status);
            return;
        }
        if (s.present !== o.present || s.module !== o.module) {
            logLine(now(), who + (s.present ? "{{ lang._('module inserted') }}: " + s.module : "{{ lang._('module removed') }}"), 'info');
        }
        if (s.status !== o.status) {
            logLine(now(), who + "{{ lang._('link') }} " + s.status, s.status === 'active' ? 'ok' : 'bad');
        }
        if (s.media !== o.media && s.present) {
            logLine(now(), who + "{{ lang._('media') }} " + s.media);
        }
        if (s.los !== o.los && s.los !== undefined) {
            logLine(now(), who + (s.los ? "{{ lang._('RX loss of signal') }}" : "{{ lang._('RX signal present') }}"), s.los ? 'bad' : 'ok');
        }
        if (s.rx !== undefined && o.rx !== undefined && (s.rx === null) !== (o.rx === null || Math.abs((s.rx || 0) - (o.rx || 0)) >= 1)) {
            logLine(now(), who + 'RX ' + (s.rx === null ? "{{ lang._('no light') }}" : s.rx + ' dBm') + ', TX ' + (s.tx === null ? '-' : s.tx + ' dBm'));
        }
        if (s.alarms && s.alarms !== o.alarms) {
            logLine(now(), who + "{{ lang._('alarms') }}: " + s.alarms, 'bad');
        }
    }

    function poll() {
        ajaxGet('/api/mono/sfp/status', {}, function (data) {
            (plain(data).ports || []).forEach(changes);
        });
        const first = offset === 'boot';
        ajaxGet('/api/mono/sfp/log', {offset: offset}, function (data) {
            data = plain(data);
            if (first && data.boot) {
                logLine('', '--- ' + "{{ lang._('Since the router started at') }} " + data.boot + ' ---', 'info');
            }
            (data.lines || []).forEach(function (l) {
                logLine(l.time, l.text, 'muted');
            });
            if (first) {
                logLine('', '--- ' + "{{ lang._('Live from here') }}" + ' ---', 'info');
            }
            offset = data.offset;
        });
    }

    function startLog() {
        if (timer === null) {
            poll();
            timer = setInterval(poll, 2000);
            $('#sfp_log_toggle').text("{{ lang._('Pause') }}");
        }
    }

    function stopLog() {
        if (timer !== null) {
            clearInterval(timer);
            timer = null;
            $('#sfp_log_toggle').text("{{ lang._('Resume') }}");
        }
    }

    /* Pause while the browser tab is hidden, resume when it is shown again (not after a manual Pause). */
    let autoPaused = false;
    document.addEventListener('visibilitychange', function () {
        if (document.hidden && timer !== null) {
            stopLog();
            autoPaused = true;
        } else if (!document.hidden && autoPaused) {
            autoPaused = false;
            if ($('#tab_log').hasClass('active')) {
                logLine(now(), "{{ lang._('Paused while this browser tab was hidden, catching up') }}", 'info');
                startLog();
            }
        }
    });

    $('#sfp_log_toggle').click(function () {
        autoPaused = false;
        timer === null ? startLog() : stopLog();
    });
    $('#sfp_log_clear').click(function () {
        $('#sfp_log').empty();
    });
    $('#sfp_log_copy').click(function () {
        const lines = $('#sfp_log').children().map(function () { return $(this).text(); }).get();
        copyText(lines.join('\n'), $(this));
    });
    $('#sfp_refresh').click(load);
    $('a[data-toggle="tab"]').on('shown.bs.tab', function (e) {
        const tab = $(e.target).attr('href');
        tab === '#tab_log' ? startLog() : stopLog();
        /* Keep the open tab in the address so a refresh comes back to it. */
        history.replaceState(null, '', tab === '#tab_modules' ? window.location.pathname : tab);
    });

    load();
    if (window.location.hash === '#tab_log') {
        $('a[href="#tab_log"]').tab('show');
    }
});
</script>

<ul class="nav nav-tabs" data-tabs="tabs" id="maintabs">
    <li class="active"><a data-toggle="tab" href="#tab_modules">{{ lang._('Modules') }}</a></li>
    <li><a data-toggle="tab" href="#tab_log">{{ lang._('Live log') }}</a></li>
</ul>
<div class="tab-content content-box">
    <div id="tab_modules" class="tab-pane fade in active" style="padding:10px">
        <button id="sfp_refresh" class="btn btn-default"><i class="fa fa-refresh"></i> {{ lang._('Refresh') }}</button>
        <div id="sfp_ports" style="margin-top:10px"></div>
    </div>
    <div id="tab_log" class="tab-pane fade" style="padding:10px">
        <p>{{ lang._('Starts with every SFP message since the router started, then follows live. Remove or insert a module and watch what the router sees. Checks every 2 seconds while this tab is open.') }}</p>
        <button id="sfp_log_toggle" class="btn btn-default">{{ lang._('Pause') }}</button>
        <button id="sfp_log_clear" class="btn btn-default">{{ lang._('Clear') }}</button>
        <button id="sfp_log_copy" class="btn btn-default">{{ lang._('Copy') }}</button>
        <div id="sfp_log" style="margin-top:10px;height:400px;overflow-y:auto;font-family:monospace;font-size:12px;line-height:1.9;border:1px solid rgba(128,128,128,0.4);padding:6px"></div>
    </div>
</div>
