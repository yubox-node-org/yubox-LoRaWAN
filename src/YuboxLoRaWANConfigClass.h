#ifndef _YUBOX_LORAWAN_CONF_CLASS_H_
#define _YUBOX_LORAWAN_CONF_CLASS_H_

#include <ESPAsyncWebServer.h>
#include <Preferences.h>

#define ARDUINOJSON_USE_LONG_LONG 1

#include "AsyncJson.h"
#include "ArduinoJson.h"

#include <LoRaWan-Arduino.h>

#include <functional>

typedef void (*YuboxLoRaWAN_join_cb)(void);
typedef std::function<void (void) > YuboxLoRaWAN_join_func_cb;
typedef void (*YuboxLoRaWAN_rx_cb)(uint8_t *, uint8_t);
typedef std::function<void (uint8_t *, uint8_t) > YuboxLoRaWAN_rx_func_cb;
typedef void (*YuboxLoRaWAN_txdutychange_cb)(void);
typedef std::function<void (void) > YuboxLoRaWAN_txdutychange_func_cb;

typedef size_t yuboxlorawan_event_id_t;

class YuboxLoRaWANConfigClass
{
private:
  static const char * _ns_nvram_yuboxframework_lorawan;

  // Región de LoRaWAN a usar para configuración
  LoRaMacRegion_t _lw_region;

  // Sub-banda a usar para conexión inicial a red
  unsigned int _lw_subband;

  // Identificador sacado de MAC de ESP32, convertido en EUI
  uint8_t _lw_default_devEUI[8];

  // Credenciales e identificación de aplicación LoRaWAN
  uint8_t _lw_devEUI[8];
  uint8_t _lw_appEUI[8];
  uint8_t _lw_appKey[16];
  bool _lw_confExists;
  bool _lw_needsInit;

  // Se establece bandera a VERDADERO luego de inicio exitoso de hardware LoRaWAN
  bool _lorahw_init;

  // Si ocurre un error de TX LUEGO de que se recibe la indicación de
  // JOIN a la red, se asigna este valor. Este valor se vuelve a poner
  // a cero luego de cada transmisión exitosa.
  uint32_t _ts_errorAfterJoin;

  // La siguiente estructura necesita existir por toda la vida de la sesión LoRaWAN
  lmh_callback_t _lora_callbacks;

  AsyncEventSource * _pEvents;

  // Timestamps de últimas actividades LoRaWAN
  uint32_t _ts_ultimoTX_OK;
  uint32_t _ts_ultimoTX_FAIL;
  uint32_t _ts_ultimoRX;

  // Intervalo en segundos de TX DUTY configurado para aplicación. Es responsabilidad
  // de la aplicación instalar un callback o de otra forma recoger el valor, y
  // actualizarse para respetar este intervalo de TX DUTY
  uint32_t _tx_duty_sec;
  bool _tx_duty_sec_changed;

  void _loadSavedCredentialsFromNVRAM(void);
  bool _saveCredentialsToNVRAM(void);

  void _setupHTTPRoutes(AsyncWebServer &);

  String _reportActivityJSON(void);

  void _routeHandler_yuboxAPI_lorawan_status_onConnect(AsyncEventSourceClient *);
  void _routeHandler_yuboxAPI_lorawanconfigjson_GET(AsyncWebServerRequest *);
  void _routeHandler_yuboxAPI_lorawanconfigjson_POST(AsyncWebServerRequest *);

  String _bin2str(uint8_t *, size_t);
  bool _str2bin(const char *, uint8_t *, size_t);

  void _txdutychange_handler(void);
public:
  YuboxLoRaWANConfigClass(void);
  bool begin(AsyncWebServer & srv);

  // Función a llamar regularmente para procesar eventos de radio
  void update(void);

  // Verificar si efectivamente ya se ha unido a red LoRaWAN
  bool isJoined(void);

  // Instalar callback para aviso de unión exitosa a LoRaWAN
  yuboxlorawan_event_id_t onJoin(YuboxLoRaWAN_join_cb cbRX);
  yuboxlorawan_event_id_t onJoin(YuboxLoRaWAN_join_func_cb cbRX);
  void removeJoin(YuboxLoRaWAN_join_cb cbRX);
  void removeJoin(yuboxlorawan_event_id_t id);

  // Instalar callback para recepción de datos LoRaWAN
  yuboxlorawan_event_id_t onRX(YuboxLoRaWAN_rx_cb cbRX);
  yuboxlorawan_event_id_t onRX(YuboxLoRaWAN_rx_func_cb cbRX);
  void removeRX(YuboxLoRaWAN_rx_cb cbRX);
  void removeRX(yuboxlorawan_event_id_t id);

  // Instalar callback para cambio de duración de TX DUTY requerido
  yuboxlorawan_event_id_t onTXDuty(YuboxLoRaWAN_txdutychange_cb cb);
  yuboxlorawan_event_id_t onTXDuty(YuboxLoRaWAN_txdutychange_func_cb cb);
  void removeTXDuty(YuboxLoRaWAN_txdutychange_cb cb);
  void removeTXDuty(yuboxlorawan_event_id_t id);

  uint32_t getRequestedTXDutyCycle(void) { return _tx_duty_sec; }
  bool setRequestedTXDutyCycle(uint32_t);

  // Enviar datos una vez confirmado que hay enlace a red
  bool send(uint8_t * p, uint8_t n, bool is_txconfirmed = false);

  // NO LLAMAR DESDE CÓDIGO LAS SIGUIENTES FUNCIONES
  void _joinstart_handler(void);
  void _join_handler(void);
  void _joinfail_handler(void);
  void _rx_handler(uint8_t *, uint8_t);
};

extern YuboxLoRaWANConfigClass YuboxLoRaWANConf;

#endif
