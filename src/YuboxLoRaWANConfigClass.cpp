#include <Arduino.h>

#include "YuboxWebAuthClass.h"

#include <SPI.h>
#include <RadioLib.h>
#include "YuboxLoRaWANConfigClass.h"
#include <YuboxParamPOST.h>

#include <functional>

#include <Preferences.h>

#include <esp_attr.h>
#ifndef YUBOX_LORAWAN_NO_RTCMEM
#include <esp_rom_crc.h>
#endif

#define ARDUINOJSON_USE_LONG_LONG 1

#include "AsyncJson.h"
#include "ArduinoJson.h"

#if RADIOLIB_EXCLUDE_LORAWAN
#error Esta biblioteca requiere soporte LoRaWAN de RadioLib (RADIOLIB_EXCLUDE_LORAWAN debe ser 0)
#endif

#define JOINREQ_NBTRIALS 3         /**< Number of trials for the join request. */

#define LORAWAN_APP_DEFAULT_TX_DUTYCYCLE 10     /* Default number of seconds for duty cycle */

// Pausa luego de cada grupo de JOINREQ_NBTRIALS intentos fallidos de join
#define YBX_LW_JOIN_RETRY_DELAY_MS          2000

// Pausa luego de un intento de join que falló por error de radio o de configuración
#define YBX_LW_JOIN_ERROR_DELAY_MS          5000

// Intervalo de sondeo de downlinks en clase C
#define YBX_LW_CLASSC_POLL_MS               10

// Para reducir desgaste de flash, la sesión completa (~300 bytes) se guarda en NVRAM
// luego del join, y después sólo si tiene cambios pendientes y ocurre lo primero de:
// - este número de uplinks enviados desde el último guardado completo, o
// - este número de milisegundos transcurridos desde el uplink más reciente.
// Los downlinks y cambios de clase sólo marcan la sesión como pendiente de guardar.
// En cada uplink se guarda además el contador de uplink por separado (4 bytes).
#define YBX_LW_SESSION_FULLSAVE_UPLINKS     30
#define YBX_LW_SESSION_FULLSAVE_IDLE_MS     (300 * 1000)

// Al restaurar una sesión, el contador de uplink se adelanta en este valor sobre el
// máximo valor guardado, por si se transmitió una trama que no alcanzó a guardarse.
#define YBX_LW_FCNTUP_RESTORE_MARGIN        2

// Si no hay ninguna transmisión exitosa en este intervalo luego de un primer
// error de transmisión, se descarta la sesión y se vuelve a hacer join.
#define YBX_LW_TX_ERROR_REJOIN_MS           (90 * 1000)

// Parámetros de la tarea de radio. La prioridad es mayor que la del loop de Arduino
// para respetar la temporización de las ventanas de recepción LoRaWAN.
#define YBX_LW_TASK_NAME                    "YBX_LoRaWAN"
#define YBX_LW_TASK_STACK                   8192
#define YBX_LW_TASK_PRIO                    2

// Claves privadas de NVRAM para persistencia de sesión RadioLib
#define YBX_LW_NVKEY_NONCES                 "lwnonces"
#define YBX_LW_NVKEY_SESSION                "lwsession"
#define YBX_LW_NVKEY_FCNTUP                 "lwfcntup"

// Estado de unión a la red, mismos nombres que reportaba SX126x-Arduino
enum {
  YBX_LW_JOIN_RESET = 0,
  YBX_LW_JOIN_ONGOING,
  YBX_LW_JOIN_SET,
  YBX_LW_JOIN_FAILED,
};

typedef enum {
  YBX_LW_NETJOIN = 0,
  YBX_LW_RX = 1,
  YBX_LW_TXDUTY_CHANGE = 2,
  YBX_LW_TX_CONFIRM = 3,

  YBX_LW_EVENT_MAX = 255
} yuboxlorawan_event_t;

typedef struct YuboxLoRaWAN_rx_List
{
  static yuboxlorawan_event_id_t current_id;
  yuboxlorawan_event_t event_type;
  yuboxlorawan_event_id_t id;

  YuboxLoRaWAN_join_func_cb j_fcb;
  YuboxLoRaWAN_rx_func_cb rx_fcb;
  YuboxLoRaWAN_txdutychange_func_cb txd_fcb;
  YuboxLoRaWAN_txconfirm_func_cb txc_fcb;

  YuboxLoRaWAN_rx_List() : event_type(YBX_LW_EVENT_MAX), id(current_id++), j_fcb(NULL), rx_fcb(NULL), txd_fcb(NULL), txc_fcb(NULL) {}
} YuboxLoRaWAN_rx_List_t;
yuboxlorawan_event_id_t YuboxLoRaWAN_rx_List::current_id = 1;

static std::vector<YuboxLoRaWAN_rx_List_t> cbRXList;

const char * YuboxLoRaWANConfigClass::_ns_nvram_yuboxframework_lorawan = "YUBOX/LoRaWAN";

/*
 * Subclase de LoRaWANNode para acceder al miembro protegido fCntUp de RadioLib,
 * y así adelantar el contador de uplink al restaurar una sesión cuyo contador se
 * guardó por separado (ver YBX_LW_SESSION_FULLSAVE_UPLINKS).
 */
class YuboxLoRaWANNode final : public LoRaWANNode
{
public:
  YuboxLoRaWANNode(PhysicalLayer * phy, const LoRaWANBand_t * band, uint8_t subBand)
    : LoRaWANNode(phy, band, subBand) {}

  // Siguiente valor de contador de uplink que se usará
  uint32_t getNextFCntUp(void) { return this->fCntUp; }
  void advanceFCntUp(uint32_t minNext) { if (this->fCntUp < minNext) this->fCntUp = minNext; }
};

/*
 * Tabla de regiones. El índice es el valor numérico que se usaba con SX126x-Arduino
 * (enum LoRaMacRegion_t), que es el valor guardado en la clave NVRAM "region" y el
 * que se reporta y recibe en el API web. Las regiones sin banda RadioLib (band NULL)
 * no están soportadas por RadioLib.
 */
typedef struct {
  const char * name;
  const LoRaWANBand_t * band;
  // Si la región tenía sub-bandas reales (bandas de canales fijos)
  bool hasSubbands;
} YuboxLoRaWANRegionInfo_t;

#define YBX_LW_REGION_AS923   0
#define YBX_LW_REGION_AU915   1

static const YuboxLoRaWANRegionInfo_t _ybx_lw_regions[] = {
  /*  0 */ { "Asia 923 MHz",              &AS923,   false },
  /*  1 */ { "Australia 915 MHz",         &AU915,   true  },
  /*  2 */ { "China 470 MHz",             &CN470,   true  },
  /*  3 */ { "China 779 MHz",             NULL,     false },
  /*  4 */ { "Europe 433 MHz",            &EU433,   false },
  /*  5 */ { "Europe 868 MHz",            &EU868,   false },
  /*  6 */ { "Korea 920 MHz",             &KR920,   false },
  /*  7 */ { "India 865 MHz",             &IN865,   false },
  /*  8 */ { "US 915 MHz",                &US915,   true  },
  /*  9 */ { "Asia 923 MHz variante 2",   &AS923_2, false },
  /* 10 */ { "Asia 923 MHz variante 3",   &AS923_3, false },
  /* 11 */ { "Asia 923 MHz variante 4",   &AS923_4, false },
  /* 12 */ { "Russia 864 MHz",            NULL,     false },
};
#define YBX_LW_NUM_REGIONS (sizeof(_ybx_lw_regions) / sizeof(_ybx_lw_regions[0]))

// Sub-banda a asumir si la región guardada no está soportada por RadioLib
#define YBX_LW_FALLBACK_SUBBAND 2

// Estado privado de la tarea de radio
struct YuboxLoRaWANConfigClass::_WorkerCtx
{
  enum { WK_IDLE, WK_JOINING, WK_JOINED } st;

  // Generación y épocas de NVRAM vigentes para el trabajo en curso
  uint32_t gen;
  uint32_t sessionEpoch;
  uint32_t credEpoch;
  _JoinParams params;

  YuboxLoRaWANNode * node;

  uint32_t nextJoinTs;
  uint32_t joinFails;

  // Contador de uplink al momento del último guardado completo de sesión
  uint32_t fcntLastFullSave;
  // Último contador de uplink guardado en NVRAM (completo o por separado)
  uint32_t fcntLastSaved;

  // VERDADERO si la sesión en memoria tiene cambios no guardados en NVRAM
  bool sessionDirty;
  // Momento del uplink más reciente
  uint32_t lastSendTs;

  // Copia de los últimos nonces guardados, para no reescribir si no cambian
  uint8_t lastNonces[RADIOLIB_LORAWAN_NONCES_BUF_SIZE];

  // Uplink vacío pendiente para informar cambio de clase a la red
  bool classAckPending;

  // Uplink vacío pendiente para negociar payload mayor luego de payload muy largo
  bool emptyTxPending;
  uint8_t emptyTxPort;

  uint8_t txBuf[255];
  uint8_t downBuf[RADIOLIB_LORAWAN_MAX_PAYLOAD_SIZE + 1];
};

// Candado recursivo con liberación automática al salir del ámbito
class YuboxLoRaWANLockGuard
{
private:
  SemaphoreHandle_t _m;
public:
  YuboxLoRaWANLockGuard(SemaphoreHandle_t m) : _m(m) { if (_m != NULL) xSemaphoreTakeRecursive(_m, portMAX_DELAY); }
  ~YuboxLoRaWANLockGuard() { if (_m != NULL) xSemaphoreGiveRecursive(_m); }
};

/*
 * Copia de la sesión LoRaWAN en memoria RTC, complementaria a la de NVRAM.
 *
 * La memoria RTC se conserva durante deep-sleep y en todo reinicio que no corte
 * la energía (esp_restart, panic, watchdog, brownout). Como es RAM, se actualiza
 * en cada operación que muta la sesión o los contadores sin gastar flash, y al
 * arrancar la sesión se retoma de inmediato desde esta copia. Si la copia no es
 * válida (encendido, reset por pin EN, firmware distinto tras OTA) se recurre a
 * lo guardado en NVRAM, igual que antes.
 *
 * Se usa RTC_NOINIT_ATTR y no RTC_DATA_ATTR: el bootloader de ESP-IDF recarga la
 * sección .rtc.data desde la imagen en todo reinicio que no sea un despertar de
 * deep-sleep, mientras que .rtc_noinit nunca se toca. Al no inicializarse nunca,
 * el bloque debe validarse con magic, versión de layout y CRC32 antes de usarse.
 * Las sumas de 16 bits de RadioLib no bastan para detectar una copia a medias.
 *
 * Definir globalmente YUBOX_LORAWAN_NO_RTCMEM (por build flags, igual que la
 * selección de chipset) desactiva esta copia, para proyectos que requieran la
 * memoria RTC para otros fines. El bloque ocupa unos 340 bytes.
 */
#ifndef YUBOX_LORAWAN_NO_RTCMEM
#define YBX_LW_RTC_MAGIC                    0x4C584259  // "YBXL"
#define YBX_LW_RTC_LAYOUT                   1

struct YuboxLoRaWANRTCSession
{
  uint32_t magic;
  uint16_t layout;
  uint16_t noncesSize;
  uint16_t sessionSize;
  uint16_t reserved;

  // Mínimo valor de FCntUp a usar al restaurar. Se adelanta antes de cada
  // transmisión, por si el reinicio ocurre entre la transmisión y la copia.
  uint32_t fcntReserved;

  // Contador de uplink al último guardado completo en NVRAM, para que la política
  // de guardado diferido en flash continúe a través del reinicio.
  uint32_t fcntLastFullSave;

  uint8_t nonces[RADIOLIB_LORAWAN_NONCES_BUF_SIZE];
  uint8_t session[RADIOLIB_LORAWAN_SESSION_BUF_SIZE];

