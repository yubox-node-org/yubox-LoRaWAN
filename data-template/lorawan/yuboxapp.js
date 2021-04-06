function setupLoRaWANTab()
{
    var lorawanpane = getYuboxPane('lorawan');

    lorawanpane.find('input#deviceEUI, input#appEUI, input#appKey').blur(function() {
        var txt = $(this);
        txt.val(lorawan_formatEUI(txt.val()));
    });

    lorawanpane.find('button#deveui_random').click(function () {
        var lorawanpane = getYuboxPane('lorawan');

        lorawanpane.find('input#deviceEUI').val(lorawan_formatEUI(lorawan_genEUI(8)));
    });
    lorawanpane.find('button#deveui_fetch_mac').click(function () {
        var lorawanpane = getYuboxPane('lorawan');

        lorawanpane.find('input#deviceEUI').val(lorawanpane.find('input#deviceEUI_ESP32').val());
    });
    lorawanpane.find('button#appkey_random').click(function () {
        var lorawanpane = getYuboxPane('lorawan');

        lorawanpane.find('input#appKey').val(lorawan_formatEUI(lorawan_genEUI(16)));
    });
    lorawanpane.find('button#appkey_toggle').click(function () {
        var lorawanpane = getYuboxPane('lorawan');
        var txt_appKey = lorawanpane.find('input#appKey');
        var btn_toggle = lorawanpane.find('button#appkey_toggle');
        var btn_appkeyrandom = lorawanpane.find('button#appkey_random');

        if (txt_appKey.attr('type') == 'password') {
            // Se debe mostrar clave
            btn_toggle.find('svg.eye-show').hide();
            btn_toggle.find('svg.eye-hide').show();
            txt_appKey.attr('type', 'text');
            btn_appkeyrandom.show();
        } else {
            // Se debe ocultar clave
            btn_toggle.find('svg.eye-hide').hide();
            btn_toggle.find('svg.eye-show').show();
            txt_appKey.attr('type', 'password');
            btn_appkeyrandom.hide();
        }
    });

    // https://getbootstrap.com/docs/4.4/components/navs/#events
    getYuboxNavTab('lorawan')
    .on('shown.bs.tab', function (e) {
        lorawanpane.find('form span#lorawan_connstatus')
            .removeClass('badge-success badge-danger')
            .addClass('badge-secondary')
            .text('(consultando)');

        $.get(yuboxAPI('lorawan')+'/config.json')
        .done(function (data) {
            var lorawanpane = getYuboxPane('lorawan');
            var span_connstatus = lorawanpane.find('form span#lorawan_connstatus');

            span_connstatus.removeClass('badge-danger badge-success badge-secondary');
            if (data.netjoined) {
                span_connstatus
                    .addClass('badge-success')
                    .text('CONECTADO');
            } else {
                span_connstatus
                    .addClass('badge-danger')
                    .text('NO CONECTADO');
            }

            lorawanpane.find('input#region').val(data.region);
            lorawanpane.find('input#subband').val(data.subband);
            // data.netjoined
            lorawanpane.find('input#deviceEUI_ESP32').val(lorawan_formatEUI(data.deviceEUI_ESP32));
            lorawanpane.find('input#deviceEUI').val(lorawan_formatEUI((data.deviceEUI == undefined) ? data.deviceEUI_ESP32 : data.deviceEUI));
            lorawanpane.find('input#appEUI').val((data.appEUI == undefined) ? '' : lorawan_formatEUI(data.appEUI));
            lorawanpane.find('input#appKey').val((data.appKey == undefined) ? '' : lorawan_formatEUI(data.appKey));
        })
        .fail(function (e) { yuboxStdAjaxFailHandler(e); });
    })
    .on('hide.bs.tab', function (e) {
    });

    lorawanpane.find('button[name=apply]').click(function () {
        var lorawanpane = getYuboxPane('lorawan');

        var postData = {
            subband:    lorawanpane.find('input#subband').val(),
            deviceEUI:  lorawan_unformatEUI(lorawanpane.find('input#deviceEUI').val()),
            //appEUI:     lorawan_unformatEUI(lorawanpane.find('input#appEUI').val()),
            appKey:     lorawan_unformatEUI(lorawanpane.find('input#appKey').val())
        };
        var appeui = lorawan_unformatEUI(lorawanpane.find('input#appEUI').val());
        if (appeui != '') postData.appEUI = appeui;
        $.post(yuboxAPI('lorawan')+'/config.json', postData)
        .done(function (r) {
            if (r.success) {
                // Recargar los datos recién guardados del dispositivo
                yuboxMostrarAlertText('success', r.msg, 3000);
            } else {
                yuboxMostrarAlertText('danger', r.msg);
            }
        })
        .fail(function (e) { yuboxStdAjaxFailHandler(e); });
    });
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