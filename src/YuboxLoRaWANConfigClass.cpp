#include <Arduino.h>

#include "YuboxWebAuthClass.h"

#include <lorawan.h>
#include <SPI.h>
#include "YuboxLoRaWANConfigClass.h"

#include <functional>

#include <Preferences.h>

#define ARDUINOJSON_USE_LONG_LONG 1

#include "AsyncJson.h"
#include "ArduinoJson.h"

#define JOINREQ_NBTRIALS 3         /**< Number of trials for the join request. */

// Constantes para estado de join de red
#define YBX_JOIN_FAIL       -1
#define YBX_JOIN_NONE       0
#define YBX_JOIN_ONGOING    1
#define YBX_JOIN_SUCCESS    2

typedef enum {
  YBX_LW_NETJOIN = 0,
  YBX_LW_RX = 1,

  YBX_LW_EVENT_MAX = 255
} yuboxlorawan_event_t;

typedef struct YuboxLoRaWAN_rx_List
{
  static yuboxlorawan_event_id_t current_id;
  yuboxlorawan_event_t event_type;
  yuboxlorawan_event_id_t id;
  YuboxLoRaWAN_join_cb j_cb;
  YuboxLoRaWAN_join_func_cb j_fcb;
  YuboxLoRaWAN_rx_cb rx_cb;
  YuboxLoRaWAN_rx_func_cb rx_fcb;

  YuboxLoRaWAN_rx_List() : event_type(YBX_LW_EVENT_MAX), id(current_id++), j_cb(NULL), j_fcb(NULL), rx_cb(NULL), rx_fcb(NULL) {}
} YuboxLoRaWAN_rx_List_t;
yuboxlorawan_event_id_t YuboxLoRaWAN_rx_List::current_id = 1;

static std::vector<YuboxLoRaWAN_rx_List_t> cbRXList;

const char * YuboxLoRaWANConfigClass::_ns_nvram_yuboxframework_lorawan = "YUBOX/LoRaWAN";

YuboxLoRaWANConfigClass::YuboxLoRaWANConfigClass(void)
{
    _lw_subband = 1;
    memset(_lw_devEUI, 0, sizeof(_lw_devEUI));
    memset(_lw_appEUI, 0, sizeof(_lw_appEUI));
    memset(_lw_appKey, 0, sizeof(_lw_appKey));
    memset(_lw_default_devEUI, 0, sizeof(_lw_default_devEUI));
    _lw_confExists = false;
    _lw_needsInit = true;
    _join_status = YBX_JOIN_NONE;
    _ts_ultimoJoin_FAIL = 0;

    uint64_t uniqueId = ESP.getEfuseMac();
    for (auto i = 0; i < 8; i++) {
        _lw_default_devEUI[i] = (uint8_t)(uniqueId);
        uniqueId >>= 8;
    }

    _ts_errorAfterJoin = 0;
    _pEvents = NULL;
    _ts_ultimoTX_OK = 0;
    _ts_ultimoTX_FAIL = 0;
    _ts_ultimoRX = 0;
}

// Las constantes RST_LoRa, DIO[0,1,2] se definen para el board heltec_wifi_lora_32_V2
// La definición siguiente DEBE EXISTIR para que el programa compile con la biblioteca Beelan-LoRaWAN
const sRFM_pins RFM_pins = {
  .CS = SS,
  .RST = RST_LoRa,
  .DIO0 = DIO0,
  .DIO1 = DIO1,
  .DIO2 = DIO2,
  .DIO5 = -1, // NOOP
};

void YuboxLoRaWANConfigClass::begin(AsyncWebServer & srv)
{
    _loadSavedCredentialsFromNVRAM();
    _setupHTTPRoutes(srv);

    if (!lora.init()) {
        ESP_LOGE(__FILE__, "lora_hardware_init failed\r\n");
    }
}