  // CRC32 de todos los campos anteriores
  uint32_t crc;
};
static RTC_NOINIT_ATTR YuboxLoRaWANRTCSession _ybx_lw_rtc;

static uint32_t _ybx_lw_rtcCRC(const YuboxLoRaWANRTCSession & r)
{
    return esp_rom_crc32_le(0, (const uint8_t *)&r, offsetof(YuboxLoRaWANRTCSession, crc));
}

static bool _ybx_lw_rtcValid(const YuboxLoRaWANRTCSession & r)
{
    return (r.magic == YBX_LW_RTC_MAGIC
        && r.layout == YBX_LW_RTC_LAYOUT
        && r.noncesSize == RADIOLIB_LORAWAN_NONCES_BUF_SIZE
        && r.sessionSize == RADIOLIB_LORAWAN_SESSION_BUF_SIZE
        && r.crc == _ybx_lw_rtcCRC(r));
}
#endif

static void _nvramRemoveIfExists(Preferences & nvram, const char * key)
{
    if (nvram.isKey(key)) nvram.remove(key);
}

// Conversión de EUI guardado (byte más significativo primero) a entero para RadioLib
static uint64_t _eui2u64(const uint8_t * eui)
{
    uint64_t v = 0;
    for (auto i = 0; i < 8; i++) v = (v << 8) | eui[i];
    return v;
}

YuboxLoRaWANConfigClass::YuboxLoRaWANConfigClass(void)
{
    _lw_region = YBX_LW_REGION_AU915;
    _lw_subband = 1;
    _lw_regionNeedsSave = false;
    memset(_lw_devEUI, 0, sizeof(_lw_devEUI));
    memset(_lw_appEUI, 0, sizeof(_lw_appEUI));
    memset(_lw_appKey, 0, sizeof(_lw_appKey));
    memset(_lw_default_devEUI, 0, sizeof(_lw_default_devEUI));
    _lw_confExists = false;
    _lw_deleteConf = false;
    _lw_needsInit = true;
    _lorahw_init = false;
    _tx_waiting_confirm = false;
    _ts_confirmTX_start = 0;
    _num_confirmTX_OK = 0;
    _num_confirmTX_FAIL = 0;

    _tx_conf_num_retries = 3;
    _tx_conf_display = false;
    _last_rx_dataport = 0;
    _enable_all_ports = false;

    _mtx = xSemaphoreCreateRecursiveMutex();
    _phy = NULL;
    _wk_task = NULL;
    _wk_exit = false;
    _wk_gen = 0;
    memset(&_wk_joinParams, 0, sizeof(_wk_joinParams));
    _sessionEpoch = 0;
    _credEpoch = 0;
    _joinStatus = YBX_LW_JOIN_RESET;
    _wk_txPending = false;
    _wk_txExecuting = false;
    _wk_txConfirmed = false;
    _wk_txInternal = false;
    _wk_txPort = LORAWAN_APP_PORT;
    _wk_txLen = 0;
    _wk_maxPayloadLen = 0;
    _ev_activity = false;
    _ev_joined = false;
    _ev_joinedNewSession = false;
    _ev_txDone = false;
    _ev_txOk = false;
    _ev_txConfirmed = false;
    _ev_txAck = false;

    // Estas claves se asumen pendientes de negociar
    _clearSessionKeys();

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
    _ts_lastDownlinkActivity = 0;
    _tx_default_duty_sec = LORAWAN_APP_DEFAULT_TX_DUTYCYCLE;
    _tx_duty_sec = _tx_default_duty_sec;
    _tx_duty_sec_changed = false;
}

void YuboxLoRaWANConfigClass::_lock(void)
{
    if (_mtx != NULL) xSemaphoreTakeRecursive(_mtx, portMAX_DELAY);
}

void YuboxLoRaWANConfigClass::_unlock(void)
{
    if (_mtx != NULL) xSemaphoreGiveRecursive(_mtx);
}

void YuboxLoRaWANConfigClass::_clearSessionKeys()
{
    _lw_useOTAA = true;
}

void YuboxLoRaWANConfigClass::_destroySessionKeys(Preferences & nvram)
{
    YuboxLoRaWANLockGuard g(_mtx);

    _clearSessionKeys();

    // Invalidar cualquier escritura de sesión en curso por parte de la tarea de radio
    _sessionEpoch++;
    _clearRTCSession();

    // Los nonces se conservan a propósito: el DevNonce debe seguir creciendo
    // entre negociaciones OTAA con las mismas credenciales.
    _nvramRemoveIfExists(nvram, YBX_LW_NVKEY_SESSION);
    _nvramRemoveIfExists(nvram, YBX_LW_NVKEY_FCNTUP);
}

void YuboxLoRaWANConfigClass::destroySessionKeys(void)
{
    YuboxLoRaWANLockGuard g(_mtx);
    Preferences nvram;
    nvram.begin(_ns_nvram_yuboxframework_lorawan, false);

    _destroySessionKeys(nvram);
    _lw_needsInit = true;
}

void YuboxLoRaWANConfigClass::destroySavedCredentials(void)
{
    YuboxLoRaWANLockGuard g(_mtx);
    Preferences nvram;
    nvram.begin(_ns_nvram_yuboxframework_lorawan, false);

    _destroySessionKeys(nvram);
    _lw_needsInit = true;

    // Invalidar cualquier escritura de nonces en curso por parte de la tarea de radio
    _credEpoch++;

    nvram.remove("devEUI");
    nvram.remove("appEUI");
    nvram.remove("appKey");
    _nvramRemoveIfExists(nvram, YBX_LW_NVKEY_NONCES);
    memset(_lw_devEUI, 0, sizeof(_lw_devEUI));
    memset(_lw_appEUI, 0, sizeof(_lw_appEUI));
    memset(_lw_appKey, 0, sizeof(_lw_appKey));
    _lw_confExists = false;

    // Detener la tarea de radio y descartar cualquier evento pendiente
    _wk_gen++;
    _wk_txPending = false;
    _joinStatus = YBX_LW_JOIN_RESET;
    _ev_activity = false;
    _ev_joined = false;
    _ev_txDone = false;
    _ev_rxQueue.clear();
    _stopWorker();
}

void YuboxLoRaWANConfigClass::_removeLegacySessionKeys(void)
{
    // Claves de sesión usadas por la implementación anterior basada en SX126x-Arduino.
    // Su formato no es compatible con RadioLib, así que se descartan.
    static const char * legacyKeys[] = { "NwkSKey", "AppSKey", "devaddr", "uplinkcnt", "downlinkcnt" };
    bool found = false;
    Preferences nvram;

    if (nvram.begin(_ns_nvram_yuboxframework_lorawan, true)) {
        for (auto i = 0; i < sizeof(legacyKeys) / sizeof(legacyKeys[0]); i++) {
            if (nvram.isKey(legacyKeys[i])) found = true;
        }
        nvram.end();
    }
    if (!found) return;

    log_w("Se descartan claves de sesión LoRaWAN de implementación anterior, se requerirá negociar OTAA");
    nvram.begin(_ns_nvram_yuboxframework_lorawan, false);
    for (auto i = 0; i < sizeof(legacyKeys) / sizeof(legacyKeys[0]); i++) {
        _nvramRemoveIfExists(nvram, legacyKeys[i]);
    }
    nvram.end();
}

/* ======================================================================== */
/* Selección de chipset de radio en tiempo de compilación                    */
/*                                                                          */
/* - YUBOX_LORAWAN_CHIPSET_SX1262: chipset SX1262. Es el chipset por omisión */
/*   si no se define ninguna de las dos macros.                              */
/* - YUBOX_LORAWAN_CHIPSET_LR1121: chipset LR1121 (familia LR11x0).          */
/*                                                                          */
/* Las macros deben definirse para toda la compilación, por ejemplo con      */
/* ARDUINO_BUILD_EXTRA_FLAGS=-DYUBOX_LORAWAN_CHIPSET_LR1121 en el Makefile   */
/* del proyecto, o en el archivo build_opt.h del proyecto.                   */
/* ======================================================================== */

#if defined(YUBOX_LORAWAN_CHIPSET_SX1262) && defined(YUBOX_LORAWAN_CHIPSET_LR1121)
#error Sólo se puede elegir uno de YUBOX_LORAWAN_CHIPSET_SX1262 o YUBOX_LORAWAN_CHIPSET_LR1121
#endif

// Los #warning siguientes permiten verificar en la salida de compilación cuál
// chipset quedó realmente seleccionado.
#if defined(YUBOX_LORAWAN_CHIPSET_LR1121)
#warning YUBOX LoRaWAN: chipset seleccionado LR1121 (YUBOX_LORAWAN_CHIPSET_LR1121)
#elif defined(YUBOX_LORAWAN_CHIPSET_SX1262)
#warning YUBOX LoRaWAN: chipset seleccionado SX1262 (YUBOX_LORAWAN_CHIPSET_SX1262)
#else
#warning YUBOX LoRaWAN: chipset seleccionado SX1262 (por omision, sin macro de chipset)
#define YUBOX_LORAWAN_CHIPSET_SX1262
#endif

/* ======================================================================== */
/* [PLACA] Pines de conexión entre ESP32 y el chip de radio                  */
/* ======================================================================== */

#if defined(YUBOX_LORAWAN_CHIPSET_LR1121)

// ESP32 - LR1121 pin configuration. Por ahora sólo se soporta la placa
// Yubox Industrial Mini (ESP32-S3 con módulo Wio-LR1121).
#ifdef YUBOX_HELTEC_WIFI_LORA_V3
#error La placa Heltec Wifi LoRa v3 usa el chipset SX1262, no LR1121!
#endif

#if CONFIG_IDF_TARGET_ESP32S3
const int PIN_LORA_NSS      = SS;   // LORA SPI CS
const int PIN_LORA_SCLK     = SCK;  // LORA SPI CLK
const int PIN_LORA_MISO     = MISO; // LORA SPI MISO
const int PIN_LORA_MOSI     = MOSI; // LORA SPI MOSI
const int PIN_LORA_RESET    = 17;   // LORA RESET
const int PIN_LORA_BUSY     = 18;   // LORA SPI BUSY
const int PIN_LORA_IRQ      =  8;   // LORA DIO9
// El conmutador de antena no usa pines del ESP32, sino las salidas DIO5 y DIO6
// del propio LR1121. Ver tabla de conmutador en la sección del chipset.
#else
#error Undefined LORA pins for target! (LR1121 sólo está soportado en ESP32-S3)
#endif

#else   // YUBOX_LORAWAN_CHIPSET_SX1262

#ifdef YUBOX_HELTEC_WIFI_LORA_V3

// Pines para controlar el Heltec Wifi LoRa v3 basado en ESP32-S3

const int PIN_LORA_NSS      =  8;   // LORA SPI CS
const int PIN_LORA_SCLK     =  9;   // LORA SPI CLK
const int PIN_LORA_MISO     = 11;   // LORA SPI MISO
const int PIN_LORA_MOSI     = 10;   // LORA SPI MOSI

const int PIN_LORA_RESET    = 12;   // LORA RESET
const int PIN_LORA_BUSY     = 13;   // LORA SPI BUSY
const int PIN_LORA_IRQ      = 14;   // LORA DIO_1
const int RADIO_TXEN        = -1;   // LORA ANTENNA TX ENABLE
const int RADIO_RXEN        = -1;   // LORA ANTENNA RX ENABLE

#else

