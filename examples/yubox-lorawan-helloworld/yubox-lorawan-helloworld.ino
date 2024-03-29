#include <YuboxSimple.h>

#include "YuboxLoRaWANConfigClass.h"

void WiFiEvent_ledStatus(WiFiEvent_t event);
void _cb_btn(void);

void lorawan_joined(void);
void lorawan_rx(uint8_t *p, uint8_t n);

void lorawan_payload(AsyncWebServerRequest * request);

// 2022-04-06: En la tarjeta YUBOX One versión 3 en adelante, ya no existe
//             control directo de LED vía GPIO_NUM_4
//#define YUBOX_LED       GPIO_NUM_4

// 2022-04-06: En la tarjeta YUBOX One versión 3 en adelante, se requiere
//             activar el step-up de 5 voltios para que LoRaWAN funcione.
#define YUBOX_ENABLE_5V   GPIO_NUM_4

#if CONFIG_IDF_TARGET_ESP32
const uint8_t PIN_WIFI_BTN = GPIO_NUM_36;
#endif

bool requestedWiFiState = false;
void setup()
{
#ifdef YUBOX_LED
  pinMode(digitalPinToInterrupt(YUBOX_LED), OUTPUT);
  digitalWrite(YUBOX_LED, LOW);
#endif
#if CONFIG_IDF_TARGET_ESP32
  pinMode(digitalPinToInterrupt(PIN_WIFI_BTN), INPUT_PULLUP);
  digitalWrite(PIN_WIFI_BTN, HIGH);
#endif
#ifdef YUBOX_ENABLE_5V
  // Se requiere activar 5V explícitamente para LoRaWAN y RS485
  pinMode(YUBOX_ENABLE_5V, OUTPUT);
  digitalWrite(YUBOX_ENABLE_5V, HIGH);
#endif

  WiFi.onEvent(WiFiEvent_ledStatus);

  // La siguiente demora es sólo para comodidad de desarrollo para enchufar el USB
  // y verlo en gtkterm. No es en lo absoluto necesaria como algoritmo requerido.
  delay(3000);
  Serial.begin(115200);
  Serial.setDebugOutput(true);

  YuboxLoRaWANConf.begin(yubox_HTTPServer);

  yubox_HTTPServer.on("/yubox-api/lorawan/payload", HTTP_POST, lorawan_payload);

  yuboxSimpleSetup();

  YuboxLoRaWANConf.onJoin(lorawan_joined);
  YuboxLoRaWANConf.onRX(lorawan_rx);
  requestedWiFiState = YuboxWiFi.haveControlOfWiFi();
  if (requestedWiFiState) {
    log_i("YUBOX Framework en control inicial de WiFi (PRENDIDO)");
  } else {
    log_i("WiFi inicialmente APAGADO");
  }
#if CONFIG_IDF_TARGET_ESP32
  attachInterrupt(digitalPinToInterrupt(PIN_WIFI_BTN), _cb_btn, RISING);
#endif
}

#define LORAWAN_APP_TX_DUTYCYCLE 10000 /**< Defines the application data transmission duty cycle. 10s, value in [ms]. */

unsigned long t = 0;
unsigned long t_send = 0;
void send_lora_frame(void);
void loop()
{
  // TODO: usar un scheduler de tareas para esto
  unsigned long tt = millis();
  if (tt - t > 100) {
    t = tt;
    yuboxSimpleLoopTask();
  }
  if (tt - t_send > LORAWAN_APP_TX_DUTYCYCLE) {
    t_send = tt;
    send_lora_frame();
  }

  if (tt <= 4000) {
    // Se ignoran transiciones anteriores a 4000 ms. de funcionamiento
    requestedWiFiState = YuboxWiFi.haveControlOfWiFi();
  } else if (requestedWiFiState != YuboxWiFi.haveControlOfWiFi()) {
    if (requestedWiFiState) {
      log_i("YUBOX Framework toma control de WiFi...");
      YuboxWiFi.takeControlOfWiFi();
      YuboxWiFi.saveControlOfWiFi();
    } else {
      log_i("YUBOX Framework cede control de WiFi y lo apaga...");
      YuboxWiFi.releaseControlOfWiFi(true);
      YuboxWiFi.saveControlOfWiFi();
    }
  }

  YuboxLoRaWANConf.update();
}

bool wifiConectado = false;
void WiFiEvent_ledStatus(WiFiEvent_t event)
{
    ESP_LOGD(__FILE__, "[WiFi-event] event: %d", event);
    switch(event) {
    case SYSTEM_EVENT_STA_GOT_IP:
#ifdef YUBOX_LED
        digitalWrite(YUBOX_LED, HIGH);
#endif
        wifiConectado = true;
        break;
    case SYSTEM_EVENT_STA_DISCONNECTED:
    case SYSTEM_EVENT_STA_STOP:
    case SYSTEM_EVENT_AP_STOP:
#ifdef YUBOX_LED
        digitalWrite(YUBOX_LED, LOW);
#endif
        wifiConectado = false;
        break;
    }
}

unsigned long lastbtn = 0;
void _cb_btn(void)
{
  if (millis() >= 5000 && (lastbtn == 0 || t - lastbtn >= 4000)) {
    lastbtn = t;
    requestedWiFiState = !requestedWiFiState;
  }
}

void lorawan_joined(void)
{
  log_i("dispositivo unido a red LoRaWAN!");
}

void lorawan_rx(uint8_t *p, uint8_t n)
{
    Serial.print("DEBUG: payload es: [");
    Serial.write(p, n);
    Serial.printf("] (%d bytes)\r\n", n);
}

String str_payload = "";
void lorawan_payload(AsyncWebServerRequest * request)
{
    YUBOX_RUN_AUTH(request);

    bool clientError = false;
    bool serverError = false;
    String responseMsg = "";
    AsyncWebParameter * p;

    if (!clientError && !request->hasParam("payload", true)) {
        clientError = true;
        responseMsg = "Payload no presente";
    }
    if (!clientError) {
        p = request->getParam("payload", true);
        str_payload = p->value();
    }


    if (!clientError && !serverError) {
        responseMsg = "Datos almacenados para siguiente actualización";
    }
    unsigned int httpCode = 200;
    if (clientError) httpCode = 400;
    if (serverError) httpCode = 500;

    AsyncResponseStream *response = request->beginResponseStream("application/json");
    response->setCode(httpCode);
#if ARDUINOJSON_VERSION_MAJOR <= 6
    DynamicJsonDocument json_doc(JSON_OBJECT_SIZE(2));
#else
    JsonDocument json_doc;
#endif
    json_doc["success"] = !(clientError || serverError);
    json_doc["msg"] = responseMsg.c_str();

    serializeJson(json_doc, *response);
    request->send(response);
}

void send_lora_frame(void)
{
    if (!YuboxLoRaWANConf.isJoined()) {
      log_w("todavía no se une a una red LoRaWAN...");
    } else {
      const char * test_payload = "Hola mundo";
      uint8_t * payload = (uint8_t *)test_payload;
      uint8_t payloadlen = strlen(test_payload);
      if (str_payload.length() > 0) {
        payload = (uint8_t *)(str_payload.c_str());
        payloadlen = str_payload.length();
      }

      log_i("INFO: enviando payload (%d bytes)... ", payloadlen);
      bool ok = YuboxLoRaWANConf.send(/*buffer*/payload, payloadlen);
      str_payload = "";
      if (ok) log_i("OK") ; else log_e("ERR");
    }
}
