#ifndef _YUBOX_LORAWAN_CONF_CLASS_H_
#define _YUBOX_LORAWAN_CONF_CLASS_H_

#include <ESPAsyncWebServer.h>
#include <Preferences.h>

#define ARDUINOJSON_USE_LONG_LONG 1

#include "AsyncJson.h"
#include "ArduinoJson.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>
#include <deque>
#include <vector>

// Puerto LoRaWAN por omisión para datos de aplicación. Este valor era provisto
// por SX126x-Arduino a través de este mismo header, y se preserva con el mismo
// valor porque hay firmwares que lo usan directamente.
#ifndef LORAWAN_APP_PORT
#define LORAWAN_APP_PORT 2
#endif

typedef std::function<void (void) > YuboxLoRaWAN_join_func_cb;
typedef std::function<void (uint8_t *, uint8_t) > YuboxLoRaWAN_rx_func_cb;
typedef std::function<void (void) > YuboxLoRaWAN_txdutychange_func_cb;
typedef std::function<void (bool) > YuboxLoRaWAN_txconfirm_func_cb;

typedef size_t yuboxlorawan_event_id_t;

// Declaraciones adelantadas de RadioLib, para no exponer RadioLib.h a la aplicación
class PhysicalLayer;
class YuboxLoRaWANNode;

class YuboxLoRaWANConfigClass
{
private:
  static const char * _ns_nvram_yuboxframework_lorawan;

  // Región de LoRaWAN a usar para configuración. El valor numérico es el mismo
  // que se usaba con SX126x-Arduino (enum LoRaMacRegion_t), para preservar el
  // significado del valor guardado en NVRAM y del API web.
  uint8_t _lw_region;

  // Sub-banda a usar para conexión inicial a red
  uint8_t _lw_subband;

  // Si VERDADERO, la región/subbanda en NVRAM no era utilizable y se asumió
  // otra en memoria. El siguiente guardado desde la web debe escribirlas.
  bool _lw_regionNeedsSave;

  // Identificador sacado de MAC de ESP32, convertido en EUI
  uint8_t _lw_default_devEUI[8];

  // Credenciales e identificación de aplicación LoRaWAN
  uint8_t _lw_devEUI[8];
  uint8_t _lw_appEUI[8];
  uint8_t _lw_appKey[16];
  bool _lw_confExists;
  bool _lw_needsInit;

  bool _lw_deleteConf;

  // VERDADERO si no hay sesión LoRaWAN establecida o restaurada, y por lo tanto
  // la siguiente conexión requiere negociación OTAA.
  bool _lw_useOTAA;

  // Se establece bandera a VERDADERO luego de inicio exitoso de hardware LoRaWAN
  bool _lorahw_init;

  // Si ocurre un error de TX LUEGO de que se recibe la indicación de
  // JOIN a la red, se asigna este valor. Este valor se vuelve a poner
  // a cero luego de cada transmisión exitosa.
  uint32_t _ts_errorAfterJoin;

  AsyncEventSource * _pEvents;

  // Timestamps de últimas actividades LoRaWAN
  uint32_t _ts_ultimoTX_OK;
  uint32_t _ts_ultimoTX_FAIL;
  uint32_t _ts_ultimoRX;
  uint32_t _ts_lastDownlinkActivity;

  // Intervalo en segundos de TX DUTY configurado para aplicación. Es responsabilidad
  // de la aplicación instalar un callback o de otra forma recoger el valor, y
  // actualizarse para respetar este intervalo de TX DUTY
  uint32_t _tx_default_duty_sec;
  uint32_t _tx_duty_sec;
  bool _tx_duty_sec_changed;

  // Bandera de reportar si se está esperando confirmación de una transmisión
  bool _tx_waiting_confirm;
  uint32_t _ts_confirmTX_start;
  uint32_t _num_confirmTX_OK;
  uint32_t _num_confirmTX_FAIL;

  // Bandera de si mostrar o no la configuración de transmisión confirmada
  bool _tx_conf_display;

  // Número de veces que se reintentará la transmisión confirmada luego de fallo
  uint32_t _tx_conf_num_retries;

  // Número de puerto de la última recepción downlink
  bool _enable_all_ports;
  uint8_t _last_rx_dataport;