// ESP32 - SX126x pin configuration
const int PIN_LORA_NSS      = SS;   // LORA SPI CS
const int PIN_LORA_SCLK     = SCK;  // LORA SPI CLK
const int PIN_LORA_MISO     = MISO; // LORA SPI MISO
const int PIN_LORA_MOSI     = MOSI; // LORA SPI MOSI
#if CONFIG_IDF_TARGET_ESP32
const int PIN_LORA_RESET    = 33;   // LORA RESET
const int PIN_LORA_BUSY     = 34;   // LORA SPI BUSY
const int PIN_LORA_IRQ      = 39;   // LORA DIO_1
const int RADIO_TXEN        = 26;   // LORA ANTENNA TX ENABLE
const int RADIO_RXEN        = 27;   // LORA ANTENNA RX ENABLE
#elif CONFIG_IDF_TARGET_ESP32S2
const int PIN_LORA_RESET    = 17;   // LORA RESET
const int PIN_LORA_BUSY     = 18;   // LORA SPI BUSY
const int PIN_LORA_IRQ      =  8;   // LORA DIO_1
const int RADIO_TXEN        = 16;   // LORA ANTENNA TX ENABLE
const int RADIO_RXEN        = 15;   // LORA ANTENNA RX ENABLE

#elif CONFIG_IDF_TARGET_ESP32S3
const int PIN_LORA_RESET    = 17;   // LORA RESET
const int PIN_LORA_BUSY     = 18;   // LORA SPI BUSY
const int PIN_LORA_IRQ      =  8;   // LORA DIO_1
const int RADIO_TXEN        = 16;   // LORA ANTENNA TX ENABLE
const int RADIO_RXEN        = 15;   // LORA ANTENNA RX ENABLE
#else
#error Undefined LORA pins for target!
#endif

#endif

#endif  // YUBOX_LORAWAN_CHIPSET_xxx

// Bus SPI propio para el radio. Se usa el bus por omisión de SPIClass, igual
// que SX126x-Arduino, para no interferir con el objeto global SPI.
static SPIClass * _ybx_lw_spi = NULL;
static Module * _ybx_lw_module = NULL;

// Crear (una sola vez) el bus SPI y el módulo RadioLib, comunes a todos los chipsets
static Module * _ybx_lw_getModule(void)
{
    if (_ybx_lw_spi == NULL) {
        _ybx_lw_spi = new SPIClass();
        _ybx_lw_spi->begin(PIN_LORA_SCLK, PIN_LORA_MISO, PIN_LORA_MOSI, PIN_LORA_NSS);
    }
    if (_ybx_lw_module == NULL) {
        _ybx_lw_module = new Module(PIN_LORA_NSS, PIN_LORA_IRQ, PIN_LORA_RESET, PIN_LORA_BUSY, *_ybx_lw_spi);
    }
    return _ybx_lw_module;
}

/*
 * Las secciones siguientes son las únicas específicas del chip de radio. Cada
 * una crea el objeto de radio, lo configura, lo duerme y lo nombra. El resto
 * del código usa PhysicalLayer y LoRaWANNode.
 */

#if defined(YUBOX_LORAWAN_CHIPSET_SX1262)

/* ======================================================================== */
/* [CHIPSET SX1262] Inicio de la sección específica del chip de radio        */
/* ======================================================================== */

// Configuración idéntica a la usada con SX126x-Arduino: TCXO controlado por DIO3
// a 3.3 V, regulador DC-DC (no LDO), y DIO2 NO usado como control de antena.
#define YBX_SX1262_TCXO_VOLTAGE     3.3
#define YBX_SX1262_USE_LDO          false
#define YBX_SX1262_DIO2_RFSWITCH    false

static SX1262 * _ybx_lw_sx1262 = NULL;

bool YuboxLoRaWANConfigClass::_chipsetInitRadio(void)
{
    if (_phy != NULL) return true;

    if (_ybx_lw_module == NULL) {
        Module * mod = _ybx_lw_getModule();

        // Conmutador de antena controlado por pines TX ENABLE y RX ENABLE. Igual que en
        // SX126x-Arduino, sólo se usa si ambos pines están definidos. En TX y RX los
        // niveles son los mismos que antes. En reposo RadioLib pone ambos en bajo.
        if (RADIO_RXEN != -1 && RADIO_TXEN != -1) {
            mod->setRfSwitchPins(RADIO_RXEN, RADIO_TXEN);
        }
    }
    if (_ybx_lw_sx1262 == NULL) {
        _ybx_lw_sx1262 = new SX1262(_ybx_lw_module);
    }

    // La frecuencia y modulación inicial son irrelevantes, LoRaWAN las reconfigura
    // en cada transmisión y recepción.
    int16_t state = _ybx_lw_sx1262->begin(
        915.0, 125.0, 9, 7,
        RADIOLIB_SX126X_SYNC_WORD_PRIVATE, 10, 8,
        YBX_SX1262_TCXO_VOLTAGE, YBX_SX1262_USE_LDO);
    if (state != RADIOLIB_ERR_NONE) {
        log_e("Fallo al inicializar SX1262 - %d", state);
        return false;
    }

    // RadioLib activa DIO2 como control de antena por omisión
    state = _ybx_lw_sx1262->setDio2AsRfSwitch(YBX_SX1262_DIO2_RFSWITCH);
    if (state != RADIOLIB_ERR_NONE) {
        log_e("Fallo al configurar DIO2 de SX1262 - %d", state);
        return false;
    }

    _phy = _ybx_lw_sx1262;
    return true;
}

void YuboxLoRaWANConfigClass::_chipsetSleepRadio(void)
{
    if (_ybx_lw_sx1262 != NULL) _ybx_lw_sx1262->sleep();
}

const char * YuboxLoRaWANConfigClass::_chipsetName(void)
{
    return "SX1262";
}

/* ======================================================================== */
/* [CHIPSET SX1262] Fin de la sección específica del chip de radio           */
/* ======================================================================== */

#elif defined(YUBOX_LORAWAN_CHIPSET_LR1121)

/* ======================================================================== */
/* [CHIPSET LR1121] Inicio de la sección específica del chip de radio        */
/* ======================================================================== */

// El módulo Wio-LR1121 lleva TCXO propio, alimentado por el LR1121 a 1.6 V
#define YBX_LR1121_TCXO_VOLTAGE     1.6

// Conmutador de RF del Wio-LR1121, gobernado por las salidas DIO5 y DIO6 del
// propio LR1121. Es la tabla del manual de la Yubox Industrial Mini (sección 7.6),
// la misma que usa RadioLib para el Wio-WM1110 de Seeed.
static const uint32_t _ybx_lr1121_rfswitch_pins[Module::RFSWITCH_MAX_PINS] = {
    RADIOLIB_LR11X0_DIO5, RADIOLIB_LR11X0_DIO6, RADIOLIB_NC, RADIOLIB_NC, RADIOLIB_NC
};
static const Module::RfSwitchMode_t _ybx_lr1121_rfswitch_table[] = {
    // modo                    DIO5  DIO6
    { LR11x0::MODE_STBY,     { LOW,  LOW  } },
    { LR11x0::MODE_RX,       { HIGH, LOW  } },
    { LR11x0::MODE_TX,       { HIGH, HIGH } },
    { LR11x0::MODE_TX_HP,    { LOW,  HIGH } },
    { LR11x0::MODE_TX_HF,    { LOW,  LOW  } },
    { LR11x0::MODE_GNSS,     { LOW,  LOW  } },
    { LR11x0::MODE_WIFI,     { LOW,  LOW  } },
    END_OF_MODE_TABLE,
};

static LR1121 * _ybx_lw_lr1121 = NULL;

bool YuboxLoRaWANConfigClass::_chipsetInitRadio(void)
{
    if (_phy != NULL) return true;

    if (_ybx_lw_lr1121 == NULL) {
        _ybx_lw_lr1121 = new LR1121(_ybx_lw_getModule());
    }

    // La frecuencia y modulación inicial son irrelevantes, LoRaWAN las reconfigura
    // en cada transmisión y recepción.
    int16_t state = _ybx_lw_lr1121->begin(
        915.0, 125.0, 9, 7,
        RADIOLIB_LR11X0_LORA_SYNC_WORD_PRIVATE, 10, 8,
        YBX_LR1121_TCXO_VOLTAGE);
    if (state != RADIOLIB_ERR_NONE) {
        log_e("Fallo al inicializar LR1121 - %d", state);
        return false;
    }

    // La tabla del conmutador se envía al chip por SPI, y begin() reinicia el chip.
    // Por lo tanto debe configurarse DESPUÉS de begin(). El chip conserva esta
    // configuración al dormir.
    _ybx_lw_lr1121->setRfSwitchTable(_ybx_lr1121_rfswitch_pins, _ybx_lr1121_rfswitch_table);

    _phy = _ybx_lw_lr1121;
    return true;
}

void YuboxLoRaWANConfigClass::_chipsetSleepRadio(void)
{
    if (_ybx_lw_lr1121 != NULL) _ybx_lw_lr1121->sleep();
}

const char * YuboxLoRaWANConfigClass::_chipsetName(void)
{
    return "LR1121";
}

/* ======================================================================== */
/* [CHIPSET LR1121] Fin de la sección específica del chip de radio           */
/* ======================================================================== */

#endif  // YUBOX_LORAWAN_CHIPSET_xxx

bool YuboxLoRaWANConfigClass::begin(AsyncWebServer & srv, bool displayTxConf)
{
    _tx_conf_display = displayTxConf;

    _loadSavedCredentialsFromNVRAM();
    _removeLegacySessionKeys();
    _setupHTTPRoutes(srv);

#ifdef YUBOX_LORAWAN_NO_RTCMEM
    log_d("Copia de sesión LoRaWAN en memoria RTC desactivada (YUBOX_LORAWAN_NO_RTCMEM)");
#else
    log_d("Copia de sesión LoRaWAN en memoria RTC activa (%u bytes)", sizeof(_ybx_lw_rtc));
#endif

    _lorahw_init = _chipsetInitRadio();
    if (!_lorahw_init) {
        log_e("Fallo al inicializar hardware de radio %s", _chipsetName());
    }

    return _lorahw_init;
}

void YuboxLoRaWANConfigClass::_loadSavedCredentialsFromNVRAM(void)
{
    YuboxLoRaWANLockGuard g(_mtx);
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
    uint8_t n_region = nvram.getUChar("region", YBX_LW_REGION_AU915);
    uint8_t n_subband = nvram.getUChar("subband", 1);
    _tx_duty_sec = nvram.getUInt("txduty", _tx_default_duty_sec);

    _tx_conf_num_retries = nvram.getUInt("txconfretries", 3);

    _lw_regionNeedsSave = false;
    if (n_region < YBX_LW_NUM_REGIONS && _ybx_lw_regions[n_region].band == NULL) {
        // Región conocida pero no implementada en RadioLib
        log_e("Región LoRaWAN guardada %u (%s) no está soportada por RadioLib, se asume %s",
            n_region, _ybx_lw_regions[n_region].name, _ybx_lw_regions[YBX_LW_REGION_AU915].name);
        bool keepSubband = (_ybx_lw_regions[n_region].hasSubbands
            && n_subband >= 1 && n_subband <= _getMaxLoRaWANRegionSubchannel(YBX_LW_REGION_AU915));
        n_region = YBX_LW_REGION_AU915;
        if (!keepSubband) n_subband = YBX_LW_FALLBACK_SUBBAND;
        _lw_regionNeedsSave = true;
    } else if (!_isValidLoRaWANRegion(n_region)) {
        // Validar si región seleccionada es válida...
        n_region = YBX_LW_REGION_AU915;
        _lw_regionNeedsSave = true;
    }
    _lw_region = n_region;
    _lw_subband = n_subband;

    // Validar si sub-banda almacenada es válida para región...
    if (_lw_subband < 1) _lw_subband = 1;
    if (_lw_subband > _getMaxLoRaWANRegionSubchannel(_lw_region)) {
        log_w("Sub-banda guardada %u no es válida para región %s, se asume 1",
            _lw_subband, _getLoRaWANRegionName(_lw_region));
        _lw_subband = 1;
        _lw_regionNeedsSave = true;
    }

    _lw_confExists = ok;

    // Si hay sesión guardada, la siguiente conexión intentará restaurarla en lugar de OTAA
    _clearSessionKeys();
    if (ok && nvram.isKey(YBX_LW_NVKEY_SESSION)) _lw_useOTAA = false;

    nvram.end();
}

