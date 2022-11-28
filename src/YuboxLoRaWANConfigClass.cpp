#include <Arduino.h>

#include "YuboxWebAuthClass.h"

#include <LoRaWan-Arduino.h>
#include <SPI.h>
#include "YuboxLoRaWANConfigClass.h"

#include <functional>

#include <Preferences.h>

#define ARDUINOJSON_USE_LONG_LONG 1

#include "AsyncJson.h"
#include "ArduinoJson.h"

#define JOINREQ_NBTRIALS 3         /**< Number of trials for the join request. */

#define LORAWAN_APP_DEFAULT_TX_DUTYCYCLE 10     /* Default number of seconds for duty cycle */

typedef enum {
  YBX_LW_NETJOIN = 0,
  YBX_LW_RX = 1,
  YBX_LW_TXDUTY_CHANGE = 2,

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
  YuboxLoRaWAN_txdutychange_cb txd_cb;
  YuboxLoRaWAN_txdutychange_func_cb txd_fcb;

  YuboxLoRaWAN_rx_List() : event_type(YBX_LW_EVENT_MAX), id(current_id++), j_cb(NULL), j_fcb(NULL), rx_cb(NULL), rx_fcb(NULL), txd_cb(NULL), txd_fcb(NULL) {}
} YuboxLoRaWAN_rx_List_t;
yuboxlorawan_event_id_t YuboxLoRaWAN_rx_List::current_id = 1;

static std::vector<YuboxLoRaWAN_rx_List_t> cbRXList;

const char * YuboxLoRaWANConfigClass::_ns_nvram_yuboxframework_lorawan = "YUBOX/LoRaWAN";

static void lorawan_has_joined_handler(void);
static void lorawan_rx_handler(lmh_app_data_t *app_data);
static void lorawan_confirm_class_handler(DeviceClass_t Class);
static void lorawan_join_failed_handler(void);

YuboxLoRaWANConfigClass::YuboxLoRaWANConfigClass(void)
{
    _lw_region = LORAMAC_REGION_AU915;
    _lw_subband = 1;
    memset(_lw_devEUI, 0, sizeof(_lw_devEUI));
    memset(_lw_appEUI, 0, sizeof(_lw_appEUI));
    memset(_lw_appKey, 0, sizeof(_lw_appKey));
    memset(_lw_default_devEUI, 0, sizeof(_lw_default_devEUI));
    _lw_confExists = false;
    _lw_needsInit = true;
    _lorahw_init = false;

    lmh_callback_t lora_callbacks = {
        BoardGetBatteryLevel,
        BoardGetUniqueId,
        BoardGetRandomSeed,
        lorawan_rx_handler,
        lorawan_has_joined_handler,
        lorawan_confirm_class_handler,
        lorawan_join_failed_handler
    };
    _lora_callbacks = lora_callbacks;

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
    _tx_duty_sec = LORAWAN_APP_DEFAULT_TX_DUTYCYCLE;
    _tx_duty_sec_changed = false;

}

// ESP32 - SX126x pin configuration
const int PIN_LORA_NSS      = SS;   // LORA SPI CS
const int PIN_LORA_SCLK     = SCK;  // LORA SPI CLK
const int PIN_LORA_MISO     = MISO; // LORA SPI MISO
const int PIN_LORA_MOSI     = MOSI; // LORA SPI MOSI
#if CONFIG_IDF_TARGET_ESP32
const int PIN_LORA_RESET    = 33;   // LORA RESET
const int PIN_LORA_BUSY     = 34;   // LORA SPI BUSY
const int PIN_LORA_DIO_1    = 39;   // LORA DIO_1
const int RADIO_TXEN        = 26;   // LORA ANTENNA TX ENABLE
const int RADIO_RXEN        = 27;   // LORA ANTENNA RX ENABLE
#elif CONFIG_IDF_TARGET_ESP32S2
const int PIN_LORA_RESET    = 17;   // LORA RESET
const int PIN_LORA_BUSY     = 18;   // LORA SPI BUSY
const int PIN_LORA_DIO_1    =  8;   // LORA DIO_1
const int RADIO_TXEN        = 16;   // LORA ANTENNA TX ENABLE
const int RADIO_RXEN        = 15;   // LORA ANTENNA RX ENABLE

