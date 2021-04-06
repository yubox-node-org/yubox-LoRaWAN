<?php
define ('MOCKUP_CONF', '/tmp/lorawan.json');

function mockup_esp32_deveui()
{
    $s = `/usr/sbin/ip addr show eth0 | grep ether`;
    $l = explode(" ", trim($s));
    return str_replace(":", "", strtoupper($l[1]))."0000";
}

Header('Content-Type: application/json');

if (!isset($_SERVER['PATH_INFO'])) {
    Header('HTTP/1.1 400 Bad Request');
    print json_encode(array(
        'success'   =>  FALSE,
        'msg'       =>  'Se requiere indicar ruta'
    ));
    exit();
}

switch ($_SERVER['PATH_INFO']) {
case '/config.json':
    $conf = array(
        'region'            =>  'US915',
        'subband'           =>  1,
        'netjoined'         =>  FALSE,
        'deviceEUI_ESP32'   =>  mockup_esp32_deveui(),
        //'deviceEUI'         =>  '',
        //'appEUI'            =>  '',
        //'appKey'            =>  '',
    );
    if (file_exists(MOCKUP_CONF)) {
        $conf = json_decode(file_get_contents(MOCKUP_CONF), TRUE);
    }
    switch ($_SERVER['REQUEST_METHOD']) {
    case 'GET':
        print json_encode($conf);
        break;
    case 'POST':
        $err = array('success' => FALSE, 'msg' => '');
        if (!isset($_POST['subband']) || !ctype_digit($_POST['subband'])) {
            $err['msg'] = 'Sub-banda no presente o no numérico';
            print json_encode($err);
            break;
        }
        if (!($_POST['subband'] >= 1 && $_POST['subband'] <= 8)) {
            $err['msg'] = 'Subcanal no está en rango 1..8';
            print json_encode($err);
            break;
        }
        $conf['subband'] = (int)$_POST['subband'];

        if (!isset($_POST['appEUI']) || trim($_POST['appEUI']) == '') $_POST['appEUI'] = '0000000000000000';
        foreach (array(
            'deviceEUI' => 8,
            'appEUI' => 8,
            'appKey' => 16,
        ) as $k => $d) {
            if (!isset($_POST[$k]) || trim($_POST[$k]) == '') {
                $err['msg'] = 'EUI no está presente: '.$k;
                print json_encode($err);
                break 2;
            }
            if (strlen($_POST[$k]) != 2 * $d) {
                $err['msg'] = "EUI de longitud incorrecta ($k), se esperaba $d";
                print json_encode($err);
                break 2;
            }
            if (!ctype_xdigit($_POST[$k])) {
                $err['msg'] = "EUI contiene caracteres no-hexadecimales: $k";
                print json_encode($err);
                break 2;
            }
            $conf[$k] = strtoupper($_POST[$k]);
        }

        $conf['netjoined'] = TRUE;
        file_put_contents(MOCKUP_CONF, json_encode($conf));
        $err['success'] = TRUE;
        $err['msg'] = "Parámetros guardados correctamente, se inicia conexión...";
        print json_encode($err);

        break;
    default:
        Header('HTTP/1.1 405 Method Not Allowed');
        Header('Allow: GET, POST');
        print json_encode(array(
            'success'   =>  FALSE,
            'msg'       =>  'Unimplemented request method'
        ));
        exit();
        break;
    }
    break;
default:
    Header('HTTP/1.1 404 Not Found');
    print json_encode(array(
        'success'   =>  FALSE,
        'msg'       =>  'El recurso indicado no existe o no ha sido implementado',
    ));
    exit();

}