bool YuboxLoRaWANConfigClass::_isValidLoRaWANRegion(uint8_t r)
{
    return (r < YBX_LW_NUM_REGIONS && _ybx_lw_regions[r].band != NULL);
}

const char * YuboxLoRaWANConfigClass::_getLoRaWANRegionName(uint8_t region)
{
    if (region < YBX_LW_NUM_REGIONS) return _ybx_lw_regions[region].name;
    return "(región no válida o no descrita)";
}

uint8_t YuboxLoRaWANConfigClass::_getMaxLoRaWANRegionSubchannel(uint8_t region)
{
    if (!_isValidLoRaWANRegion(region)) return 1;

    const LoRaWANBand_t * band = _ybx_lw_regions[region].band;

    // RadioLib sólo usa sub-bandas en bandas de canales fijos, en grupos de 8 canales
    // de 125 kHz. En bandas dinámicas el valor de sub-banda se ignora.
    if (band->bandType != RADIOLIB_LORAWAN_BAND_FIXED) return 1;
    return band->txSpans[0].numChannels / 8;
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
  srv.on("/yubox-api/lorawan/resetconn", HTTP_POST, std::bind(&YuboxLoRaWANConfigClass::_routeHandler_yuboxAPI_lorawanresetconn_POST, this, std::placeholders::_1));
  srv.on("/yubox-api/lorawan/deletecred", HTTP_POST, std::bind(&YuboxLoRaWANConfigClass::_routeHandler_yuboxAPI_lorawandeletecred_POST, this, std::placeholders::_1));
}

uint8_t YuboxLoRaWANConfigClass::_getJoinStatus(void)
{
    YuboxLoRaWANLockGuard g(_mtx);
    return _joinStatus;
}

const char * YuboxLoRaWANConfigClass::_getJoinStatusName(void)
{
    switch (_getJoinStatus()) {
    case YBX_LW_JOIN_SET:       return "SET";
    case YBX_LW_JOIN_ONGOING:   return "ONGOING";
    case YBX_LW_JOIN_FAILED:    return "FAILED";
    case YBX_LW_JOIN_RESET:
    default:                    return "RESET";
    }
}

String YuboxLoRaWANConfigClass::_reportActivityJSON(void)
{
#if ARDUINOJSON_VERSION_MAJOR <= 6
    DynamicJsonDocument json_doc(JSON_OBJECT_SIZE(8));
#else
    JsonDocument json_doc;
#endif
    if (_lorahw_init && _lw_confExists && !_lw_needsInit) {
        json_doc["join"] = _getJoinStatusName();
    } else {
        json_doc["join"] = "RESET";
    }
    if (_ts_ultimoTX_OK != 0) json_doc["tx_ok"] = _ts_ultimoTX_OK; else json_doc["tx_ok"] = (const char *)NULL;
    if (_ts_ultimoTX_FAIL != 0) json_doc["tx_fail"] = _ts_ultimoTX_FAIL; else json_doc["tx_fail"] = (const char *)NULL;
    if (_ts_ultimoRX != 0) json_doc["rx"] = _ts_ultimoRX; else json_doc["rx"] = (const char *)NULL;
    json_doc["ts"] = millis();

    json_doc["num_confirmtx_ok"] = _num_confirmTX_OK;
    json_doc["num_confirmtx_fail"] = _num_confirmTX_FAIL;
    if (_tx_waiting_confirm) json_doc["confirmtx_start"] = _ts_confirmTX_start; else json_doc["confirmtx_start"] = (const char *)NULL;

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
    for (auto i = 0; i < YBX_LW_NUM_REGIONS; i++) if (_isValidLoRaWANRegion(i)) numRegions++;

    AsyncResponseStream *response = request->beginResponseStream("application/json");
#if ARDUINOJSON_VERSION_MAJOR <= 6
    DynamicJsonDocument json_doc(JSON_ARRAY_SIZE(numRegions) + numRegions * JSON_OBJECT_SIZE(3));
#else
    JsonDocument json_doc;
#endif

    // Se conservan los identificadores originales, aunque queden huecos para las
    // regiones no soportadas por RadioLib.
    for (auto i = 0; i < YBX_LW_NUM_REGIONS; i++) {
        if (!_isValidLoRaWANRegion(i)) continue;

        JsonObject region =
#if ARDUINOJSON_VERSION_MAJOR <= 6
            json_doc.createNestedObject();
#else
            json_doc.add<JsonObject>();
#endif
        region["id"] = i;
        region["name"] = _getLoRaWANRegionName(i);
        region["max_sb"] = _getMaxLoRaWANRegionSubchannel(i);
    }

    serializeJson(json_doc, *response);
    request->send(response);
}

