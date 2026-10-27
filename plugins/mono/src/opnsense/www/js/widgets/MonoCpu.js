/*
 * Copyright (C) 2024 Deciso B.V.
 * Copyright (C) 2026 SinceVIC20
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY,
 * OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

// The stock CPU widget with a fixed 0-100 % scale and a line per core.
export default class MonoCpu extends BaseWidget {
    constructor(config) {
        super(config);
        this.resizeHandles = "e, w";
        this.configurable = true;
        this.graphs = ['cores', 'total', 'intr', 'user', 'sys'];
        // The Connections gauge's green and orange, with a violet and school-bus yellow.
        this.coreColors = ['#A97FDB', '#FFD800', '#5E9C4A', '#D94F00'];
        // See-through lines, so where two overlap both still show; the light yellow solid.
        this.coreAlpha = [0.7, 1, 0.7, 0.7];
        // Same as the Traffic widget: 20 s across, drawn 2 s behind.
        this.windowMillis = 20000;
        this.delayMillis = 2000;
        this.charts = [];
    }

    _rgba(hex, alpha) {
        const n = parseInt(hex.slice(1), 16);
        return `rgba(${n >> 16}, ${(n >> 8) & 255}, ${n & 255}, ${alpha})`;
    }

    // Keep the time across the full width fixed, whatever the width.
    _fitTime() {
        this.charts.forEach(({smoothie, canvas}) => {
            const width = canvas.clientWidth;
            if (width > 0) {
                smoothie.options.millisPerPixel = this.windowMillis / width;
            }
        });
    }

    _createChart(selector, series) {
        let smoothie = new SmoothieChart({
            responsive: true,
            millisPerPixel: 50,
            tooltip: true,
            // A little room past 0 and 100 so a line there isn't cut in half.
            minValue: -1,
            maxValue: 104,
            yMinFormatter: () => '0',
            yMaxFormatter: () => '100',
            labels: {
                fillStyle: Chart.defaults.color,
                precision: 0,
                fontSize: 11
            },
            grid: {
                strokeStyle: 'rgba(119,119,119,0.12)',
                verticalSections: 4,
                millisPerLine: 1000,
                fillStyle: 'transparent'
            }
        });

        const canvas = document.getElementById(selector);
        smoothie.streamTo(canvas, this.delayMillis);
        series.forEach(([timeSeries, color, width]) => {
            smoothie.addTimeSeries(timeSeries, {lineWidth: width, strokeStyle: color});
        });
        this.charts.push({smoothie, canvas});
    }

    async getWidgetOptions() {
        return {
            graphs: {
                title: this.translations.graphs,
                type: 'select_multiple',
                options: this.graphs.map((value) => {
                    return {
                        value: value,
                        label: this.translations[value]
                    }
                }),
                default: ['cores']
            }
        }
    }

    async onWidgetOptionsChanged(options) {
        this.graphs.filter(x => !options.graphs.includes(x)).forEach(graph => $(`#monocpu-${graph}`).hide());
        const config = await this.getWidgetConfig();
        config.graphs.forEach(graph => $(`#monocpu-${graph}`).show());
        this._fitTime();
    }

    getMarkup() {
        const graph = (id, height, legend = '') => `
            <div id="monocpu-${id}" class="smoothie-container">
                <b>${this.translations[id]}</b>${legend}
                <div><canvas id="monocpu-usage-${id}" style="width: 100%; height: ${height}px;"></canvas></div>
            </div>`;
        const legend = this.coreColors.map((color, i) =>
            `<span style="margin-left: 10px; white-space: nowrap;">` +
            `<span style="display: inline-block; width: 10px; height: 10px; border-radius: 50%; background: ${color};"></span> ` +
            `${this.translations.cpu} ${i}</span>`).join('');

        return $(`
        <div class="monocpu-type"></div>
        <div class="cpu-canvas-container">
            ${graph('cores', 140, legend)}
            ${graph('total', 50)}
            ${graph('intr', 50)}
            ${graph('user', 50)}
            ${graph('sys', 50)}
        </div>`);
    }

    async onMarkupRendered() {
        const data = await this.ajaxCall('/api/diagnostics/cpu_usage/getcputype');
        $('.monocpu-type').text(data);

        const config = await this.getWidgetConfig();
        this.graphs.forEach((graph) => {
            if (!config.graphs.includes(graph)) {
                $(`#monocpu-${graph}`).hide();
            }
        });

        const cores = this.coreColors.map(() => new TimeSeries());
        this._createChart('monocpu-usage-cores', cores.map((ts, i) => [ts, this._rgba(this.coreColors[i], this.coreAlpha[i]), 2]));
        let ts = {};
        this.graphs.filter(graph => graph !== 'cores').forEach((graph) => {
            ts[graph] = new TimeSeries();
            this._createChart(`monocpu-usage-${graph}`, [[ts[graph], '#d94f00', 3]]);
        });

        this.series = cores.concat(Object.values(ts));
        this._fitTime();

        super.openEventSource('/api/mono/status/cpu_stream', (event) => {
            if (!event) {
                super.closeEventSource();
            }
            const data = JSON.parse(event.data);
            const date = Date.now();
            (data.cores || []).forEach((value, i) => {
                if (i < cores.length) {
                    cores[i].append(date, value);
                }
            });
            Object.keys(ts).forEach((graph) => {
                ts[graph].append(date, data[graph]);
            });
        });
    }

    onVisibilityChanged(visible) {
        // Nothing was sampled while hidden. Start the lines again instead of
        // drawing a curve across the gap to the first new sample.
        if (visible) {
            (this.series || []).forEach(ts => ts.clear());
        }
        super.onVisibilityChanged(visible);
    }

    onWidgetResize(elem, width, height) {
        let viewPort = document.getElementsByClassName('page-content-main')[0].getBoundingClientRect().width;
        if (width > (viewPort / 2)) {
            $(elem).find('.cpu-canvas-container').css('flex-direction', 'row');
            $(elem).find('.smoothie-container').css('margin', '0px 10px 0px 10px');
        } else {
            $(elem).find('.cpu-canvas-container').css('flex-direction', 'column');
            $(elem).find('.smoothie-container').css('margin', '0px');
        }
        // After the layout above has settled.
        setTimeout(() => this._fitTime(), 0);

        return true;
    }
}