#elif CONFIG_IDF_TARGET_ESP32S3
const int PIN_LORA_RESET    = 17;   // LORA RESET
const int PIN_LORA_BUSY     = 18;   // LORA SPI BUSY
const int PIN_LORA_DIO_1    =  8;   // LORA DIO_1
const int RADIO_TXEN        = 16;   // LORA ANTENNA TX ENABLE
const int RADIO_RXEN        = 15;   // LORA ANTENNA RX ENABLE
#else
#error Undefined LORA pins for target!
#endif

bool YuboxLoRaWANConfigClass::begin(AsyncWebServer & srv)
{
    _loadSavedCredentialsFromNVRAM();
    _setupHTTPRoutes(srv);

    hw_config hwConfig;
    // Define the HW configuration between MCU and SX126x
    hwConfig.CHIP_TYPE = SX1262_CHIP;      // Example uses an eByte E22 module with an SX1262
    hwConfig.PIN_LORA_RESET = PIN_LORA_RESET; // LORA RESET
    hwConfig.PIN_LORA_NSS = PIN_LORA_NSS;   // LORA SPI CS
    hwConfig.PIN_LORA_SCLK = PIN_LORA_SCLK;   // LORA SPI CLK
    hwConfig.PIN_LORA_MISO = PIN_LORA_MISO;   // LORA SPI MISO
    hwConfig.PIN_LORA_DIO_1 = PIN_LORA_DIO_1; // LORA DIO_1
    hwConfig.PIN_LORA_BUSY = PIN_LORA_BUSY;   // LORA SPI BUSY
    hwConfig.PIN_LORA_MOSI = PIN_LORA_MOSI;   // LORA SPI MOSI
    hwConfig.RADIO_TXEN = RADIO_TXEN;      // LORA ANTENNA TX ENABLE
    hwConfig.RADIO_RXEN = RADIO_RXEN;      // LORA ANTENNA RX ENABLE
    hwConfig.USE_DIO2_ANT_SWITCH = false;
    hwConfig.USE_DIO3_TCXO = true;        // Example uses an CircuitRocks Alora RFM1262 which uses DIO3 to control oscillator voltage
    hwConfig.USE_DIO3_ANT_SWITCH = false;   // Only Insight ISP4520 module uses DIO3 as antenna control

    uint32_t err_code = lora_hardware_init(hwConfig);
    if (err_code != 0) {
        log_e("lora_hardware_init failed - %d", err_code);
    } else {
        _lorahw_init = true;
    }

    return _lorahw_init;
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
    _lw_region = (LoRaMacRegion_t) nvram.getUChar("region", (uint8_t)LORAMAC_REGION_AU915);
    _lw_subband = nvram.getUChar("subband", 1);
    _tx_duty_sec = nvram.getUInt("txduty", LORAWAN_APP_DEFAULT_TX_DUTYCYCLE);

    // Validar si región seleccionada es válida...
    if (!_isValidLoRaWANRegion((uint8_t)_lw_region)) _lw_region = LORAMAC_REGION_AU915;

    // Validar si sub-banda almacenada es válida para región...
    if (_lw_subband < 1) _lw_subband = 1;
    if (_lw_subband > _getMaxLoRaWANRegionSubchannel(_lw_region)) _lw_subband = 1;

    _lw_confExists = ok;
}

bool YuboxLoRaWANConfigClass::_isValidLoRaWANRegion(uint8_t r)
{
    switch (r) {
    case LORAMAC_REGION_AS923:
    case LORAMAC_REGION_AU915:
    case LORAMAC_REGION_CN470:
    case LORAMAC_REGION_CN779:
    case LORAMAC_REGION_EU433:
    case LORAMAC_REGION_EU868:
    case LORAMAC_REGION_KR920:
    case LORAMAC_REGION_IN865:
    case LORAMAC_REGION_US915:
    case LORAMAC_REGION_AS923_2:
    case LORAMAC_REGION_AS923_3:
    case LORAMAC_REGION_AS923_4:
    case LORAMAC_REGION_RU864:
        return true;
    default:
        return false;
    }
}