void YuboxLoRaWANConfigClass::_routeHandler_yuboxAPI_lorawanconfigjson_GET(AsyncWebServerRequest * request)
{
    YUBOX_RUN_AUTH(request);

    AsyncResponseStream *response = request->beginResponseStream("application/json");
#if ARDUINOJSON_VERSION_MAJOR <= 6
    DynamicJsonDocument json_doc(JSON_OBJECT_SIZE(10));
#else
    JsonDocument json_doc;
#endif

    String s_default_devEUI, s_devEUI, s_appEUI, s_appKey;
    {
        YuboxLoRaWANLockGuard g(_mtx);

        json_doc["region"] = (unsigned int)_lw_region;
        s_default_devEUI = _bin2str(_lw_default_devEUI, sizeof(_lw_default_devEUI));
        s_devEUI = _lw_confExists ? _bin2str(_lw_devEUI, sizeof(_lw_devEUI)) : s_default_devEUI;
        s_appEUI = _lw_confExists ? _bin2str(_lw_appEUI, sizeof(_lw_appEUI)) : "";
        s_appKey = _lw_confExists ? _bin2str(_lw_appKey, sizeof(_lw_appKey)) : "";
        json_doc["subband"] = _lw_subband;
    }
    json_doc["deviceEUI_ESP32"] = s_default_devEUI.c_str();
    json_doc["deviceEUI"] = s_devEUI.c_str();
    json_doc["appEUI"] = s_appEUI.c_str();
    json_doc["appKey"] = s_appKey.c_str();
    json_doc["join"] = _getJoinStatusName();
    json_doc["tx_duty_sec"] = getRequestedTXDutyCycle();

    if (_tx_waiting_confirm)
        json_doc["confirmtx_start"] = _ts_confirmTX_start;
    else json_doc["confirmtx_start"] = (const char *)NULL;

    if (_tx_conf_display)
        json_doc["txconf_retries"] = _tx_conf_num_retries;
    else json_doc["txconf_retries"] = (const char *)NULL;

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
    uint32_t n_tx_conf_num_retries = _tx_conf_num_retries;

    YBX_ASSIGN_NUM_FROM_POST(region, "ID de región", "%hhu", YBX_POST_VAR_NONEMPTY, n_region)
    if (!clientError && !_isValidLoRaWANRegion(n_region)) {
        clientError = true;
        responseMsg = "ID de región no válido o no implementado";
    }

    YBX_ASSIGN_NUM_FROM_POST(subband, "Sub-banda", "%hhu", YBX_POST_VAR_NONEMPTY, n_subband)
    if (!clientError && !(n_subband >= 1 && n_subband <= _getMaxLoRaWANRegionSubchannel(n_region))) {
        clientError = true;
        responseMsg = "Sub-banda no está en rango requerido para región";
    }

    String hexParam;
#define LWPARAM_SCAN(P, R) \
    hexParam.clear();\
    clientError = parseParamPOST(clientError, responseMsg, request, \
        YBX_POST_VAR_TRIM | YBX_POST_VAR_BLANK | ((R) ? (YBX_POST_VAR_REQUIRED|YBX_POST_VAR_NONEMPTY) : 0), \
        #P, NULL, hexParam);\
    if (!clientError) {\
        memset(n_##P, 0, sizeof(n_##P));\
        if (!hexParam.isEmpty()) {\
            if (hexParam.length() != 2 * sizeof(n_##P)) {\
                clientError = true;\
                responseMsg = "EUI de longitud incorrecta (" #P "), se esperaba ";\
                responseMsg += 2 * sizeof(n_##P);\
            } else if (!_str2bin(hexParam.c_str(), n_##P, sizeof(n_##P))) {\
                clientError = true;\
                responseMsg = "EUI contiene caracteres no-hexadecimales: " #P;\
            }\
        }\
    }


    LWPARAM_SCAN(deviceEUI, true)
    LWPARAM_SCAN(appEUI, false)
    LWPARAM_SCAN(appKey, true)

    YBX_ASSIGN_NUM_FROM_POST(tx_duty_sec, "Intervalo de transmisión", "%lu", YBX_POST_VAR_NONEMPTY, n_tx_duty_sec)
    if (!clientError && n_tx_duty_sec < 10) {
        clientError = true;
        responseMsg = "Intervalo de transmisión debe ser de al menos 10 segundos";
    }

    if (_tx_conf_display) {
        YBX_ASSIGN_NUM_FROM_POST(txconf_retries, "Reintentos de transmisión confirmada", "%lu", YBX_POST_VAR_NONEMPTY, n_tx_conf_num_retries)
        if (!clientError && n_tx_conf_num_retries < 1) {
            clientError = true;
            responseMsg = "Número de reintentos de transmisión confirmada debe ser mayor a 0";
        }
    }

    if (!clientError) {
        YuboxLoRaWANLockGuard g(_mtx);

        bool paramIguales = (
            (0 == memcmp(_lw_devEUI, n_deviceEUI, sizeof(_lw_devEUI))) &&
            (0 == memcmp(_lw_appEUI, n_appEUI, sizeof(_lw_appEUI))) &&
            (0 == memcmp(_lw_appKey, n_appKey, sizeof(_lw_appKey))) &&
            n_region == _lw_region && n_subband == _lw_subband);

        memcpy(_lw_devEUI, n_deviceEUI, sizeof(_lw_devEUI));
        memcpy(_lw_appEUI, n_appEUI, sizeof(_lw_appEUI));
        memcpy(_lw_appKey, n_appKey, sizeof(_lw_appKey));
        _lw_region = n_region;
        _lw_subband = n_subband;

        _tx_conf_num_retries = n_tx_conf_num_retries;

        if (!paramIguales) {
            serverError = !_saveCredentialsToNVRAM();
        } else if (_lw_regionNeedsSave) {
            // La región en NVRAM no era utilizable y se asumió otra. Se guarda la
            // asumida sin descartar la sesión, porque los parámetros de red no cambian.
            serverError = !_saveRegionToNVRAM();
        }
        if (!serverError) _lw_regionNeedsSave = false;

        if (serverError) {
            responseMsg = "No se pueden guardar valores LoRaWAN";
        } else if (!setRequestedTXDutyCycle(n_tx_duty_sec)) {
            serverError = true;
            responseMsg = "No se puede guardar intervalo de transmisión deseado";
        } else if (!_saveConfirmedTXRetries()) {
            serverError = true;
            responseMsg = "No se puede guardar reintentos de transmisión confirmada";
        } else {
            if (_lw_confExists && paramIguales) {
                log_d("Parámetros de red no han cambiado, se omite reinicialización");
            } else {
                log_d("Parámetros de red han cambiado, se requiere inicialización");
                _lw_needsInit = true;
            }
            _lw_confExists = true;
        }
    }

    if (!clientError && !serverError) {
        responseMsg = "Parámetros actualizados correctamente";
    }

    YBX_STD_RESPONSE
}

void YuboxLoRaWANConfigClass::_routeHandler_yuboxAPI_lorawanresetconn_POST(AsyncWebServerRequest * request)
{
    YUBOX_RUN_AUTH(request);

    bool clientError = false;
    bool serverError = false;
    String responseMsg = "";

    log_w("Se desechan credenciales negociadas, se volverá a intentar OTAA para negociación LoRaWAN...");
    {
        YuboxLoRaWANLockGuard g(_mtx);

        _ts_errorAfterJoin = 0;
        _lw_needsInit = true;

        // Destruir cualquier clave de sesión, porque debe volverse a negociar OTAA
        if (!_lw_useOTAA) destroySessionKeys();
    }

    responseMsg = "Se desechan credenciales LoRaWAN y se reinicia negociación OTAA";

    YBX_STD_RESPONSE
}

void YuboxLoRaWANConfigClass::_routeHandler_yuboxAPI_lorawandeletecred_POST(AsyncWebServerRequest * request)
{
    YUBOX_RUN_AUTH(request);

    bool clientError = false;
    bool serverError = false;
    String responseMsg = "";

    _lw_deleteConf = true;

    responseMsg = "Se desechan claves y credenciales LoRaWAN y se detiene LoRaWAN";

    YBX_STD_RESPONSE
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
    YuboxLoRaWANLockGuard g(_mtx);
    bool ok = true;
    Preferences nvram;
    nvram.begin(_ns_nvram_yuboxframework_lorawan, false);

    if (ok && !nvram.putUChar("region", (uint8_t)_lw_region)) ok = false;
    if (ok && !nvram.putUChar("subband", _lw_subband)) ok = false;
    if (ok && !nvram.putBytes("devEUI", _lw_devEUI, sizeof(_lw_devEUI))) ok = false;
    if (ok && !nvram.putBytes("appEUI", _lw_appEUI, sizeof(_lw_appEUI))) ok = false;
    if (ok && !nvram.putBytes("appKey", _lw_appKey, sizeof(_lw_appKey))) ok = false;

    // Cuando se guardan nuevas claves, se debe asumir que las claves de sesión se invalidan
    _destroySessionKeys(nvram);

    nvram.end();
    return ok;
}

bool YuboxLoRaWANConfigClass::_saveRegionToNVRAM(void)
{
    YuboxLoRaWANLockGuard g(_mtx);
    bool ok = true;
    Preferences nvram;
    nvram.begin(_ns_nvram_yuboxframework_lorawan, false);

    if (ok && !nvram.putUChar("region", (uint8_t)_lw_region)) ok = false;
    if (ok && !nvram.putUChar("subband", _lw_subband)) ok = false;

    nvram.end();
    return ok;
}

bool YuboxLoRaWANConfigClass::_saveConfirmedTXRetries(void)
{
    bool ok = true;
    Preferences nvram;
    nvram.begin(_ns_nvram_yuboxframework_lorawan, false);

    if (_tx_conf_display) {
        if (ok && !nvram.putUInt("txconfretries", _tx_conf_num_retries)) ok = false;
    }

    nvram.end();
    return ok;
}

void YuboxLoRaWANConfigClass::setDefaultTXDutyCycle(uint32_t n_txduty)
{
    if (n_txduty < LORAWAN_APP_DEFAULT_TX_DUTYCYCLE) n_txduty = LORAWAN_APP_DEFAULT_TX_DUTYCYCLE;
    _tx_default_duty_sec = n_txduty;
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
    if (_lw_deleteConf) {
        _lw_deleteConf = false;

        log_w("Se desechan claves y credenciales negociadas, se detiene LoRaWAN hasta volver a ingresar claves...");
        _ts_errorAfterJoin = 0;
        _lw_needsInit = true;

        // Destruir cualquier clave de sesión, porque debe volverse a negociar OTAA
        destroySavedCredentials();

        _sendActivityEventJSON();
    }

    if (!_lw_confExists) return;

    if (_tx_duty_sec_changed) {
        _tx_duty_sec_changed = false;
        _txdutychange_handler();
    }

    if (!_lorahw_init) return;

    if (_lw_needsInit) {
        _lw_needsInit = false;
        _tx_waiting_confirm = false;
        _ts_confirmTX_start = 0;
        _num_confirmTX_OK = 0;
        _num_confirmTX_FAIL = 0;
        _ts_lastDownlinkActivity = 0;

        bool useOTAA;
        {
            YuboxLoRaWANLockGuard g(_mtx);

            // Nueva generación de conexión: todo trabajo y evento previo se descarta
            _wk_gen++;
            memcpy(_wk_joinParams.devEUI, _lw_devEUI, sizeof(_lw_devEUI));
            memcpy(_wk_joinParams.appEUI, _lw_appEUI, sizeof(_lw_appEUI));
            memcpy(_wk_joinParams.appKey, _lw_appKey, sizeof(_lw_appKey));
            _wk_joinParams.region = _lw_region;
            _wk_joinParams.subband = _lw_subband;
            _wk_txPending = false;
            _wk_maxPayloadLen = 0;
            _joinStatus = YBX_LW_JOIN_ONGOING;
            _ev_activity = false;
            _ev_joined = false;
            _ev_txDone = false;
            _ev_rxQueue.clear();
            useOTAA = _lw_useOTAA;
        }

        log_d("Para esta unión a la red LoRaWAN %s se usará OTAA...", useOTAA ? "SÍ" : "NO");
        (void)useOTAA;
        if (!_startWorker()) {
            log_e("No se puede iniciar tarea de radio LoRaWAN");
            {
                YuboxLoRaWANLockGuard g(_mtx);
                _joinStatus = YBX_LW_JOIN_FAILED;
            }
            _sendActivityEventJSON();
        } else {
            log_i("Starting join LoRaWAN network (region %s subband %d)...",
                _getLoRaWANRegionName(_lw_region), _lw_subband);
            _joinstart_handler();
        }
    }

    _processWorkerEvents();
}

void YuboxLoRaWANConfigClass::_processWorkerEvents(void)
{
    bool activity, joined, joinedNew, txDone, txOk, txConfirmed, txAck;
    std::deque<_RxEvent> rxQueue;

    {
        YuboxLoRaWANLockGuard g(_mtx);

        activity = _ev_activity; _ev_activity = false;
        joined = _ev_joined; joinedNew = _ev_joinedNewSession; _ev_joined = false;
        txDone = _ev_txDone; _ev_txDone = false;
        txOk = _ev_txOk; txConfirmed = _ev_txConfirmed; txAck = _ev_txAck;
        rxQueue.swap(_ev_rxQueue);
    }

    // Los manejadores siguientes envían por sí mismos el evento de actividad
    if (joined) {
        _join_handler(joinedNew);
        activity = false;
    }
    while (!rxQueue.empty()) {
        _RxEvent & ev = rxQueue.front();
        _rx_handler(ev.data.data(), (uint8_t)ev.data.size(), ev.port);
        rxQueue.pop_front();
        activity = false;
    }
    if (txDone) {
        _tx_result_handler(txOk, txConfirmed, txAck);
        activity = false;
    }

    if (activity) _sendActivityEventJSON();
}

void YuboxLoRaWANConfigClass::_sendActivityEventJSON(void)
{
    if (_pEvents != NULL && _pEvents->count() > 0) {
        String json_str = _reportActivityJSON();
        _pEvents->send(json_str.c_str());
    }
}

void YuboxLoRaWANConfigClass::_joinstart_handler(void)
{
    _sendActivityEventJSON();
}

bool YuboxLoRaWANConfigClass::isJoined(void)
{
    return (_lorahw_init && _lw_confExists && !_lw_needsInit && (YBX_LW_JOIN_SET == _getJoinStatus()));
}

bool YuboxLoRaWANConfigClass::_enqueueTX(uint8_t port, uint8_t * p, uint8_t n, bool is_txconfirmed, bool internal)
{
    YuboxLoRaWANLockGuard g(_mtx);

    if (_wk_task == NULL || _wk_exit) return false;
    if (_joinStatus != YBX_LW_JOIN_SET) return false;
    if (port < RADIOLIB_LORAWAN_FPORT_PAYLOAD_MIN || port > RADIOLIB_LORAWAN_FPORT_PAYLOAD_MAX) {
        log_w("Puerto LoRaWAN %u no válido para datos de aplicación", port);
        return false;
    }
    if (_wk_txPending || _wk_txExecuting) {
        log_d("Hay una transmisión LoRaWAN en curso, no se puede enviar todavía");
        return false;
    }
    if (n > _wk_maxPayloadLen) {
        log_d("Payload de %u bytes excede máximo actual de %u bytes", n, _wk_maxPayloadLen);
        return false;
    }

    _wk_txPort = port;
    _wk_txLen = (p != NULL) ? n : 0;
    if (_wk_txLen > 0) memcpy(_wk_txBuf, p, _wk_txLen);
    _wk_txConfirmed = is_txconfirmed;
    _wk_txInternal = internal;
    _wk_txPending = true;

    // Se notifica con el candado tomado, así la tarea no puede terminar en medio
    xTaskNotifyGive(_wk_task);
    return true;
}

bool YuboxLoRaWANConfigClass::_registerTXFailure(void)
{
    uint32_t t = millis();
    _ts_ultimoTX_FAIL = t;
    if (_ts_errorAfterJoin == 0) _ts_errorAfterJoin = t;

    if (t - _ts_errorAfterJoin >= YBX_LW_TX_ERROR_REJOIN_MS) {
        log_w("No hay transmisión exitosa luego de timeout, se reintenta join...");
        _ts_errorAfterJoin = 0;
        _lw_needsInit = true;

        // Destruir cualquier clave de sesión, porque debe volverse a negociar OTAA
        if (!_lw_useOTAA) destroySessionKeys();
        return true;
    }
    return false;
}

bool YuboxLoRaWANConfigClass::send(uint8_t app_port, uint8_t * p, uint8_t n, bool is_txconfirmed)
{
    if (!_lorahw_init) return false;
    if (!_lw_confExists || _lw_needsInit) return false;

    if (_getJoinStatus() != YBX_LW_JOIN_SET) return false;

    if (p == NULL) n = 0;

    // La transmisión real ocurre en la tarea de radio. El resultado se reporta luego
    // a través de update(), igual que antes llegaba por callbacks de la biblioteca.
    bool ok = _enqueueTX(app_port, p, n, is_txconfirmed, false);

    if (!ok) {
        bool rejoin = _registerTXFailure();

        if (!rejoin && p != NULL) {
            /**
             * NOTA: por Alex Villacís Lasso 2020/11/24
             * En caso de que falle el envío, probablemente es porque no se ha negociado
             * todavía una longitud máxima de payload, lo cual depende de la calidad de radio.
             * El mecanismo de negociación consiste en intentar enviar un paquete LoRaWAN de
             * longitud cero, lo cual permite negociar un payload mayor según se reciba o no
             * en el gateway.
             */
            _enqueueTX(app_port, NULL, 0, false, true);
        }
    } else {
        if (is_txconfirmed) {
            _tx_waiting_confirm = true;
            _ts_confirmTX_start = millis();
        }
    }

    _sendActivityEventJSON();

    return ok;
}

yuboxlorawan_event_id_t YuboxLoRaWANConfigClass::onJoin(YuboxLoRaWAN_join_func_cb cbJ)
{
  if (!cbJ) return 0;

  YuboxLoRaWANLockGuard g(_mtx);
  YuboxLoRaWAN_rx_List_t n_evHandler;
  n_evHandler.event_type = YBX_LW_NETJOIN;
  n_evHandler.j_fcb = cbJ;
  cbRXList.push_back(n_evHandler);
  return n_evHandler.id;
}

void YuboxLoRaWANConfigClass::removeJoin(yuboxlorawan_event_id_t id)
{
  YuboxLoRaWANLockGuard g(_mtx);
  for (auto i = 0; i < cbRXList.size(); i++) {
    YuboxLoRaWAN_rx_List_t entry = cbRXList[i];
    if (entry.event_type == YBX_LW_NETJOIN && entry.id == id) cbRXList.erase(cbRXList.begin() + i);
  }
}

yuboxlorawan_event_id_t YuboxLoRaWANConfigClass::onRX(YuboxLoRaWAN_rx_func_cb cbRX)
{
  if (!cbRX) return 0;

  YuboxLoRaWANLockGuard g(_mtx);
  YuboxLoRaWAN_rx_List_t n_evHandler;
  n_evHandler.event_type = YBX_LW_RX;
  n_evHandler.rx_fcb = cbRX;
  cbRXList.push_back(n_evHandler);
  return n_evHandler.id;
}

void YuboxLoRaWANConfigClass::removeRX(yuboxlorawan_event_id_t id)
{
  YuboxLoRaWANLockGuard g(_mtx);
  for (auto i = 0; i < cbRXList.size(); i++) {
    YuboxLoRaWAN_rx_List_t entry = cbRXList[i];
    if (entry.event_type == YBX_LW_RX && entry.id == id) cbRXList.erase(cbRXList.begin() + i);
  }
}

yuboxlorawan_event_id_t YuboxLoRaWANConfigClass::onTXDuty(YuboxLoRaWAN_txdutychange_func_cb cb)
{
  if (!cb) return 0;

  YuboxLoRaWANLockGuard g(_mtx);
  YuboxLoRaWAN_rx_List_t n_evHandler;
  n_evHandler.event_type = YBX_LW_TXDUTY_CHANGE;
  n_evHandler.txd_fcb = cb;
  cbRXList.push_back(n_evHandler);
  return n_evHandler.id;
}

void YuboxLoRaWANConfigClass::removeTXDuty(yuboxlorawan_event_id_t id)
{
  YuboxLoRaWANLockGuard g(_mtx);
  for (auto i = 0; i < cbRXList.size(); i++) {
    YuboxLoRaWAN_rx_List_t entry = cbRXList[i];
    if (entry.event_type == YBX_LW_TXDUTY_CHANGE && entry.id == id) cbRXList.erase(cbRXList.begin() + i);
  }
}

yuboxlorawan_event_id_t YuboxLoRaWANConfigClass::onTXConfirm(YuboxLoRaWAN_txconfirm_func_cb cb)
{
  if (!cb) return 0;

  YuboxLoRaWANLockGuard g(_mtx);
  YuboxLoRaWAN_rx_List_t n_evHandler;
  n_evHandler.event_type = YBX_LW_TX_CONFIRM;
  n_evHandler.txc_fcb = cb;
  cbRXList.push_back(n_evHandler);
  return n_evHandler.id;
}

void YuboxLoRaWANConfigClass::removeTXConfirm(yuboxlorawan_event_id_t id)
{
  YuboxLoRaWANLockGuard g(_mtx);
  for (auto i = 0; i < cbRXList.size(); i++) {
    YuboxLoRaWAN_rx_List_t entry = cbRXList[i];
    if (entry.event_type == YBX_LW_TX_CONFIRM && entry.id == id) cbRXList.erase(cbRXList.begin() + i);
  }
}

// Copia de la lista de callbacks, para invocarlos sin mantener el candado tomado
static std::vector<YuboxLoRaWAN_rx_List_t> _copyCallbacks(SemaphoreHandle_t mtx, yuboxlorawan_event_t evtype)
{
    std::vector<YuboxLoRaWAN_rx_List_t> l;
    YuboxLoRaWANLockGuard g(mtx);
    for (auto i = 0; i < cbRXList.size(); i++) {
        if (cbRXList[i].event_type == evtype) l.push_back(cbRXList[i]);
    }
    return l;
}

void YuboxLoRaWANConfigClass::_join_handler(bool newSession)
{
    _lw_useOTAA = false;
    if (newSession) {
        // Luego de negociar OTAA ya hay actividad downlink (el Join-Accept)
        _ts_lastDownlinkActivity = millis();
    }

    _sendActivityEventJSON();

    auto l = _copyCallbacks(_mtx, YBX_LW_NETJOIN);
    for (auto i = 0; i < l.size(); i++) {
        l[i].j_fcb();
    }
}

void YuboxLoRaWANConfigClass::_rx_handler(uint8_t * p, uint8_t n, uint8_t app_port)
{
    _last_rx_dataport = app_port;
    _ts_ultimoRX = millis();
    _ts_lastDownlinkActivity = _ts_ultimoRX;
    if (_enable_all_ports || app_port == LORAWAN_APP_PORT) {
        auto l = _copyCallbacks(_mtx, YBX_LW_RX);
        for (auto i = 0; i < l.size(); i++) {
            l[i].rx_fcb(p, n);
        }
    }

    _sendActivityEventJSON();
}

void YuboxLoRaWANConfigClass::_txdutychange_handler(void)
{
    auto l = _copyCallbacks(_mtx, YBX_LW_TXDUTY_CHANGE);
    for (auto i = 0; i < l.size(); i++) {
        l[i].txd_fcb();
    }
}

void YuboxLoRaWANConfigClass::_tx_result_handler(bool txOk, bool isConfirmed, bool ackOk)
{
    if (txOk) {
        _ts_errorAfterJoin = 0;
        _ts_ultimoTX_OK = millis();
    } else {
        _registerTXFailure();
    }

    if (isConfirmed) {
        // Se reporta también el fallo de transmisión como fallo de confirmación
        _tx_confirmed_result(ackOk);
    } else {
        _sendActivityEventJSON();
    }
}

void YuboxLoRaWANConfigClass::_tx_confirmed_result(bool r)
{
    log_v("RESULTADO DE CONFIRMED TX ES %s", r ? "OK": "FAIL");

    _tx_waiting_confirm = false;
    _ts_confirmTX_start = 0;
    if (r) {
        _ts_lastDownlinkActivity = millis();
        _num_confirmTX_OK++;
    } else {
        _num_confirmTX_FAIL++;
    }

    _sendActivityEventJSON();

    auto l = _copyCallbacks(_mtx, YBX_LW_TX_CONFIRM);
    for (auto i = 0; i < l.size(); i++) {
        l[i].txc_fcb(r);
    }
}

/* ======================================================================== */
/* Tarea de radio. Es la única que accede al radio y al nodo LoRaWAN de      */
/* RadioLib, cuyo API es síncrono (join y sendReceive bloquean segundos).    */
/* ======================================================================== */

bool YuboxLoRaWANConfigClass::_startWorker(void)
{
    YuboxLoRaWANLockGuard g(_mtx);

    if (_wk_task != NULL) {
        // Tarea ya existe. Si estaba por terminar, se cancela la terminación.
        _wk_exit = false;
        xTaskNotifyGive(_wk_task);
        return true;
    }

    _wk_exit = false;
    BaseType_t r = xTaskCreate(_workerTaskEntry, YBX_LW_TASK_NAME, YBX_LW_TASK_STACK, this, YBX_LW_TASK_PRIO, &_wk_task);
    if (r != pdPASS) {
        _wk_task = NULL;
        return false;
    }

    // La tarea recién creada espera una notificación antes de revisar si hay
    // una petición de conexión pendiente. La notificación queda guardada aunque
    // la tarea todavía no haya llegado a esperarla.
    xTaskNotifyGive(_wk_task);
    log_d("Tarea de radio LoRaWAN iniciada");
    return true;
}

void YuboxLoRaWANConfigClass::_stopWorker(void)
{
    YuboxLoRaWANLockGuard g(_mtx);

    // La tarea termina por sí misma luego de completar la operación de radio en curso
    if (_wk_task != NULL) {
        _wk_exit = true;
        xTaskNotifyGive(_wk_task);
    }
}

void YuboxLoRaWANConfigClass::_workerTaskEntry(void * p)
{
    ((YuboxLoRaWANConfigClass *)p)->_workerLoop();
    vTaskDelete(NULL);
}

void YuboxLoRaWANConfigClass::_workerLoop(void)
{
    _WorkerCtx * pCtx = new _WorkerCtx;
    _WorkerCtx & ctx = *pCtx;

    ctx.st = _WorkerCtx::WK_IDLE;
    ctx.gen = 0;
    ctx.sessionEpoch = 0;
    ctx.credEpoch = 0;
    memset(&ctx.params, 0, sizeof(ctx.params));
    ctx.node = NULL;
    ctx.nextJoinTs = 0;
    ctx.joinFails = 0;
    ctx.fcntLastFullSave = 0;
    ctx.fcntLastSaved = 0;
    ctx.sessionDirty = false;
    ctx.lastSendTs = 0;
    memset(ctx.lastNonces, 0, sizeof(ctx.lastNonces));
    ctx.classAckPending = false;
    ctx.emptyTxPending = false;
    ctx.emptyTxPort = LORAWAN_APP_PORT;

    while (true) {
        // Calcular cuánto se puede esperar una notificación antes de tener trabajo propio
        TickType_t wait = portMAX_DELAY;
        if (ctx.st == _WorkerCtx::WK_JOINING) {
            int32_t dt = (int32_t)(ctx.nextJoinTs - millis());
            wait = (dt > 0) ? pdMS_TO_TICKS(dt) : 0;
        } else if (ctx.st == _WorkerCtx::WK_JOINED) {
            if (ctx.classAckPending || ctx.emptyTxPending) {
                wait = 0;
            } else if (ctx.node->getClass() == RADIOLIB_LORAWAN_CLASS_C) {
                wait = pdMS_TO_TICKS(YBX_LW_CLASSC_POLL_MS);
            }

            // Despertar a tiempo para guardar la sesión pendiente por inactividad
            if (ctx.sessionDirty && wait > 0) {
                int32_t dt = (int32_t)(ctx.lastSendTs + YBX_LW_SESSION_FULLSAVE_IDLE_MS - millis());
                TickType_t waitSave = (dt > 0) ? pdMS_TO_TICKS(dt) : 0;
                if (waitSave < wait) wait = waitSave;
            }
        }
        ulTaskNotifyTake(pdTRUE, wait);

        if (_wkCheckExit(ctx)) break;

        bool newGen = false;
        bool haveTx = false;
        uint8_t txPort = LORAWAN_APP_PORT, txLen = 0;
        bool txConfirmed = false, txInternal = false;
        {
            YuboxLoRaWANLockGuard g(_mtx);

            if (_wk_gen != ctx.gen) {
                ctx.gen = _wk_gen;
                ctx.params = _wk_joinParams;
                ctx.sessionEpoch = _sessionEpoch;
                ctx.credEpoch = _credEpoch;
                newGen = true;
            } else if (ctx.st == _WorkerCtx::WK_JOINED && _wk_txPending) {
                txPort = _wk_txPort;
                txLen = _wk_txLen;
                memcpy(ctx.txBuf, _wk_txBuf, txLen);
                txConfirmed = _wk_txConfirmed;
                txInternal = _wk_txInternal;
                _wk_txPending = false;
                _wk_txExecuting = true;
                haveTx = true;
            }
        }

        if (newGen) {
            _wkStartSession(ctx);
            continue;
        }

        if (ctx.st == _WorkerCtx::WK_JOINING) {
            if ((int32_t)(millis() - ctx.nextJoinTs) >= 0) _wkJoinAttempt(ctx);
            continue;
        }

        if (ctx.st != _WorkerCtx::WK_JOINED) continue;

        if (haveTx) {
            _wkUplink(ctx, txPort, ctx.txBuf, txLen, txConfirmed, txInternal);
            YuboxLoRaWANLockGuard g(_mtx);
            _wk_txExecuting = false;
            continue;
        }

        if (ctx.classAckPending) {
            // Informar a la red lo más pronto posible que el cambio de clase ocurrió
            ctx.classAckPending = false;
            _wkUplink(ctx, LORAWAN_APP_PORT, NULL, 0, false, true);
            continue;
        }

        if (ctx.emptyTxPending) {
            ctx.emptyTxPending = false;
            _wkUplink(ctx, ctx.emptyTxPort, NULL, 0, false, true);
            continue;
        }

        if (ctx.sessionDirty && (int32_t)(millis() - (ctx.lastSendTs + YBX_LW_SESSION_FULLSAVE_IDLE_MS)) >= 0) {
            log_d("Se guarda sesión LoRaWAN pendiente luego de inactividad");
            _wkSaveSession(ctx, true);
            _wkSaveRTCSession(ctx);
        }

        _wkPollClassC(ctx);
    }

    delete pCtx;
    log_d("Tarea de radio LoRaWAN terminada");
}

bool YuboxLoRaWANConfigClass::_wkCheckExit(_WorkerCtx & ctx)
{
    {
        YuboxLoRaWANLockGuard g(_mtx);
        if (!_wk_exit) return false;
    }

    // Liberar el radio ANTES de confirmar la terminación, para que una nueva tarea
    // no pueda tocar el radio mientras éste se libera.
    if (ctx.node != NULL) {
        delete ctx.node;
        ctx.node = NULL;
    }
    _chipsetSleepRadio();
    ctx.st = _WorkerCtx::WK_IDLE;
    ctx.gen = 0;

    YuboxLoRaWANLockGuard g(_mtx);
    if (_wk_exit) {
        _wk_exit = false;
        _wk_task = NULL;
        return true;
    }

    // Se pidió reiniciar LoRaWAN mientras se terminaba la tarea, se continúa
    return false;
}

void YuboxLoRaWANConfigClass::_wkStartSession(_WorkerCtx & ctx)
{
    if (ctx.node != NULL) {
        delete ctx.node;
        ctx.node = NULL;
    }
    ctx.st = _WorkerCtx::WK_IDLE;
    ctx.joinFails = 0;
    ctx.classAckPending = false;
    ctx.emptyTxPending = false;
    ctx.sessionDirty = false;

    if (!_isValidLoRaWANRegion(ctx.params.region)) {
        log_e("Región LoRaWAN %u no soportada", ctx.params.region);
        _wkSetJoinStatus(ctx, YBX_LW_JOIN_FAILED);
        return;
    }

    ctx.node = new YuboxLoRaWANNode(_phy, _ybx_lw_regions[ctx.params.region].band, ctx.params.subband);
    ctx.node->setADR(true);
    ctx.node->setDutyCycle(false);

    // LoRaWAN 1.0.x: sin NwkKey, sólo AppKey (igual que SX126x-Arduino)
    int16_t state = ctx.node->beginOTAA(_eui2u64(ctx.params.appEUI), _eui2u64(ctx.params.devEUI), NULL, ctx.params.appKey);
    if (state != RADIOLIB_ERR_NONE) {
        log_e("Fallo al configurar credenciales OTAA - %d", state);
        _wkSetJoinStatus(ctx, YBX_LW_JOIN_FAILED);
        return;
    }

    // Recuperar nonces y, si es posible, la sesión completa. Primero desde la copia
    // en memoria RTC, que es la más reciente; si no es válida, desde NVRAM.
    bool restored = false;
    bool fromRTC = false;
    uint32_t savedFCntUp = 0;
    memset(ctx.lastNonces, 0, sizeof(ctx.lastNonces));
    if (_wkRestoreRTCSession(ctx)) {
        restored = true;
        fromRTC = true;
    } else {
        Preferences nvram;
        if (nvram.begin(_ns_nvram_yuboxframework_lorawan, true)) {
            if (nvram.getBytesLength(YBX_LW_NVKEY_NONCES) == RADIOLIB_LORAWAN_NONCES_BUF_SIZE) {
                uint8_t nonces[RADIOLIB_LORAWAN_NONCES_BUF_SIZE];
                nvram.getBytes(YBX_LW_NVKEY_NONCES, nonces, sizeof(nonces));
                state = ctx.node->setBufferNonces(nonces);
                if (state != RADIOLIB_ERR_NONE) {
                    log_w("Nonces LoRaWAN guardados no corresponden a configuración actual (%d), se descartan", state);
                } else {
                    memcpy(ctx.lastNonces, nonces, sizeof(nonces));

                    if (nvram.getBytesLength(YBX_LW_NVKEY_SESSION) == RADIOLIB_LORAWAN_SESSION_BUF_SIZE) {
                        uint8_t * session = new uint8_t[RADIOLIB_LORAWAN_SESSION_BUF_SIZE];
                        nvram.getBytes(YBX_LW_NVKEY_SESSION, session, RADIOLIB_LORAWAN_SESSION_BUF_SIZE);
                        state = ctx.node->setBufferSession(session);
                        delete[] session;
                        if (state != RADIOLIB_ERR_NONE) {
                            log_w("Sesión LoRaWAN guardada no es válida (%d), se descarta", state);
                        } else {
                            state = ctx.node->activateOTAA();
                            if (state == RADIOLIB_LORAWAN_SESSION_RESTORED) {
                                restored = true;
                            } else {
                                log_w("No se puede activar sesión LoRaWAN guardada (%d)", state);
                            }
                        }
                    }
                    savedFCntUp = nvram.getUInt(YBX_LW_NVKEY_FCNTUP, 0);
                }
            }
            nvram.end();
        }
    }

    if (restored) {
        if (!fromRTC) {
            // El contador de uplink más reciente puede estar guardado por separado. La
            // copia RTC no requiere margen porque reserva el contador antes de transmitir.
            uint32_t fcnt = ctx.node->getNextFCntUp();
            if (savedFCntUp > fcnt) fcnt = savedFCntUp;
            ctx.node->advanceFCntUp(fcnt + YBX_LW_FCNTUP_RESTORE_MARGIN);
            ctx.fcntLastFullSave = ctx.node->getNextFCntUp();
        }
        ctx.fcntLastSaved = 0;

        ctx.node->setClass(RADIOLIB_LORAWAN_CLASS_A);
        _wkUpdateMaxPayload(ctx);
        _chipsetSleepRadio();

        log_i("Sesión LoRaWAN restaurada desde %s (DevAddr %08x, siguiente FCntUp %u)",
            fromRTC ? "memoria RTC" : "NVRAM", ctx.node->getDevAddr(), ctx.node->getNextFCntUp());
        ctx.st = _WorkerCtx::WK_JOINED;
        _wkSaveRTCSession(ctx);
        _wkPostJoined(ctx, false);
        return;
    }

    // Sin sesión que retomar, cualquier copia RTC que exista es obsoleta
    _clearRTCSession();

    // Se requiere negociar OTAA. No se fija un datarate inicial, se deja el de RadioLib:
    // - En bandas de canales fijos (AU915, US915) el datarate del Join-Request lo define
    //   la banda según el canal (AU915: DR2 en 125 kHz, DR6 en 500 kHz), y queda como
    //   datarate de uplink luego del join hasta que ADR lo cambie.
    // - En bandas de canales dinámicos RadioLib usa por omisión DR3.

    ctx.st = _WorkerCtx::WK_JOINING;
    ctx.nextJoinTs = millis();
    _wkSetJoinStatus(ctx, YBX_LW_JOIN_ONGOING);
}

void YuboxLoRaWANConfigClass::_wkJoinAttempt(_WorkerCtx & ctx)
{
    _wkSetJoinStatus(ctx, YBX_LW_JOIN_ONGOING);

    LoRaWANJoinEvent_t joinEv;
    int16_t state = ctx.node->activateOTAA(&joinEv);

    // El DevNonce cambia con cada Join-Request enviado, y debe persistir
    _wkSaveNonces(ctx);

    if (state == RADIOLIB_LORAWAN_NEW_SESSION) {
        log_i("Unión OTAA exitosa a red LoRaWAN (DevAddr %08x)", ctx.node->getDevAddr());
        ctx.node->setClass(RADIOLIB_LORAWAN_CLASS_A);
        ctx.joinFails = 0;
        ctx.fcntLastSaved = 0;
        _wkSaveSession(ctx, true);
        _wkSaveRTCSession(ctx);
        _wkUpdateMaxPayload(ctx);
        _chipsetSleepRadio();
        log_v("Espacio libre en pila de tarea de radio: %u", uxTaskGetStackHighWaterMark(NULL));

        ctx.st = _WorkerCtx::WK_JOINED;
        _wkPostJoined(ctx, true);
        return;
    }

    _chipsetSleepRadio();
    ctx.joinFails++;
    log_d("Intento de join OTAA %u falló (%d)", ctx.joinFails, state);

    uint32_t delayMs = 0;
    if (state != RADIOLIB_ERR_NO_JOIN_ACCEPT && state != RADIOLIB_ERR_MIC_MISMATCH && state != RADIOLIB_ERR_DOWNLINK_MALFORMED) {
        // Error de radio o de configuración, no tiene sentido reintentar de inmediato
        delayMs = YBX_LW_JOIN_ERROR_DELAY_MS;
    }
    if (ctx.joinFails % JOINREQ_NBTRIALS == 0) {
        log_w("OVER_THE_AIR_ACTIVATION failed! Retrying...");
        _wkSetJoinStatus(ctx, YBX_LW_JOIN_FAILED);
        if (delayMs < YBX_LW_JOIN_RETRY_DELAY_MS) delayMs = YBX_LW_JOIN_RETRY_DELAY_MS;
    }
    ctx.nextJoinTs = millis() + delayMs;
}

void YuboxLoRaWANConfigClass::_wkUplink(_WorkerCtx & ctx, uint8_t port, const uint8_t * p, uint8_t n, bool is_txconfirmed, bool internal)
{
    static const uint8_t emptyPayload[1] = { 0 };
    if (p == NULL) {
        p = emptyPayload;
        n = 0;
    }

    // El número de transmisiones de cada uplink (NbTrans) es el que asigna la red por
    // ADR, tanto para uplinks confirmados como no confirmados, según LoRaWAN 1.0.4.
    uint8_t classBefore = ctx.node->getClass();
    uint32_t fcntBefore = ctx.node->getNextFCntUp();
    size_t lenDown = 0;
    LoRaWANEvent_t evUp, evDown;
    memset(&evUp, 0, sizeof(evUp));
    memset(&evDown, 0, sizeof(evDown));

    // Reservar el contador de trama en la copia RTC por si hay reinicio a mitad del intercambio
    _wkReserveRTCFCntUp(ctx);

    int16_t state = ctx.node->sendReceive(p, n, port, ctx.downBuf, &lenDown, is_txconfirmed, &evUp, &evDown);

    bool txOk = (state >= RADIOLIB_ERR_NONE);
    bool downlink = (state > RADIOLIB_ERR_NONE);
    bool ackOk = (is_txconfirmed && downlink && evDown.confirming);

    if (!txOk) {
        log_w("Fallo en transmisión LoRaWAN (%d)", state);
    } else {
        log_d("Uplink LoRaWAN enviado (FCnt %u, %u bytes, puerto %u, DR %u)%s",
            evUp.fCnt, n, port, evUp.datarate, downlink ? ", downlink recibido" : "");
    }

    bool classChanged = false;
    if (downlink && evDown.fPort != 0 && !evDown.multicast) {
        classChanged = _wkHandleDownlink(ctx, evDown.fPort, ctx.downBuf, lenDown);
    }

    // RadioLib avanza el contador de uplink sólo si llegó a transmitir
    if (ctx.node->getNextFCntUp() != fcntBefore) {
        ctx.lastSendTs = millis();
        ctx.sessionDirty = true;
    }
    if (downlink || classChanged || classBefore != ctx.node->getClass()) ctx.sessionDirty = true;

    bool fullSave = (ctx.sessionDirty
        && (ctx.node->getNextFCntUp() - ctx.fcntLastFullSave >= YBX_LW_SESSION_FULLSAVE_UPLINKS));
    _wkSaveSession(ctx, fullSave);
    _wkSaveRTCSession(ctx);
    _wkUpdateMaxPayload(ctx);

    if (!internal) {
        _wkPostTXResult(ctx, txOk, is_txconfirmed, ackOk);

        // Ver NOTA en send() sobre negociación de payload mayor
        if (state == RADIOLIB_ERR_PACKET_TOO_LONG) {
            ctx.emptyTxPending = true;
            ctx.emptyTxPort = port;
        }
    }

    if (ctx.node->getClass() == RADIOLIB_LORAWAN_CLASS_A) _chipsetSleepRadio();
    log_v("Espacio libre en pila de tarea de radio: %u", uxTaskGetStackHighWaterMark(NULL));
}

void YuboxLoRaWANConfigClass::_wkPollClassC(_WorkerCtx & ctx)
{
    if (ctx.node->getClass() != RADIOLIB_LORAWAN_CLASS_C) return;

    size_t lenDown = 0;
    LoRaWANEvent_t evDown;
    memset(&evDown, 0, sizeof(evDown));
    int16_t state = ctx.node->getDownlinkClassC(ctx.downBuf, &lenDown, &evDown);
    if (state > RADIOLIB_ERR_NONE) {
        if (evDown.fPort != 0 && !evDown.multicast) {
            _wkHandleDownlink(ctx, evDown.fPort, ctx.downBuf, lenDown);
        }
        ctx.sessionDirty = true;
        _wkSaveRTCSession(ctx);
        _wkUpdateMaxPayload(ctx);
        if (ctx.node->getClass() == RADIOLIB_LORAWAN_CLASS_A) _chipsetSleepRadio();
    } else if (state < RADIOLIB_ERR_NONE) {
        log_d("Error al recibir downlink en clase C (%d)", state);
    }
}

bool YuboxLoRaWANConfigClass::_wkHandleDownlink(_WorkerCtx & ctx, uint8_t port, const uint8_t * p, size_t n)
{
    // Port 3 switches the class
    if (port == 3) {
        if (n != 1) return false;

        uint8_t newClass;
        switch (p[0]) {
        case 0: newClass = RADIOLIB_LORAWAN_CLASS_A; break;
        case 1: newClass = RADIOLIB_LORAWAN_CLASS_B; break;
        case 2: newClass = RADIOLIB_LORAWAN_CLASS_C; break;
        default: return false;
        }
        int16_t state = ctx.node->setClass(newClass);
        if (state != RADIOLIB_ERR_NONE) {
            log_w("No se puede cambiar a clase %c (%d)", "ABC"[p[0]], state);
            return false;
        }
        log_i("switch to class %c done", "ABC"[p[0]]);
        ctx.classAckPending = true;
        return true;
    }

    _wkPostRX(ctx, port, p, n);
    return false;
}

void YuboxLoRaWANConfigClass::_wkSaveNonces(_WorkerCtx & ctx)
{
    uint8_t * nonces = ctx.node->getBufferNonces();
    if (0 == memcmp(nonces, ctx.lastNonces, RADIOLIB_LORAWAN_NONCES_BUF_SIZE)) return;

    YuboxLoRaWANLockGuard g(_mtx);

    // Si las credenciales se borraron o cambiaron, estos nonces ya no valen
    if (ctx.credEpoch != _credEpoch) return;

    Preferences nvram;
    nvram.begin(_ns_nvram_yuboxframework_lorawan, false);
    if (nvram.putBytes(YBX_LW_NVKEY_NONCES, nonces, RADIOLIB_LORAWAN_NONCES_BUF_SIZE) == RADIOLIB_LORAWAN_NONCES_BUF_SIZE) {
        memcpy(ctx.lastNonces, nonces, RADIOLIB_LORAWAN_NONCES_BUF_SIZE);
    } else {
        log_e("No se pueden guardar nonces LoRaWAN");
    }
    nvram.end();
}

void YuboxLoRaWANConfigClass::_wkSaveSession(_WorkerCtx & ctx, bool full)
{
    uint32_t fcnt = ctx.node->getNextFCntUp();
    if (!full && fcnt == ctx.fcntLastSaved) return;

    YuboxLoRaWANLockGuard g(_mtx);

    // Si la sesión se descartó o hay una conexión nueva, no se debe resucitar en NVRAM.
    // Tampoco tiene sentido seguir reintentando el guardado pendiente.
    if (ctx.gen != _wk_gen || ctx.sessionEpoch != _sessionEpoch) {
        ctx.sessionDirty = false;
        return;
    }

    Preferences nvram;
    nvram.begin(_ns_nvram_yuboxframework_lorawan, false);
    if (full) {
        uint8_t * session = ctx.node->getBufferSession();
        // El contador separado se reescribe también, para que nunca quede un valor de
        // una sesión anterior que adelante indebidamente el contador al restaurar.
        if (nvram.putBytes(YBX_LW_NVKEY_SESSION, session, RADIOLIB_LORAWAN_SESSION_BUF_SIZE) == RADIOLIB_LORAWAN_SESSION_BUF_SIZE
            && nvram.putUInt(YBX_LW_NVKEY_FCNTUP, fcnt)) {
            log_v("Sesión LoRaWAN guardada (siguiente FCntUp %u)", fcnt);
            ctx.fcntLastFullSave = fcnt;
            ctx.fcntLastSaved = fcnt;
            ctx.sessionDirty = false;
        } else {
            log_e("No se puede guardar sesión LoRaWAN");
        }
    } else {
        if (nvram.putUInt(YBX_LW_NVKEY_FCNTUP, fcnt)) {
            log_v("Contador uplink LoRaWAN guardado (siguiente FCntUp %u)", fcnt);
            ctx.fcntLastSaved = fcnt;
        } else {
            log_e("No se puede guardar contador uplink LoRaWAN");
        }
    }
    nvram.end();
}

void YuboxLoRaWANConfigClass::_clearRTCSession(void)
{
#ifndef YUBOX_LORAWAN_NO_RTCMEM
    if (_ybx_lw_rtcValid(_ybx_lw_rtc)) log_d("Se descarta copia de sesión LoRaWAN en memoria RTC");
    memset(&_ybx_lw_rtc, 0, sizeof(_ybx_lw_rtc));
#endif
}

bool YuboxLoRaWANConfigClass::_wkRestoreRTCSession(_WorkerCtx & ctx)
{
#ifndef YUBOX_LORAWAN_NO_RTCMEM
    // Se valida una copia local, por si el bloque se invalida desde otra tarea mientras se usa
    YuboxLoRaWANRTCSession r;
    memcpy(&r, &_ybx_lw_rtc, sizeof(r));
    if (!_ybx_lw_rtcValid(r)) {
        log_d("No hay copia válida de sesión LoRaWAN en memoria RTC");
        return false;
    }

    int16_t state = ctx.node->setBufferNonces(r.nonces);
    if (state != RADIOLIB_ERR_NONE) {
        log_w("Nonces LoRaWAN en memoria RTC no corresponden a configuración actual (%d), se descartan", state);
        return false;
    }
    state = ctx.node->setBufferSession(r.session);
    if (state != RADIOLIB_ERR_NONE) {
        log_w("Sesión LoRaWAN en memoria RTC no es válida (%d), se descarta", state);
        return false;
    }
    state = ctx.node->activateOTAA();
    if (state != RADIOLIB_LORAWAN_SESSION_RESTORED) {
        log_w("No se puede activar sesión LoRaWAN de memoria RTC (%d)", state);
        return false;
    }

    memcpy(ctx.lastNonces, r.nonces, sizeof(ctx.lastNonces));
    ctx.node->advanceFCntUp(r.fcntReserved);
    ctx.fcntLastFullSave = r.fcntLastFullSave;
    return true;
#else
    (void)ctx;
    return false;
#endif
}

void YuboxLoRaWANConfigClass::_wkSaveRTCSession(_WorkerCtx & ctx)
{
#ifndef YUBOX_LORAWAN_NO_RTCMEM
    YuboxLoRaWANLockGuard g(_mtx);

    // Mismas condiciones que para NVRAM: no resucitar una sesión o credenciales descartadas
    if (ctx.gen != _wk_gen || ctx.sessionEpoch != _sessionEpoch || ctx.credEpoch != _credEpoch) return;

    YuboxLoRaWANRTCSession r;
    memset(&r, 0, sizeof(r));
    r.magic = YBX_LW_RTC_MAGIC;
    r.layout = YBX_LW_RTC_LAYOUT;
    r.noncesSize = RADIOLIB_LORAWAN_NONCES_BUF_SIZE;
    r.sessionSize = RADIOLIB_LORAWAN_SESSION_BUF_SIZE;
    r.fcntReserved = ctx.node->getNextFCntUp();
    r.fcntLastFullSave = ctx.fcntLastFullSave;
    memcpy(r.nonces, ctx.node->getBufferNonces(), sizeof(r.nonces));
    memcpy(r.session, ctx.node->getBufferSession(), sizeof(r.session));
    r.crc = _ybx_lw_rtcCRC(r);

    // Se invalida antes de copiar, para que un reinicio a mitad de la copia no deje
    // un bloque mezclado. El CRC es lo último que se escribe.
    _ybx_lw_rtc.magic = 0;
    memcpy(&_ybx_lw_rtc, &r, sizeof(r));
#else
    (void)ctx;
#endif
}

void YuboxLoRaWANConfigClass::_wkReserveRTCFCntUp(_WorkerCtx & ctx)
{
#ifndef YUBOX_LORAWAN_NO_RTCMEM
    YuboxLoRaWANLockGuard g(_mtx);
    if (ctx.gen != _wk_gen || ctx.sessionEpoch != _sessionEpoch) return;
    if (!_ybx_lw_rtcValid(_ybx_lw_rtc)) return;

    uint32_t next = ctx.node->getNextFCntUp() + 1;
    if (_ybx_lw_rtc.fcntReserved >= next) return;
    _ybx_lw_rtc.fcntReserved = next;
    _ybx_lw_rtc.crc = _ybx_lw_rtcCRC(_ybx_lw_rtc);
#else
    (void)ctx;
#endif
}

void YuboxLoRaWANConfigClass::_wkUpdateMaxPayload(_WorkerCtx & ctx)
{
    uint8_t m = ctx.node->getMaxPayloadLen();

    YuboxLoRaWANLockGuard g(_mtx);
    if (ctx.gen == _wk_gen) _wk_maxPayloadLen = m;
}

void YuboxLoRaWANConfigClass::_wkSetJoinStatus(_WorkerCtx & ctx, uint8_t s)
{
    YuboxLoRaWANLockGuard g(_mtx);
    if (ctx.gen != _wk_gen || _joinStatus == s) return;
    _joinStatus = s;
    _ev_activity = true;
}

void YuboxLoRaWANConfigClass::_wkPostJoined(_WorkerCtx & ctx, bool newSession)
{
    YuboxLoRaWANLockGuard g(_mtx);
    if (ctx.gen != _wk_gen) return;
    _joinStatus = YBX_LW_JOIN_SET;
    _ev_joined = true;
    _ev_joinedNewSession = newSession;
}

void YuboxLoRaWANConfigClass::_wkPostRX(_WorkerCtx & ctx, uint8_t port, const uint8_t * p, size_t n)
{
    YuboxLoRaWANLockGuard g(_mtx);
    if (ctx.gen != _wk_gen) return;

    _RxEvent ev;
    ev.port = port;
    ev.data.assign(p, p + n);
    _ev_rxQueue.push_back(ev);
}

void YuboxLoRaWANConfigClass::_wkPostTXResult(_WorkerCtx & ctx, bool txOk, bool isConfirmed, bool ackOk)
{
    YuboxLoRaWANLockGuard g(_mtx);
    if (ctx.gen != _wk_gen) return;
    _ev_txDone = true;
    _ev_txOk = txOk;
    _ev_txConfirmed = isConfirmed;
    _ev_txAck = ackOk;
}

YuboxLoRaWANConfigClass YuboxLoRaWANConf;
