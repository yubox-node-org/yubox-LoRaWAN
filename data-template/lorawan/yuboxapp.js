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
        const span_connstatus = pane.querySelector('form span#lorawan_connstatus');
        span_connstatus.classList.remove('badge-success', 'badge-danger');
        span_connstatus.classList.add('badge-secondary');
        span_connstatus.textContent = '(consultando)';

        yuboxFetch('lorawan', 'config.json')
        .then((data) => {
            span_connstatus.classList.remove('badge-danger', 'badge-warning', 'badge-success', 'badge-secondary');
            if (data.netjoined) {
                span_connstatus.classList.add('badge-success');
                span_connstatus.textContent = 'CONECTADO';
            } else {
                span_connstatus.classList.add('badge-danger');
                span_connstatus.textContent = 'NO CONECTADO';
            }

            [
                ['input#region',            data.region],
                ['input#subband',           data.subband],
                ['input#deviceEUI_ESP32',   lorawan_formatEUI(data.deviceEUI_ESP32)],
                ['input#deviceEUI',         lorawan_formatEUI((data.deviceEUI == undefined) ? data.deviceEUI_ESP32 : data.deviceEUI)],
                ['input#appEUI',            (data.appEUI == undefined) ? '' : lorawan_formatEUI(data.appEUI)],
                ['input#appKey',            (data.appKey == undefined) ? '' : lorawan_formatEUI(data.appKey)],
                ['input#tx_duty_sec',       data.tx_duty_sec]
            ].forEach(t => pane.querySelector(t[0]).value = t[1]);
        }, (e) => { yuboxStdAjaxFailHandler(e); });

        if (!!window.EventSource) {
            let sse = new EventSource(yuboxAPI('lorawan')+'/status');
            sse.addEventListener('message', function (e) {
                let data = JSON.parse(e.data);

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

                [ 'rx', 'tx_ok', 'tx_fail' ]
                .forEach(k => pane.querySelector('div.lorawan-stats#'+k).textContent = lorawan_format_tsactivity(data[k], data.ts));
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

    pane.querySelector('button[name=apply]').addEventListener('click', function () {
        let postData = {
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