const char * YuboxLoRaWANConfigClass::_getLoRaWANRegionName(LoRaMacRegion_t region)
{
    switch (region) {
    case LORAMAC_REGION_AS923:
        return "Asia 923 MHz";
    case LORAMAC_REGION_AU915:
        return "Australia 915 MHz";
    case LORAMAC_REGION_CN470:
        return "China 470 MHz";
    case LORAMAC_REGION_CN779:
        return "China 779 MHz";
    case LORAMAC_REGION_EU433:
        return "Europe 433 MHz";
    case LORAMAC_REGION_EU868:
        return "Europe 868 MHz";
    case LORAMAC_REGION_KR920:
        return "Korea 920 MHz";
    case LORAMAC_REGION_IN865:
        return "India 865 MHz";
    case LORAMAC_REGION_US915:
        return "US 915 MHz";
    case LORAMAC_REGION_AS923_2:
        return "Asia 923 MHz variante 2";
    case LORAMAC_REGION_AS923_3:
        return "Asia 923 MHz variante 3";
    case LORAMAC_REGION_AS923_4:
        return "Asia 923 MHz variante 4";
    case LORAMAC_REGION_RU864:
        return "Russia 864 MHz";
    default:
        return "(región no válida o no descrita)";
    }
}

uint8_t YuboxLoRaWANConfigClass::_getMaxLoRaWANRegionSubchannel(LoRaMacRegion_t region)
{
    switch (region) {
    case LORAMAC_REGION_AU915:
    case LORAMAC_REGION_US915:
        return 9;
    case LORAMAC_REGION_CN470:
        return 12;
    case LORAMAC_REGION_CN779:
    case LORAMAC_REGION_EU433:
    case LORAMAC_REGION_EU868:
    case LORAMAC_REGION_KR920:
    case LORAMAC_REGION_IN865:
        return 2;
    case LORAMAC_REGION_AS923:
    case LORAMAC_REGION_AS923_2:
    case LORAMAC_REGION_AS923_3:
    case LORAMAC_REGION_AS923_4:
    case LORAMAC_REGION_RU864:
    default:
        return 1;
    }
}


void YuboxLoRaWANConfigClass::_setupHTTPRoutes(AsyncWebServer & srv)
{
  srv.on("/yubox-api/lorawan/config.json", HTTP_GET, std::bind(&YuboxLoRaWANConfigClass::_routeHandler_yuboxAPI_lorawanconfigjson_GET, this, std::placeholders::_1));
  srv.on("/yubox-api/lorawan/config.json", HTTP_POST, std::bind(&YuboxLoRaWANConfigClass::_routeHandler_yuboxAPI_lorawanconfigjson_POST, this, std::placeholders::_1));
  _pEvents = new AsyncEventSource("/yubox-api/lorawan/status");
  YuboxWebAuth.addManagedHandler(_pEvents);
  srv.addHandler(_pEvents);
  _pEvents->onConnect(std::bind(&YuboxLoRaWANConfigClass::_routeHandler_yuboxAPI_lorawan_status_onConnect, this, std::placeholders::_1));
  srv.on("/yubox-api/lorawan/regions.json", HTTP_GET, std::bind(&YuboxLoRaWANConfigClass::_routeHandler_yuboxAPI_lorawanregionsjson_GET, this, std::placeholders::_1));
}

