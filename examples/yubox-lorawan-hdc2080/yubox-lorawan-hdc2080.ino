#include <Arduino.h>
#include <Wire.h>
#include <HDC2080.h>
#include <YuboxSimple.h>
#include <TaskScheduler.h>

#include "YuboxLoRaWANConfigClass.h"

#define ARDUINOJSON_USE_LONG_LONG 1
#include <ArduinoJson.h>

static void lorawan_joined(void);
static void lorawan_txdutychange(void);
static void lorawan_rx(uint8_t *p, uint8_t n);

static Scheduler yuboxScheduler;

static void yuboxUpdateNTP(void);
Task task_yuboxUpdateNTP( TASK_SECOND * 5, TASK_FOREVER, &yuboxUpdateNTP, &yuboxScheduler, true );

static void yuboxUpdateGraph(void);
Task task_yuboxUpdateGraph( TASK_SECOND * 5, TASK_FOREVER, &yuboxUpdateGraph, &yuboxScheduler, true );

static void yuboxUploadLoRaWAN(void);
Task task_yuboxUploadLoRaWAN( TASK_SECOND * 10, TASK_FOREVER, &yuboxUploadLoRaWAN, &yuboxScheduler, true );

#if CONFIG_IDF_TARGET_ESP32
// Pines a usar para I2C
#define YUBOX_I2C_SDA               SDA
#define YUBOX_I2C_SCL               SCL

#elif CONFIG_IDF_TARGET_ESP32S2
// Pines a usar para I2C
#define YUBOX_I2C_SDA               GPIO_NUM_40
#define YUBOX_I2C_SCL               GPIO_NUM_41

// 2022-04-06: En la tarjeta YUBOX One versión 3 en adelante, se requiere
//             activar el step-up de 5 voltios para que LoRaWAN funcione.
#define YUBOX_ENABLE_5V   GPIO_NUM_4

#elif CONFIG_IDF_TARGET_ESP32S3
// Pines a usar para I2C
#define YUBOX_I2C_SDA               GPIO_NUM_40
#define YUBOX_I2C_SCL               GPIO_NUM_41

#define YUBOX_ENABLE_5V   GPIO_NUM_4

#else
#error Pines de control no definidos para board objetivo!
#endif

//#define HDC2010_I2C_ADDR    0x41
#define HDC2010_I2C_ADDR    0x40
static HDC2080 sensor_hdc2080(HDC2010_I2C_ADDR);
static bool ok_sensor_hdc2080 = false;

AsyncEventSource eventosLector("/yubox-api/lectura/events");

void setup()
{
#ifdef YUBOX_ENABLE_5V
  // Se requiere activar 5V explícitamente para LoRaWAN y RS485
  pinMode(YUBOX_ENABLE_5V, OUTPUT);
  digitalWrite(YUBOX_ENABLE_5V, HIGH);
#endif

  // La siguiente demora es sólo para comodidad de desarrollo para enchufar el USB
  // y verlo en gtkterm. No es en lo absoluto necesaria como algoritmo requerido.
  //delay(3000);
  Serial.begin(115200);

  // Iniciar Libreria Wire con declaración de pines I2C de la Yubox Industrial
  Wire.begin(YUBOX_I2C_SDA, YUBOX_I2C_SCL); 

  iniciarSensores();

  YuboxLoRaWANConf.begin(yubox_HTTPServer);

  yuboxAddManagedHandler(&eventosLector);

  yuboxSimpleSetup();

  YuboxLoRaWANConf.onJoin(lorawan_joined);
  YuboxLoRaWANConf.onRX(lorawan_rx);
  YuboxLoRaWANConf.onTXDuty(lorawan_txdutychange);

  log_i("Intervalo de TX DUTY LoRaWAN inicia en %u segundos", YuboxLoRaWANConf.getRequestedTXDutyCycle());
  task_yuboxUploadLoRaWAN.setInterval(TASK_SECOND * YuboxLoRaWANConf.getRequestedTXDutyCycle());
}

void loop()
{
  YuboxLoRaWANConf.update();
  yuboxScheduler.execute();
}

static bool test_i2cdev_presente(uint8_t i2c_addr)
{
    // Verificar si dispositivo está presente...
    Wire.beginTransmission(i2c_addr);
    auto r = Wire.endTransmission();
    if (r != 0) {
        log_w("No se encuentra dispositivo I2C esperado en dirección 0x%02x, error %u", i2c_addr, r);
    }
    return (r == 0);
}

static void iniciarSensores(void)
{
  if (!test_i2cdev_presente(HDC2010_I2C_ADDR)) return;

  // Aparentemente no hay manera de verificar si el HDC2010 se inicializó correctamente
  sensor_hdc2080.begin();
  sensor_hdc2080.reset(); // Begin with a device reset
  sensor_hdc2080.setMeasurementMode(TEMP_AND_HUMID);  // Set measurements to temperature and humidity
  sensor_hdc2080.setRate(ONE_HZ);                     // Set measurement frequency to 1 Hz
  sensor_hdc2080.setTempRes(FOURTEEN_BIT);
  sensor_hdc2080.setHumidRes(FOURTEEN_BIT);
  sensor_hdc2080.triggerMeasurement();

  ok_sensor_hdc2080 = true;
}

static void yuboxUpdateNTP(void)
{
    if (!YuboxNTPConf.update(0)) {
        if (WiFi.isConnected()) log_e("fallo al obtener hora de red");
    } else {
        // TODO: activar banderas por ser NTP válido
    }
}

static void lorawan_joined(void)
{
  log_i("dispositivo unido a red LoRaWAN!");
}

static void lorawan_rx(uint8_t *p, uint8_t n)
{
    Serial.print("DEBUG: payload es: [");
    Serial.write(p, n);
    Serial.printf("] (%d bytes)\r\n", n);
}

static void lorawan_txdutychange(void)
{
    log_i("Intervalo de TX DUTY LoRaWAN es ahora %u segundos", YuboxLoRaWANConf.getRequestedTXDutyCycle());
    task_yuboxUploadLoRaWAN.setInterval(TASK_SECOND * YuboxLoRaWANConf.getRequestedTXDutyCycle());
}

static void yuboxUploadLoRaWAN(void)
{
    if (!YuboxLoRaWANConf.isJoined()) {
      log_w("todavía no se une a una red LoRaWAN...");
    } else {
      String json_payload = crearPayloadSensor();
      uint8_t * payload = (uint8_t *)json_payload.c_str();
      uint8_t payloadlen = json_payload.length();

      log_i("INFO: enviando payload (%d bytes)... ", payloadlen);
      bool ok = YuboxLoRaWANConf.send(payload, payloadlen);
      if (ok) log_i("OK") ; else log_e("ERR");
    }
}

static String crearPayloadSensor(void)
{
  JsonDocument json_doc;

  if (ok_sensor_hdc2080) {
    auto temp = sensor_hdc2080.readTemp();
    auto hum = sensor_hdc2080.readHumidity();

    json_doc["ts"] = 1000ULL * YuboxNTPConf.getUTCTime();
    json_doc["temp"] = temp;
    json_doc["hum"] = hum;
  }

  String jsonstr;
  serializeJson(json_doc, jsonstr);
  return jsonstr;
}

static void yuboxUpdateGraph(void)
{
    if (eventosLector.count() > 0) {
      String json_payload = crearPayloadSensor();
      eventosLector.send(json_payload.c_str());
    }
}