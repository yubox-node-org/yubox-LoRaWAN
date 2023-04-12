function setupLoRaWANTab()
{
    const pane = getYuboxPane('lorawan', true);
    pane.data = {
        'sse': null
    };

    pane.querySelectorAll('input#deviceEUI, input#appEUI, input#appKey')
    .forEach(el => el.addEventListener('blur', function (ev) {
        this.value = lorawan_formatEUI(this.value);
    }));

    pane.querySelector('button#deveui_random').addEventListener('click', function () {
        pane.querySelector('input#deviceEUI').value = lorawan_formatEUI(lorawan_genEUI(8));
    });
    pane.querySelector('button#deveui_fetch_mac').addEventListener('click', function () {
        pane.querySelector('input#deviceEUI').value = pane.querySelector('input#deviceEUI_ESP32').value;
    });
    pane.querySelector('button#appkey_random').addEventListener('click', function () {
        pane.querySelector('input#appKey').value = lorawan_formatEUI(lorawan_genEUI(16));
    });
    pane.querySelector('button#appkey_toggle').addEventListener('click', function () {
        const txt_appKey = pane.querySelector('input#appKey');
        const btn_toggle = pane.querySelector('button#appkey_toggle');
        const btn_appkeyrandom = pane.querySelector('button#appkey_random');
        const svg_eye_show = btn_toggle.querySelector('svg.eye-show');
        const svg_eye_hide = btn_toggle.querySelector('svg.eye-hide')

        if (txt_appKey.type == 'password') {
            // Se debe mostrar clave
            svg_eye_show.style.display = 'none';
            svg_eye_hide.style.display = '';
            txt_appKey.type = 'text';
            btn_appkeyrandom.style.display = '';
        } else {
            // Se debe ocultar clave
            svg_eye_hide.style.display = 'none';
            svg_eye_show.style.display = '';
            txt_appKey.type = 'password';
            btn_appkeyrandom.style.display = 'none';
        }
    });

    // https://getbootstrap.com/docs/4.4/components/navs/#events
    getYuboxNavTab('lorawan')
    .on('shown.bs.tab', function (e) {
        const sel_region = pane.querySelector('select#region');
        const span_connstatus = pane.querySelector('form span#lorawan_connstatus');
        const span_txconfstatus = pane.querySelector('form span#lorawan_txconfstatus');
        span_connstatus.classList.remove('badge-success', 'badge-danger');
        span_connstatus.classList.add('badge-secondary');
        span_connstatus.textContent = '(consultando)';
        span_txconfstatus.classList.remove('badge-secondary', 'badge-info');
        span_txconfstatus.classList.add('badge-secondary');
        span_txconfstatus.textContent = '(consultando)';

        const lw_updatestatus = (data) => {
            span_connstatus.classList.remove('badge-danger', 'badge-warning', 'badge-success', 'badge-secondary');
            const nstatus = {
                'RESET':    ['badge-secondary', 'NO CONECTADO'],
                'FAILED':   ['badge-danger',    'FALLO CONEXIÓN'],
                'ONGOING':  ['badge-warning',   'CONECTANDO...'],
                'SET':      ['badge-success',   'CONECTADO']
            };
            if (data.join in nstatus) {
                span_connstatus.classList.add(nstatus[data.join][0]);
                span_connstatus.textContent = nstatus[data.join][1]
            }

            if ('confirmtx_start' in data) {
                span_txconfstatus.classList.remove('badge-secondary', 'badge-info');
                if (data.confirmtx_start == null) {
                    span_txconfstatus.classList.add('badge-secondary');
                    span_txconfstatus.textContent = 'INACTIVO';
                } else {
                    span_txconfstatus.classList.add('badge-info');
                    span_txconfstatus.textContent = 'EN PROCESO';
                }
            }
        };

        yuboxFetch('lorawan', 'regions.json')
        .then((data) => {
            const prev_region = sel_region.value;
            while (sel_region.firstChild) sel_region.removeChild(sel_region.firstChild);
            for (var i = 0; i < data.length; i++) {
                let opt = document.createElement('option');
                opt.value = data[i].id;
                opt.textContent = data[i].name;
                opt.data = data[i];
                sel_region.appendChild(opt);
            }
            sel_region.value = (prev_region == '') ? data[0].id : prev_region;
            sel_region.dispatchEvent(new Event('change'));

            return yuboxFetch('lorawan', 'config.json');
        }).then((data) => {
            lw_updatestatus(data);

            [
                ['select#region',           data.region],
                ['input#subband',           data.subband],
                ['input#deviceEUI_ESP32',   lorawan_formatEUI(data.deviceEUI_ESP32)],
                ['input#deviceEUI',         lorawan_formatEUI((data.deviceEUI == undefined) ? data.deviceEUI_ESP32 : data.deviceEUI)],
                ['input#appEUI',            (data.appEUI == undefined) ? '' : lorawan_formatEUI(data.appEUI)],
                ['input#appKey',            (data.appKey == undefined) ? '' : lorawan_formatEUI(data.appKey)],
                ['input#tx_duty_sec',       data.tx_duty_sec]
            ].forEach(t => pane.querySelector(t[0]).value = t[1]);

            sel_region.dispatchEvent(new Event('change'));
        }, (e) => { yuboxStdAjaxFailHandler(e); });

        if (!!window.EventSource) {
            let sse = new EventSource(yuboxAPI('lorawan')+'/status');
            sse.addEventListener('message', function (e) {
                let data = JSON.parse(e.data);

                lw_updatestatus(data);

                [ 'rx', 'tx_ok', 'tx_fail', 'confirmtx_start' ]
                .forEach(k => {
                    var div_stats = pane.querySelector('div.lorawan-stats#'+k);
                    if (div_stats != null)
                        div_stats.textContent = lorawan_format_tsactivity(data[k], data.ts);
                    else console.error('No se encuentra selector', 'div.lorawan-stats#'+k);
                });
                [ 'num_confirmtx_ok', 'num_confirmtx_fail' ]
                .forEach(k => {
                    var div_stats = pane.querySelector('div.lorawan-stats#'+k);
                    if (div_stats != null)
                        div_stats.textContent = data[k];
                    else console.error('No se encuentra selector', 'div.lorawan-stats#'+k);
                });
                if (data.confirmtx_start != null) {}
            });
            sse.addEventListener('error', function (e) {
                yuboxMostrarAlertText('danger', 'Se ha perdido conexión con dispositivo para monitoreo LoRaWAN');
            });
            pane.data['sse'] = sse;
        } else {
            yuboxMostrarAlertText('danger', 'Este navegador no soporta Server-Sent Events, no se puede monitorear LoRaWAN.');
        }
    })
    .on('hide.bs.tab', function (e) {
        if (pane.data['sse'] != null) {
            pane.data['sse'].close();
            pane.data['sse'] = null;
        }
    });

    pane.querySelector('select#region').addEventListener('change', function () {
        const opt = this.querySelector('option[value="'+this.value+'"]');
        const txt_subband = pane.querySelector('input#subband');
        txt_subband.max = opt.data.max_sb;

        const subband = parseInt(txt_subband.value);
        if (isNaN(subband) || subband < 1) {
            txt_subband.value = 1;
        } else if (subband > opt.data.max_sb) {
            txt_subband.value = opt.data.max_sb;
        }
    });

    pane.querySelector('button[name=apply]').addEventListener('click', function () {
        let postData = {
            region:     pane.querySelector('select#region').value,
            subband:    pane.querySelector('input#subband').value,
            deviceEUI:  lorawan_unformatEUI(pane.querySelector('input#deviceEUI').value),
            appKey:     lorawan_unformatEUI(pane.querySelector('input#appKey').value),
            tx_duty_sec: pane.querySelector('input#tx_duty_sec').value
        };
        let appeui = lorawan_unformatEUI(pane.querySelector('input#appEUI').value);
        if (appeui != '') postData.appEUI = appeui;
        yuboxFetch('lorawan', 'config.json', postData)
        .then((r) => {
            if (r.success) {
                // Recargar los datos recién guardados del dispositivo
                yuboxMostrarAlertText('success', r.msg, 3000);
            } else {
                yuboxMostrarAlertText('danger', r.msg);
            }
        }, e => yuboxStdAjaxFailHandler(e));
    });
}

function lorawan_format_tsactivity(t, ts_now)
{
    if (t == null) return '--';
    var d = new Date((new Date()).getTime() - (ts_now - t));
    return d.toLocaleString();
}

function lorawan_genEUI(n)
{
    var s = '';
    for (var i = 0; i < n; i++) {
        s += ("0" + Math.floor(Math.random() * 256).toString(16).toUpperCase()).substr(-2);
    }
    return s;
}

function lorawan_formatEUI(s)
{
    s = lorawan_unformatEUI(s);

    var a = [];
    var x = 0; var y = 2;
    while (x < s.length) {
        a.push(s.slice(x, y));
        x += 2; y += 2;
    }
    return a.join(' ');
}

// Quitar todos los espacios de la cadena de texto
function lorawan_unformatEUI(s)
{
    return s.replace(/\s+/g, '');
}