String YuboxLoRaWANConfigClass::_reportActivityJSON(void)
{
    DynamicJsonDocument json_doc(JSON_OBJECT_SIZE(5));
    switch (lmh_join_status_get()) {
    case LMH_RESET:     json_doc["join"] = "RESET"; break;
    case LMH_SET:       json_doc["join"] = "SET"; break;
    case LMH_ONGOING:   json_doc["join"] = "ONGOING"; break;
    case LMH_FAILED:    json_doc["join"] = "FAILED"; break;
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

void YuboxLoRaWANConfigClass::_routeHandler_yuboxAPI_lorawanregionsjson_GET(AsyncWebServerRequest * request)
{
    YUBOX_RUN_AUTH(request);

    // Cuántas regiones soporta esta versión de la biblioteca?
    uint8_t numRegions = 0;
    while (_isValidLoRaWANRegion(numRegions)) numRegions++;

    AsyncResponseStream *response = request->beginResponseStream("application/json");
    DynamicJsonDocument json_doc(JSON_ARRAY_SIZE(numRegions) + numRegions * JSON_OBJECT_SIZE(3));

    for (auto i = 0; i < numRegions; i++) {
        JsonObject region = json_doc.createNestedObject();
        region["id"] = i;
        region["name"] = _getLoRaWANRegionName((LoRaMacRegion_t)i);
        region["max_sb"] = _getMaxLoRaWANRegionSubchannel((LoRaMacRegion_t)i);
    }

    serializeJson(json_doc, *response);
    request->send(response);
}

void YuboxLoRaWANConfigClass::_routeHandler_yuboxAPI_lorawanconfigjson_GET(AsyncWebServerRequest * request)
{
    YUBOX_RUN_AUTH(request);

    AsyncResponseStream *response = request->beginResponseStream("application/json");
    DynamicJsonDocument json_doc(JSON_OBJECT_SIZE(9));

    json_doc["region"] = (unsigned int)_lw_region;
    String s_default_devEUI = _bin2str(_lw_default_devEUI, sizeof(_lw_default_devEUI));
    String s_devEUI = _lw_confExists ? _bin2str(_lw_devEUI, sizeof(_lw_devEUI)) : s_default_devEUI;
    String s_appEUI = _lw_confExists ? _bin2str(_lw_appEUI, sizeof(_lw_appEUI)) : "";
    String s_appKey = _lw_confExists ? _bin2str(_lw_appKey, sizeof(_lw_appKey)) : "";
    json_doc["deviceEUI_ESP32"] = s_default_devEUI.c_str();
    json_doc["deviceEUI"] = s_devEUI.c_str();
    json_doc["appEUI"] = s_appEUI.c_str();
    json_doc["appKey"] = s_appKey.c_str();
    json_doc["subband"] = _lw_subband;
    switch (lmh_join_status_get()) {
    case LMH_RESET:     json_doc["join"] = "RESET"; break;
    case LMH_SET:       json_doc["join"] = "SET"; break;
    case LMH_ONGOING:   json_doc["join"] = "ONGOING"; break;
    case LMH_FAILED:    json_doc["join"] = "FAILED"; break;
    }
    json_doc["tx_duty_sec"] = getRequestedTXDutyCycle();

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

    uint8_t n_region = (uint8_t)_lw_region;
    uint8_t n_subband = _lw_subband;
    uint8_t n_deviceEUI[8];
    uint8_t n_appEUI[8];
    uint8_t n_appKey[16];

    uint32_t n_tx_duty_sec = getRequestedTXDutyCycle();

    if (!clientError && request->hasParam("region", true)) {
        p = request->getParam("region", true);

        if (0 >= sscanf(p->value().c_str(), "%hhu", &n_region)) {
            clientError = true;
            responseMsg = "ID de región no numérico";
        } else if (!_isValidLoRaWANRegion(n_region)) {
            clientError = true;
            responseMsg = "ID de región no válido o no implementado";
        }
    }

    if (!clientError && request->hasParam("subband", true)) {
        p = request->getParam("subband", true);

        if (0 >= sscanf(p->value().c_str(), "%hhu", &n_subband)) {
            clientError = true;
            responseMsg = "Sub-banda no numérico";
        } else if (!(n_subband >= 1 && n_subband <= _getMaxLoRaWANRegionSubchannel((LoRaMacRegion_t)n_region))) {
            clientError = true;
            responseMsg = "Sub-banda no está en rango requerido para región";
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

    if (!clientError && request->hasParam("tx_duty_sec", true)) {
        p = request->getParam("tx_duty_sec", true);

        if (0 >= sscanf(p->value().c_str(), "%lu", &n_tx_duty_sec)) {
            clientError = true;
            responseMsg = "Intervalo de transmisión no es numérico";
        } else if (n_tx_duty_sec < 10) {
            clientError = true;
            responseMsg = "Intervalo de transmisión debe ser de al menos 10 segundos";
        }
    }

    if (!clientError) {
        memcpy(_lw_devEUI, n_deviceEUI, sizeof(_lw_devEUI));
        memcpy(_lw_appEUI, n_appEUI, sizeof(_lw_appEUI));
        memcpy(_lw_appKey, n_appKey, sizeof(_lw_appKey));
        _lw_region = (LoRaMacRegion_t)n_region;
        _lw_subband = n_subband;

        serverError = !_saveCredentialsToNVRAM();
        if (serverError) {
            responseMsg = "No se pueden guardar valores LoRaWAN";
        } else if (!setRequestedTXDutyCycle(n_tx_duty_sec)) {
            serverError = true;
            responseMsg = "No se puede guardar intervalo de transmisión deseado";
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

    if (ok && !nvram.putUChar("region", (uint8_t)_lw_region)) ok = false;
    if (ok && !nvram.putUChar("subband", _lw_subband)) ok = false;
    if (ok && !nvram.putBytes("devEUI", _lw_devEUI, sizeof(_lw_devEUI))) ok = false;
    if (ok && !nvram.putBytes("appEUI", _lw_appEUI, sizeof(_lw_appEUI))) ok = false;
    if (ok && !nvram.putBytes("appKey", _lw_appKey, sizeof(_lw_appKey))) ok = false;

    nvram.end();
    return ok;
}

bool YuboxLoRaWANConfigClass::setRequestedTXDutyCycle(uint32_t n_txduty)
{
    if (n_txduty <= 0) return false;
    if (n_txduty != _tx_duty_sec) {
        Preferences nvram;
        nvram.begin(_ns_nvram_yuboxframework_lorawan, false);

        if (!nvram.putUInt("txduty", n_txduty)) return false;

        _tx_duty_sec = n_txduty;
        _tx_duty_sec_changed = true;
    }
    return true;
}

void YuboxLoRaWANConfigClass::update(void)
{
    if (!_lw_confExists) return;

    if (_tx_duty_sec_changed) {
        _tx_duty_sec_changed = false;
        _txdutychange_handler();
    }

    if (!_lorahw_init) return;

    if (_lw_needsInit) {
        _lw_needsInit = false;

        // Setup the EUIs and Keys
        lmh_setDevEui(_lw_devEUI);
        lmh_setAppEui(_lw_appEUI);
        lmh_setAppKey(_lw_appKey);

        lmh_param_t lora_param_init = {
            LORAWAN_ADR_ON,
            LORAWAN_DEFAULT_DATARATE,
            LORAWAN_PUBLIC_NETWORK,
            //LORAWAN_PRIVAT_NETWORK,
            JOINREQ_NBTRIALS,
            LORAWAN_DEFAULT_TX_POWER,
            LORAWAN_DUTYCYCLE_OFF
        };

        uint32_t err_code = lmh_init(&_lora_callbacks, lora_param_init, true, CLASS_A, _lw_region);
        if (err_code != 0) {
            log_e("lmh_init failed - %d", err_code);
            _joinfail_handler();
        } else if (!lmh_setSubBandChannels(_lw_subband)) {
            log_e("lmh_setSubBandChannels(%d) failed. Wrong sub band requested?", _lw_subband);
            _joinfail_handler();
        } else {
            log_i("Starting join LoRaWAN network (region %s subband %d)...",
                _getLoRaWANRegionName(_lw_region), _lw_subband);
            lmh_join();
            _joinstart_handler();
        }
    } else {
        // En versión 2.0.0+ el proceso de IRQ se mueve a tarea separada
        //Radio.IrqProcess();
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
    return (_lorahw_init && (LMH_SET == lmh_join_status_get()));
}

bool YuboxLoRaWANConfigClass::send(uint8_t * p, uint8_t n, bool is_txconfirmed)
{
    if (!_lorahw_init) return false;
    if (!_lw_confExists || _lw_needsInit) return false;

    if (lmh_join_status_get() != LMH_SET) return false;

    if (p == NULL) n = 0;
    lmh_app_data_t m_lora_app_data = {p, n, LORAWAN_APP_PORT, 0, 0};

    lmh_error_status main_err = lmh_send(&m_lora_app_data, is_txconfirmed ? LMH_CONFIRMED_MSG : LMH_UNCONFIRMED_MSG);

    if (main_err != LMH_SUCCESS) {
        uint32_t t = millis();
        _ts_ultimoTX_FAIL = t;
        if (_ts_errorAfterJoin == 0) _ts_errorAfterJoin = t;

        if (t - _ts_errorAfterJoin >= 90 * 1000) {
            log_w("No hay transmisión exitosa luego de timeout, se reintenta join...");
            _ts_errorAfterJoin = 0;
            _lw_needsInit = true;
        } else if (p != NULL) {
            /**
             * NOTA: por Alex Villacís Lasso 2020/11/24
             * En caso de que falle el envío, probablemente es porque no se ha negociado
             * todavía una longitud máxima de payload, lo cual depende de la calidad de radio.
             * El mecanismo de negociación consiste en intentar enviar un paquete LoRaWAN de
             * longitud cero, lo cual permite negociar un payload mayor según se reciba o no
             * en el gateway.
             */
            m_lora_app_data.port = LORAWAN_APP_PORT;
            m_lora_app_data.buffsize = 0;
            lmh_error_status recv_err = lmh_send(&m_lora_app_data, LMH_UNCONFIRMED_MSG);
        }
    } else {
        _ts_errorAfterJoin = 0;
        _ts_ultimoTX_OK = millis();
    }

    if (_pEvents != NULL && _pEvents->count() > 0) {
        String json_str = _reportActivityJSON();
        _pEvents->send(json_str.c_str());
    }

    return (main_err == LMH_SUCCESS);
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

yuboxlorawan_event_id_t YuboxLoRaWANConfigClass::onTXDuty(YuboxLoRaWAN_txdutychange_cb cb)
{
  if (!cb) return 0;

  YuboxLoRaWAN_rx_List_t n_evHandler;
  n_evHandler.event_type = YBX_LW_TXDUTY_CHANGE;
  n_evHandler.txd_cb = cb;
  n_evHandler.txd_fcb = NULL;
  cbRXList.push_back(n_evHandler);
  return n_evHandler.id;
}

yuboxlorawan_event_id_t YuboxLoRaWANConfigClass::onTXDuty(YuboxLoRaWAN_txdutychange_func_cb cb)
{
  if (!cb) return 0;

  YuboxLoRaWAN_rx_List_t n_evHandler;
  n_evHandler.event_type = YBX_LW_TXDUTY_CHANGE;
  n_evHandler.txd_cb = NULL;
  n_evHandler.txd_fcb = cb;
  cbRXList.push_back(n_evHandler);
  return n_evHandler.id;
}

void YuboxLoRaWANConfigClass::removeTXDuty(YuboxLoRaWAN_txdutychange_cb cb)
{
  if (!cb) return;

  for (auto i = 0; i < cbRXList.size(); i++) {
    YuboxLoRaWAN_rx_List_t entry = cbRXList[i];
    if (entry.event_type == YBX_LW_TXDUTY_CHANGE && entry.txd_cb == cb) cbRXList.erase(cbRXList.begin() + i);
  }
}

void YuboxLoRaWANConfigClass::removeTXDuty(yuboxlorawan_event_id_t id)
{
  for (auto i = 0; i < cbRXList.size(); i++) {
    YuboxLoRaWAN_rx_List_t entry = cbRXList[i];
    if (entry.event_type == YBX_LW_TXDUTY_CHANGE && entry.id == id) cbRXList.erase(cbRXList.begin() + i);
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

void YuboxLoRaWANConfigClass::_txdutychange_handler(void)
{
    for (auto i = 0; i < cbRXList.size(); i++) {
        YuboxLoRaWAN_rx_List_t entry = cbRXList[i];
        if (entry.event_type == YBX_LW_TXDUTY_CHANGE && (entry.txd_cb || entry.txd_fcb)) {
            if (entry.txd_cb) {
                entry.txd_cb();
            } else if (entry.txd_fcb) {
                entry.txd_fcb();
            }
        }
    }
}

static void lorawan_confirm_class_handler(DeviceClass_t Class)
{
    log_i("switch to class %c done", "ABC"[Class]);

    // Informs the server that switch has occurred ASAP
    lmh_app_data_t m_lora_app_data = {NULL, 0, 0, 0, 0};
    m_lora_app_data.buffsize = 0;
    m_lora_app_data.port = LORAWAN_APP_PORT;
    lmh_send(&m_lora_app_data, LMH_UNCONFIRMED_MSG);
}

static void lorawan_has_joined_handler(void)
{
#if (OVER_THE_AIR_ACTIVATION != 0)
    log_i("Network Joined");
#else
    log_i("OVER_THE_AIR_ACTIVATION != 0");
#endif
  lmh_class_request(CLASS_A);
  YuboxLoRaWANConf._join_handler();
}

static void lorawan_join_failed_handler(void)
{
    log_w("OVER_THE_AIR_ACTIVATION failed! Retrying...");
    YuboxLoRaWANConf._joinfail_handler();
    lmh_join();
    YuboxLoRaWANConf._joinstart_handler();
}

static void lorawan_rx_handler(lmh_app_data_t *app_data)
{
    switch (app_data->port) {
    case 3: // Port 3 switches the class
        if (app_data->buffsize == 1) {
            switch (app_data->buffer[0]) {
            case 0: lmh_class_request(CLASS_A); break;
            case 1: lmh_class_request(CLASS_B); break;
            case 2: lmh_class_request(CLASS_C); break;
            default: break;
            }
        }
        break;
    case LORAWAN_APP_PORT:
        YuboxLoRaWANConf._rx_handler(app_data->buffer, app_data->buffsize);
        break;
    default:
        break;
    }
}

YuboxLoRaWANConfigClass YuboxLoRaWANConf;