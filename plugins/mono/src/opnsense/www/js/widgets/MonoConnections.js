/*
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

export default class MonoConnections extends BaseGaugeWidget {
    constructor(config) {
        super(config);
        this.tickTimeout = 15;
    }

    async onMarkupRendered() {
        // The text is drawn by fitText() below, so it can shrink to fit inside the ring.
        super.createGaugeChart({
            colorMap: ['#5E9C4A', '#D94F00', '#E5E5E5'],
            labels: [this.translations.hardware, this.translations.cpu, ''],
            primaryText: () => '',
            secondaryText: () => false
        });
        this.chart.config.options.plugins.monoText = {};
        this.chart.config.plugins.push({id: 'monoText', afterDatasetsDraw: (chart) => this.fitText(chart)});
        this.chart._plugins.invalidate();
    }

    // Same fonts and spots as the Memory and Disk gauges, made smaller when too wide for the ring's hole.
    fitText(chart) {
        const data = chart.config.data.datasets[0].data;
        if (data.length === 0) {
            return;
        }
        const ctx = chart.ctx;
        const height = chart.height;
        const room = chart.getDatasetMeta(0).data[0].innerRadius * 2 * 0.85;
        const heading = window.getComputedStyle(document.querySelector('h1')).getPropertyValue('font-family');
        const body = window.getComputedStyle(document.body).getPropertyValue('font-family');

        const draw = (text, size, family, y) => {
            ctx.font = `${size}em ${family}`;
            const width = ctx.measureText(text).width;
            if (width > room) {
                ctx.font = `${(size * room / width).toFixed(2)}em ${family}`;
            }
            ctx.fillText(text, Math.round((chart.width - ctx.measureText(text).width) / 2), y);
        };

        ctx.save();
        ctx.textBaseline = 'middle';
        ctx.fillStyle = Chart.defaults.color;
        draw(`${data[0] + data[1]}`, height / 60, heading, height * 0.70);
        draw(`${data[0]} ${this.translations.hw} / ${data[1]} ${this.translations.cpu}`, height / 90, body, height * 0.90);
        ctx.restore();
    }

    async onWidgetTick() {
        const data = await this.ajaxCall('/api/mono/status/info');
        if (!data) {
            return;
        }

        const hardware = data.connections || 0;
        const cpu = data.cpu_connections || 0;
        // The grey part only fills the ring when there are no connections at all.
        super.updateChart([hardware, cpu, hardware + cpu === 0 ? 1 : 0]);
    }
}