  // ---- Estado compartido con la tarea de radio. Protegido por _mtx ----

  // Candado recursivo que protege todo el estado compartido entre la tarea
  // de radio, el loop de la aplicación, y los manejadores web.
  SemaphoreHandle_t _mtx;

  // Capa física de radio (independiente del chipset concreto)
  PhysicalLayer * _phy;

  // Tarea de radio. Es NULL si la tarea no está corriendo.
  TaskHandle_t _wk_task;
  bool _wk_exit;

  // Generación de la petición de conexión. Cada reinicio de LoRaWAN incrementa
  // este número, y la tarea de radio descarta todo trabajo de generaciones previas.
  uint32_t _wk_gen;

  struct _JoinParams {
    uint8_t devEUI[8];
    uint8_t appEUI[8];
    uint8_t appKey[16];
    uint8_t region;
    uint8_t subband;
  };
  _JoinParams _wk_joinParams;

  // Épocas de validez de NVRAM. La tarea de radio sólo escribe la sesión (o los
  // nonces) si la época correspondiente no ha cambiado desde que inició la
  // conexión. Así se evita resucitar en NVRAM datos ya borrados.
  uint32_t _sessionEpoch;
  uint32_t _credEpoch;

  // Estado de unión a la red LoRaWAN, ver YBX_LW_JOIN_* en implementación
  uint8_t _joinStatus;

  // Ranura de transmisión pendiente (una sola transmisión a la vez)
  bool _wk_txPending;
  bool _wk_txExecuting;
  bool _wk_txConfirmed;
  bool _wk_txInternal;
  uint8_t _wk_txPort;
  uint8_t _wk_txLen;
  uint8_t _wk_txBuf[255];

  // Máximo tamaño de payload actualmente aceptado por la red
  uint8_t _wk_maxPayloadLen;

  // Eventos generados por la tarea de radio, despachados en update()
  struct _RxEvent {
    uint8_t port;
    std::vector<uint8_t> data;
  };
  bool _ev_activity;
  bool _ev_joined;
  bool _ev_joinedNewSession;
  bool _ev_txDone;
  bool _ev_txOk;
  bool _ev_txConfirmed;
  bool _ev_txAck;
  std::deque<_RxEvent> _ev_rxQueue;

  struct _WorkerCtx;

  void _lock(void);
  void _unlock(void);

  void _loadSavedCredentialsFromNVRAM(void);
  bool _saveCredentialsToNVRAM(void);
  bool _saveRegionToNVRAM(void);
  void _clearSessionKeys(void);
  void _destroySessionKeys(Preferences &);
  void _removeLegacySessionKeys(void);

  bool _saveConfirmedTXRetries(void);

  bool _isValidLoRaWANRegion(uint8_t);
  uint8_t _getMaxLoRaWANRegionSubchannel(uint8_t);
  const char * _getLoRaWANRegionName(uint8_t);

  void _setupHTTPRoutes(AsyncWebServer &);

  uint8_t _getJoinStatus(void);
  const char * _getJoinStatusName(void);
  void _sendActivityEventJSON(void);
  String _reportActivityJSON(void);

  void _routeHandler_yuboxAPI_lorawan_status_onConnect(AsyncEventSourceClient *);
  void _routeHandler_yuboxAPI_lorawanconfigjson_GET(AsyncWebServerRequest *);
  void _routeHandler_yuboxAPI_lorawanconfigjson_POST(AsyncWebServerRequest *);
  void _routeHandler_yuboxAPI_lorawanregionsjson_GET(AsyncWebServerRequest *);
  void _routeHandler_yuboxAPI_lorawanresetconn_POST(AsyncWebServerRequest *);
  void _routeHandler_yuboxAPI_lorawandeletecred_POST(AsyncWebServerRequest *);

  String _bin2str(uint8_t *, size_t);
  bool _str2bin(const char *, uint8_t *, size_t);

  // Manejadores de eventos, ejecutados desde update()
  void _processWorkerEvents(void);
  void _txdutychange_handler(void);
  void _joinstart_handler(void);
  void _join_handler(bool newSession);
  void _rx_handler(uint8_t *, uint8_t, uint8_t);
  void _tx_result_handler(bool txOk, bool isConfirmed, bool ackOk);
  void _tx_confirmed_result(bool);
  bool _registerTXFailure(void);
  bool _enqueueTX(uint8_t port, uint8_t * p, uint8_t n, bool is_txconfirmed, bool internal);