void YuboxLoRaWANConfigClass::_loadSavedCredentialsFromNVRAM(void)
{
    Preferences nvram;
    bool ok = true;

    // Para cada una de las preferencias, si no está seteada se obtendrá cadena vacía
    nvram.begin(_ns_nvram_yuboxframework_lorawan, true);

#define LWPARAM_LOAD(P) \
    if (nvram.getBytesLength(#P) >= sizeof(_lw_##P)) {\
        nvram.getBytes(#P, _lw_##P, sizeof(_lw_##P));\
    } else {\
        ok = false;\
    }

    LWPARAM_LOAD(devEUI)
    LWPARAM_LOAD(appEUI)
    LWPARAM_LOAD(appKey)
    _lw_subband = nvram.getUChar("subband", 1);

    _lw_confExists = ok;
}

void YuboxLoRaWANConfigClass::_setupHTTPRoutes(AsyncWebServer & srv)
{
  srv.on("/yubox-api/lorawan/config.json", HTTP_GET, std::bind(&YuboxLoRaWANConfigClass::_routeHandler_yuboxAPI_lorawanconfigjson_GET, this, std::placeholders::_1));
  srv.on("/yubox-api/lorawan/config.json", HTTP_POST, std::bind(&YuboxLoRaWANConfigClass::_routeHandler_yuboxAPI_lorawanconfigjson_POST, this, std::placeholders::_1));
  _pEvents = new AsyncEventSource("/yubox-api/lorawan/status");
  YuboxWebAuth.addManagedHandler(_pEvents);
  srv.addHandler(_pEvents);
  _pEvents->onConnect(std::bind(&YuboxLoRaWANConfigClass::_routeHandler_yuboxAPI_lorawan_status_onConnect, this, std::placeholders::_1));
}

String YuboxLoRaWANConfigClass::_reportActivityJSON(void)
{
    DynamicJsonDocument json_doc(JSON_OBJECT_SIZE(5));

    switch (_join_status) {
    case YBX_JOIN_NONE:     json_doc["join"] = "RESET"; break;
    case YBX_JOIN_ONGOING:  json_doc["join"] = "ONGOING"; break;
    case YBX_JOIN_SUCCESS:  json_doc["join"] = "SET"; break;
    case YBX_JOIN_FAIL:     json_doc["join"] = "FAILED"; break;
    }

    if (_ts_ultimoTX_OK != 0) json_doc["tx_ok"] = _ts_ultimoTX_OK; else json_doc["tx_ok"] = (const char *)NULL;
    if (_ts_ultimoTX_FAIL != 0) json_doc["tx_fail"] = _ts_ultimoTX_FAIL; else json_doc["tx_fail"] = (const char *)NULL;
    if (_ts_ultimoRX != 0) json_doc["rx"] = _ts_ultimoRX; else json_doc["rx"] = (const char *)NULL;
    json_doc["ts"] = millis();

    String json_str;
    serializeJson(json_doc, json_str);
    return json_str;
}

void YuboxLoRaWANConfigClass::_routeHandler_yuboxAPI_lorawan_status_onConnect(AsyncEventSourceClient * c)
{
    String json_str = _reportActivityJSON();
    c->send(json_str.c_str());
}

void YuboxLoRaWANConfigClass::_routeHandler_yuboxAPI_lorawanconfigjson_GET(AsyncWebServerRequest * request)
{
  YUBOX_RUN_AUTH(request);
  
  AsyncResponseStream *response = request->beginResponseStream("application/json");
  DynamicJsonDocument json_doc(JSON_OBJECT_SIZE(7));

#if defined(US_915)
  json_doc["region"] = "US 915 MHz";
#elif defined (AU_915)
  json_doc["region"] = "Australia 915 MHz";
#elif defined (AS_923)
  json_doc["region"] = "Asia 923 MHz";
#elif defined (EU_868)
  json_doc["region"] = "Europe 868 MHz";
#else
  #error No LoRaWAN region defined in Config.h
#endif

    String s_default_devEUI = _bin2str(_lw_default_devEUI, sizeof(_lw_default_devEUI));
    String s_devEUI = _lw_confExists ? _bin2str(_lw_devEUI, sizeof(_lw_devEUI)) : s_default_devEUI;
    String s_appEUI = _lw_confExists ? _bin2str(_lw_appEUI, sizeof(_lw_appEUI)) : "";
    String s_appKey = _lw_confExists ? _bin2str(_lw_appKey, sizeof(_lw_appKey)) : "";
    json_doc["deviceEUI_ESP32"] = s_default_devEUI.c_str();
    json_doc["deviceEUI"] = s_devEUI.c_str();
    json_doc["appEUI"] = s_appEUI.c_str();
    json_doc["appKey"] = s_appKey.c_str();
#if defined(SUBND_0)
  json_doc["subband"] = 0;
#elif defined(SUBND_1)
  json_doc["subband"] = 1;
#elif defined(SUBND_2)
  json_doc["subband"] = 2;
#elif defined(SUBND_3)
  json_doc["subband"] = 3;
#elif defined(SUBND_4)
  json_doc["subband"] = 4;
#elif defined(SUBND_5)
  json_doc["subband"] = 5;
#elif defined(SUBND_6)
  json_doc["subband"] = 6;
#elif defined(SUBND_1)
  json_doc["subband"] = 7;
#else
  // No hay sub-banda para región
  json_doc["subband"] = 0;
#endif

  json_doc["netjoined"] = (_join_status == YBX_JOIN_SUCCESS);

    serializeJson(json_doc, *response);
    request->send(response);
}

void YuboxLoRaWANConfigClass::_routeHandler_yuboxAPI_lorawanconfigjson_POST(AsyncWebServerRequest * request)
{
    YUBOX_RUN_AUTH(request);

    bool clientError = false;
    bool serverError = false;
    String responseMsg = "";
    AsyncWebParameter * p;

    uint32_t n_subband;
    uint8_t n_deviceEUI[8];
    uint8_t n_appEUI[8];
    uint8_t n_appKey[16];

    if (!clientError && !request->hasParam("subband", true)) {
        clientError = true;
        responseMsg = "Sub-banda no presente";
    }
    if (!clientError) {
        p = request->getParam("subband", true);

        if (0 >= sscanf(p->value().c_str(), "%ld", &n_subband)) {
            clientError = true;
            responseMsg = "Sub-banda no numérico";
        } else if (!(n_subband >= 1 && n_subband <= 8)) {
            clientError = true;
            responseMsg = "Sub-banda no está en rango 1..8";
        }
    }

#define LWPARAM_SCAN(P, R) \
    if (!clientError && R && !request->hasParam(#P, true)) {\
        clientError = true;\
        responseMsg = "EUI no está presente: " #P;\
    }\
    if (!clientError) {\
        memset(n_##P, 0, sizeof(n_##P));\
        if (request->hasParam(#P, true)) {\
            p = request->getParam(#P, true);\
            if (p->value().length() != 2 * sizeof(n_##P)) {\
                clientError = true;\
                responseMsg = "EUI de longitud incorrecta (" #P "), se esperaba ";\
                responseMsg += 2 * sizeof(n_##P);\
            } else if (!_str2bin(p->value().c_str(), n_##P, sizeof(n_##P))) {\
                clientError = true;\
                responseMsg = "EUI contiene caracteres no-hexadecimales: " #P;\
            }\
        }\
    }

    LWPARAM_SCAN(deviceEUI, true)
    LWPARAM_SCAN(appEUI, false)
    LWPARAM_SCAN(appKey, true)

    if (!clientError) {
        memcpy(_lw_devEUI, n_deviceEUI, sizeof(_lw_devEUI));
        memcpy(_lw_appEUI, n_appEUI, sizeof(_lw_appEUI));
        memcpy(_lw_appKey, n_appKey, sizeof(_lw_appKey));
        // No hay soporte de sub-banda en biblioteca
        _lw_subband = n_subband;

        serverError = !_saveCredentialsToNVRAM();
        if (serverError) {
            responseMsg = "No se pueden guardar valores LoRaWAN";
        } else {
            _lw_confExists = true;
            _lw_needsInit = true;
        }
    }

    if (!clientError && !serverError) {
        responseMsg = "Parámetros actualizados correctamente";
    }
    unsigned int httpCode = 200;
    if (clientError) httpCode = 400;
    if (serverError) httpCode = 500;

    AsyncResponseStream *response = request->beginResponseStream("application/json");
    response->setCode(httpCode);
    DynamicJsonDocument json_doc(JSON_OBJECT_SIZE(2));
    json_doc["success"] = !(clientError || serverError);
    json_doc["msg"] = responseMsg.c_str();

    serializeJson(json_doc, *response);
    request->send(response);
}

String YuboxLoRaWANConfigClass::_bin2str(uint8_t * p, size_t n)
{
    String s = "";

    while (n > 0) {
        char buf[4];
        sprintf(buf, "%02x", *p);
        s += buf;
        n--; p++;
    }

    return s;
}

bool YuboxLoRaWANConfigClass::_str2bin(const char * s, uint8_t * p, size_t n)
{
    while (n > 0) {
        char buf[4];
        unsigned int x;

        if (s[0] == '\0') return false; buf[0] = s[0];
        if (s[1] == '\0') return false; buf[1] = s[1];
        buf[2] = '\0';
        if (sscanf(buf, "%x", &x) < 1) return false;
        *p = (uint8_t)x;

        n--; p++; s += 2;
    }
    return true;
}

bool YuboxLoRaWANConfigClass::_saveCredentialsToNVRAM(void)
{
    bool ok = true;
    Preferences nvram;
    nvram.begin(_ns_nvram_yuboxframework_lorawan, false);

    if (ok && !nvram.putUChar("subband", _lw_subband)) ok = false;
    if (ok && !nvram.putBytes("devEUI", _lw_devEUI, sizeof(_lw_devEUI))) ok = false;
    if (ok && !nvram.putBytes("appEUI", _lw_appEUI, sizeof(_lw_appEUI))) ok = false;
    if (ok && !nvram.putBytes("appKey", _lw_appKey, sizeof(_lw_appKey))) ok = false;

    nvram.end();
    return ok;
}

void YuboxLoRaWANConfigClass::update(void)
{
    if (!_lw_confExists) return;

    if (_lw_needsInit) {
        _lw_needsInit = false;
        _join_status = YBX_JOIN_NONE;

        _ts_ultimoJoin_FAIL = 0;
        if (!lora.init()) {
            ESP_LOGE(__FILE__, "lora.init failed\r\n");
            _join_status = YBX_JOIN_FAIL;
            _joinfail_handler();
            return;
        }

        // Set LoRaWAN Class change CLASS_A or CLASS_C
        lora.setDeviceClass(CLASS_A);

        // Set Data Rate
        // NOTA: la biblioteca no soporta ADR
        lora.setDataRate(SF9BW125);

        // Biblioteca no soporta elegir sub-banda en tiempo de ejecución

        // set channel to random
        lora.setChannel(MULTI);

        // Setup the EUIs and Keys
        lora.setDevEUI(_bin2str(_lw_devEUI, sizeof(_lw_devEUI)).c_str());
        lora.setAppEUI(_bin2str(_lw_appEUI, sizeof(_lw_appEUI)).c_str());
        lora.setAppKey(_bin2str(_lw_appKey, sizeof(_lw_appKey)).c_str());

        ESP_LOGI(__FILE__, "Starting join LoRaWAN network...\r\n");
        _join_status = YBX_JOIN_ONGOING;
        _joinstart_handler();

        // TODO: llamada bloqueante por 6 segundos
        if (lora.join()) {
            _join_status = YBX_JOIN_SUCCESS;
            _join_handler();
        } else {
            _join_status = YBX_JOIN_FAIL;
            _ts_ultimoJoin_FAIL = millis();
            _joinfail_handler();
        }
    } else {
        if (_join_status == YBX_JOIN_SUCCESS) {
            lora.update();

            // No hay un callback para recepción
            // No hay manera de constreñir la máxima longitud de búfer
            uint8_t rxbuffer[MAX_DOWNLINK_PAYLOAD_SIZE];
            uint8_t recvLen = lora.readData((char *)rxbuffer);
            if (recvLen > 0) {
                _rx_handler(rxbuffer, recvLen);
            }
        } else {
            if (_join_status == YBX_JOIN_FAIL && _ts_ultimoJoin_FAIL != 0 && millis() - _ts_ultimoJoin_FAIL >= 10000) {
                ESP_LOGI(__FILE__, "Starting join LoRaWAN network...\r\n");
                _join_status = YBX_JOIN_ONGOING;
                _joinstart_handler();

                // TODO: llamada bloqueante por 6 segundos
                if (lora.join()) {
                    _join_status = YBX_JOIN_SUCCESS;
                    _join_handler();
                } else {
                    _join_status = YBX_JOIN_FAIL;
                    _ts_ultimoJoin_FAIL = millis();
                    _joinfail_handler();
                }
            }
        }
    }
}

void YuboxLoRaWANConfigClass::_joinstart_handler(void)
{
    if (_pEvents != NULL && _pEvents->count() > 0) {
        String json_str = _reportActivityJSON();
        _pEvents->send(json_str.c_str());
    }
}

void YuboxLoRaWANConfigClass::_joinfail_handler(void)
{
    if (_pEvents != NULL && _pEvents->count() > 0) {
        String json_str = _reportActivityJSON();
        _pEvents->send(json_str.c_str());
    }
}

bool YuboxLoRaWANConfigClass::isJoined(void)
{
    return (YBX_JOIN_SUCCESS == _join_status);
}

#ifndef LORAWAN_APP_PORT
#define LORAWAN_APP_PORT 2
#endif

bool YuboxLoRaWANConfigClass::send(uint8_t * p, uint8_t n, bool is_txconfirmed)
{
    if (!_lw_confExists || _lw_needsInit) return false;

    if (!isJoined()) return false;

    // No tengo indicación de error para sendUplink()
    if (n > MAX_UPLINK_PAYLOAD_SIZE) return false;
    lora.sendUplink((char *)p, n, is_txconfirmed ? 1 : 0, LORAWAN_APP_PORT);

    _ts_errorAfterJoin = 0;
    _ts_ultimoTX_OK = millis();

    if (_pEvents != NULL && _pEvents->count() > 0) {
        String json_str = _reportActivityJSON();
        _pEvents->send(json_str.c_str());
    }

    return true;
}

yuboxlorawan_event_id_t YuboxLoRaWANConfigClass::onJoin(YuboxLoRaWAN_join_cb cbJ)
{
  if (!cbJ) return 0;

  YuboxLoRaWAN_rx_List_t n_evHandler;
  n_evHandler.event_type = YBX_LW_NETJOIN;
  n_evHandler.j_cb = cbJ;
  n_evHandler.j_fcb = NULL;
  cbRXList.push_back(n_evHandler);
  return n_evHandler.id;
}

yuboxlorawan_event_id_t YuboxLoRaWANConfigClass::onJoin(YuboxLoRaWAN_join_func_cb cbJ)
{
  if (!cbJ) return 0;

  YuboxLoRaWAN_rx_List_t n_evHandler;
  n_evHandler.event_type = YBX_LW_NETJOIN;
  n_evHandler.j_cb = NULL;
  n_evHandler.j_fcb = cbJ;
  cbRXList.push_back(n_evHandler);
  return n_evHandler.id;
}

void YuboxLoRaWANConfigClass::removeJoin(YuboxLoRaWAN_join_cb cbJ)
{
  if (!cbJ) return;

  for (auto i = 0; i < cbRXList.size(); i++) {
    YuboxLoRaWAN_rx_List_t entry = cbRXList[i];
    if (entry.event_type == YBX_LW_NETJOIN && entry.j_cb == cbJ) cbRXList.erase(cbRXList.begin() + i);
  }
}

void YuboxLoRaWANConfigClass::removeJoin(yuboxlorawan_event_id_t id)
{
  for (auto i = 0; i < cbRXList.size(); i++) {
    YuboxLoRaWAN_rx_List_t entry = cbRXList[i];
    if (entry.event_type == YBX_LW_NETJOIN && entry.id == id) cbRXList.erase(cbRXList.begin() + i);
  }
}

yuboxlorawan_event_id_t YuboxLoRaWANConfigClass::onRX(YuboxLoRaWAN_rx_cb cbRX)
{
  if (!cbRX) return 0;

  YuboxLoRaWAN_rx_List_t n_evHandler;
  n_evHandler.event_type = YBX_LW_RX;
  n_evHandler.rx_cb = cbRX;
  n_evHandler.rx_fcb = NULL;
  cbRXList.push_back(n_evHandler);
  return n_evHandler.id;
}

yuboxlorawan_event_id_t YuboxLoRaWANConfigClass::onRX(YuboxLoRaWAN_rx_func_cb cbRX)
{
  if (!cbRX) return 0;

  YuboxLoRaWAN_rx_List_t n_evHandler;
  n_evHandler.event_type = YBX_LW_RX;
  n_evHandler.rx_cb = NULL;
  n_evHandler.rx_fcb = cbRX;
  cbRXList.push_back(n_evHandler);
  return n_evHandler.id;
}

void YuboxLoRaWANConfigClass::removeRX(YuboxLoRaWAN_rx_cb cbRX)
{
  if (!cbRX) return;

  for (auto i = 0; i < cbRXList.size(); i++) {
    YuboxLoRaWAN_rx_List_t entry = cbRXList[i];
    if (entry.event_type == YBX_LW_RX && entry.rx_cb == cbRX) cbRXList.erase(cbRXList.begin() + i);
  }
}

void YuboxLoRaWANConfigClass::removeRX(yuboxlorawan_event_id_t id)
{
  for (auto i = 0; i < cbRXList.size(); i++) {
    YuboxLoRaWAN_rx_List_t entry = cbRXList[i];
    if (entry.event_type == YBX_LW_RX && entry.id == id) cbRXList.erase(cbRXList.begin() + i);
  }
}

void YuboxLoRaWANConfigClass::_join_handler(void)
{
    if (_pEvents != NULL && _pEvents->count() > 0) {
        String json_str = _reportActivityJSON();
        _pEvents->send(json_str.c_str());
    }

    for (auto i = 0; i < cbRXList.size(); i++) {
        YuboxLoRaWAN_rx_List_t entry = cbRXList[i];
        if (entry.event_type == YBX_LW_NETJOIN && (entry.j_cb || entry.j_fcb)) {
            if (entry.j_cb) {
                entry.j_cb();
            } else if (entry.j_fcb) {
                entry.j_fcb();
            }
        }
    }
}

void YuboxLoRaWANConfigClass::_rx_handler(uint8_t * p, uint8_t n)
{
    _ts_ultimoRX = millis();
    for (auto i = 0; i < cbRXList.size(); i++) {
        YuboxLoRaWAN_rx_List_t entry = cbRXList[i];
        if (entry.event_type == YBX_LW_RX && (entry.rx_cb || entry.rx_fcb)) {
            if (entry.rx_cb) {
                entry.rx_cb(p, n);
            } else if (entry.rx_fcb) {
                entry.rx_fcb(p, n);
            }
        }
    }

    if (_pEvents != NULL && _pEvents->count() > 0) {
        String json_str = _reportActivityJSON();
        _pEvents->send(json_str.c_str());
    }
}

YuboxLoRaWANConfigClass YuboxLoRaWANConf;