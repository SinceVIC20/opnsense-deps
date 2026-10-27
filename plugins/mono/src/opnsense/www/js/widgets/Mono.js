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

export default class Mono extends BaseTableWidget {
    constructor() {
        super();
        this.tickTimeout = 60; // release and offload state rarely change
    }

    getMarkup() {
        let $container = $('<div></div>');
        $container.append(this.createTable('mono-table', {
            headerPosition: 'left',
        }));
        return $container;
    }

    async onWidgetTick() {
        const data = await this.ajaxCall('/api/mono/status/info');
        if (!data) {
            return;
        }

        let release = data.release ? $('<span>').text(data.release).prop('outerHTML') : this.translations.unknown;
        if (data.release) {
            release += ' &nbsp; ' + $('<a>').attr('href', '/ui/core/firmware#status')
                .text(this.translations.status).prop('outerHTML');
        }

        let offload;
        if (data.offload === 'on') {
            offload = `<i class="fa fa-check text-success"></i> <a href="/ui/core/service">` +
                `${this.translations.on}</a>`;
        } else if (data.offload === 'off') {
            offload = `<i class="fa fa-times text-danger"></i> ${this.translations.off} &nbsp; ` +
                `<a href="/ui/core/service">${this.translations.start}</a>`;
        } else if (data.offload === 'unavailable') {
            offload = `<i class="fa fa-times text-danger"></i> ${this.translations.unavailable} &nbsp; ` +
                `<a href="/ui/core/reboot">${this.translations.reboot}</a>`;
        } else {
            offload = this.translations.unknown;
        }

        let server = this.translations.default;
        if (data.server_url) {
            server = $('<a>').attr({href: data.server_url, target: '_blank', rel: 'noopener noreferrer'})
                .text(data.server).prop('outerHTML');
        } else if (data.server) {
            server = $('<span>').text(data.server).prop('outerHTML');
        }

        super.updateTable('mono-table', [
            [this.translations.release, release],
            [this.translations.offload, offload],
            [this.translations.server, server],
        ]);
    }
}