  // Funciones específicas de chipset de radio
  bool _chipsetInitRadio(void);
  void _chipsetSleepRadio(void);
  const char * _chipsetName(void);

  // Manejo de la tarea de radio
  bool _startWorker(void);
  void _stopWorker(void);
  static void _workerTaskEntry(void *);
  void _workerLoop(void);
  bool _wkCheckExit(_WorkerCtx &);
  void _wkStartSession(_WorkerCtx &);
  void _wkJoinAttempt(_WorkerCtx &);
  void _wkUplink(_WorkerCtx &, uint8_t port, const uint8_t * p, uint8_t n, bool is_txconfirmed, bool internal);
  void _wkPollClassC(_WorkerCtx &);
  bool _wkHandleDownlink(_WorkerCtx &, uint8_t port, const uint8_t * p, size_t n);
  void _wkSaveNonces(_WorkerCtx &);
  void _wkSaveSession(_WorkerCtx &, bool full);
  void _wkSetJoinStatus(_WorkerCtx &, uint8_t);
  void _wkUpdateMaxPayload(_WorkerCtx &);
  void _wkPostJoined(_WorkerCtx &, bool newSession);
  void _wkPostRX(_WorkerCtx &, uint8_t port, const uint8_t * p, size_t n);
  void _wkPostTXResult(_WorkerCtx &, bool txOk, bool isConfirmed, bool ackOk);
public:
  YuboxLoRaWANConfigClass(void);
  bool begin(AsyncWebServer & srv, bool displayTxConf = false);

  // Función a llamar regularmente para procesar eventos de radio
  void update(void);

  // Verificar si efectivamente ya se ha unido a red LoRaWAN
  bool isJoined(void);

  // Verificar si se está todavía esperando la confirmación de una transmisión confirmada
  bool isWaitingConfirmation(void) { return _tx_waiting_confirm; }

  // Destruir las claves de sesión y volver a empezar el join
  void destroySessionKeys(void);

  // Destruir claves de sesión Y credenciales OTAA, y parar LoRaWAN hasta volver a ingresar credenciales
  void destroySavedCredentials(void);

  // Instalar callback para aviso de unión exitosa a LoRaWAN
  yuboxlorawan_event_id_t onJoin(YuboxLoRaWAN_join_func_cb cbRX);
  void removeJoin(yuboxlorawan_event_id_t id);

  // Instalar callback para recepción de datos LoRaWAN
  yuboxlorawan_event_id_t onRX(YuboxLoRaWAN_rx_func_cb cbRX);
  void removeRX(yuboxlorawan_event_id_t id);

  // Instalar callback para cambio de duración de TX DUTY requerido
  yuboxlorawan_event_id_t onTXDuty(YuboxLoRaWAN_txdutychange_func_cb cb);
  void removeTXDuty(yuboxlorawan_event_id_t id);

  // Instalar callback para confirmación de éxito o fallo de TX confirmado
  yuboxlorawan_event_id_t onTXConfirm(YuboxLoRaWAN_txconfirm_func_cb cb);
  void removeTXConfirm(yuboxlorawan_event_id_t cb);

  void setDefaultTXDutyCycle(uint32_t);
  uint32_t getRequestedTXDutyCycle(void) { return _tx_duty_sec; }
  bool setRequestedTXDutyCycle(uint32_t);

  // Enviar datos una vez confirmado que hay enlace a red
  bool send(uint8_t * p, uint8_t n, bool is_txconfirmed = false) { return send(LORAWAN_APP_PORT, p, n, is_txconfirmed); }
  bool send(uint8_t app_port, uint8_t * p, uint8_t n, bool is_txconfirmed = false);

  uint32_t getNumTxConfRetries(void) { return _tx_conf_num_retries; }

  uint32_t getLastDownlinkActivity(void) { return _ts_lastDownlinkActivity; }
  uint32_t getLastRxDataPort(void) { return _last_rx_dataport; }
  void setEnableAllDataPorts(bool b) { _enable_all_ports = b; }
};

extern YuboxLoRaWANConfigClass YuboxLoRaWANConf;

#endif
