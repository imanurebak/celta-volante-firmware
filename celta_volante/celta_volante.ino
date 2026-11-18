/*
 * Celta Volante - ESP32 (WROOM-32)
 *
 * VERSION: ver FIRMWARE_VERSION abajo. Subirla en cada cambio y anotarla en
 * el historial de esp32/README.md. Se ve en el Monitor serie al arrancar y
 * en el celular (Celta Hub > ⚙ > Volante ESP32).
 *
 * Lee botones DIGITALES (por ahora 4, simulados con un cable puente a GND)
 * y los manda por Bluetooth (BLE) a Celta Hub, que ejecuta la accion
 * configurada en el celular (⚙ > Botones del volante).
 *
 * Conexion (etapa 1, sin componentes):
 *   Boton 1 (RES+ arriba) = GPIO 25   Boton 2 (SET- abajo) = GPIO 26
 *   Boton 3 (Crucero)     = GPIO 27   Boton 4 (Cancelar)   = GPIO 14
 *   "Apretar" = unir ese pin con GND (cable puente o pulsador).
 *   Las resistencias internas (INPUT_PULLUP) mantienen el pin en alto si no se aprieta.
 *
 * Toque o mantener: si se suelta antes de 0,6 s se manda "toque" al soltar;
 * si se mantiene 0,6 s se manda "mantener" en ese momento (sin esperar a
 * soltar). Cada uno hace su propia accion en Celta Hub.
 *
 * LED azul de la placa (GPIO 2): parpadea lento = esperando a Celta Hub,
 * fijo = conectado, apagado un instante = boton enviado, parpadeo rapido =
 * actualizando.
 *
 * ACTUALIZACION POR BLUETOOTH (desde 1.3.0): Celta Hub manda el programa
 * nuevo (.bin) y el ESP32 lo graba en el otro espacio de memoria; si llega
 * completo y su huella SHA-256 coincide, se reinicia con el nuevo. Si algo
 * falla (se corta, se apaga), sigue con el actual. Para que haya lugar,
 * cargar UNA VEZ por cable con Herramientas > Partition Scheme >
 * "Minimal SPIFFS (1.9MB APP with OTA/190KB SPIFFS)".
 *   Control (escribir texto / notifica respuesta):
 *     "BEGIN <bytes> <sha256 hex>" -> "OK" | "ERR ..."
 *     "END"                        -> "DONE" (y se reinicia) | "ERR ..."
 *     "ABORT"                      -> "ABORTED"
 *   Datos: pedazos del .bin, en orden (escritura con respuesta).
 *
 * Usa la libreria BLE que ya viene con el paquete de placas ESP32 de
 * Arduino (no hay que instalar nada mas).
 */

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLE2902.h>
#include <Update.h>
#include "mbedtls/sha256.h"

// Mismos UUID que en Celta Hub (WheelBle.kt). No cambiarlos.
#define SERVICE_UUID "c3a1e8b0-4f2d-4c6a-9b1e-7d5f0a1c2e30"
#define BUTTON_UUID  "c3a1e8b1-4f2d-4c6a-9b1e-7d5f0a1c2e30"
#define VERSION_UUID "c3a1e8b2-4f2d-4c6a-9b1e-7d5f0a1c2e30"  // version del programa (texto, solo lectura)

// Version de este programa (Celta Hub la lee y avisa si hay una mas nueva).
#define OTA_CONTROL_UUID "c3a1e8b3-4f2d-4c6a-9b1e-7d5f0a1c2e30"  // actualizacion: ordenes y respuestas
#define OTA_DATA_UUID    "c3a1e8b4-4f2d-4c6a-9b1e-7d5f0a1c2e30"  // actualizacion: el programa en pedazos

#define FIRMWARE_VERSION "1.3.0"

const int BUTTON_PINS[] = {25, 26, 27, 14};
const int BUTTON_COUNT = sizeof(BUTTON_PINS) / sizeof(BUTTON_PINS[0]);
const int LED_PIN = 2;
const unsigned long DEBOUNCE_MS = 30;
const unsigned long LONG_PRESS_MS = 600;  // mantener apretado este tiempo = "mantener"

// Tipo de pulsacion que se manda (segundo byte del mensaje).
const uint8_t PRESS_TAP = 1;
const uint8_t PRESS_HOLD = 2;

BLECharacteristic* buttonCharacteristic = nullptr;
volatile bool connected = false;

// Actualizacion por Bluetooth.
BLECharacteristic* otaControl = nullptr;
bool otaActive = false;
size_t otaSize = 0;
size_t otaReceived = 0;
String otaExpectedSha = "";
mbedtls_sha256_context otaSha;
unsigned long restartAt = 0;
unsigned long lastOtaLog = 0;

void otaReply(const String& text) {
  otaControl->setValue(text.c_str());
  otaControl->notify();
  Serial.println(String("Actualizacion: ") + text);
}

void otaAbort(const char* why) {
  if (otaActive) {
    Update.abort();
    mbedtls_sha256_free(&otaSha);
  }
  otaActive = false;
  Serial.printf("Actualizacion cancelada: %s\n", why);
}

String bytesToText(BLECharacteristic* c) {
  String text = "";
  uint8_t* data = c->getData();
  size_t length = c->getLength();
  for (size_t i = 0; i < length; i++) text += (char)data[i];
  text.trim();
  return text;
}

class OtaControlCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    String command = bytesToText(c);
    if (command.startsWith("BEGIN ")) {
      int space = command.indexOf(' ', 6);
      if (space < 0) {
        otaReply("ERR orden incompleta");
        return;
      }
      size_t size = (size_t)command.substring(6, space).toInt();
      String sha = command.substring(space + 1);
      sha.toLowerCase();
      if (size == 0 || sha.length() != 64) {
        otaReply("ERR datos invalidos");
        return;
      }
      if (otaActive) otaAbort("nueva actualizacion");
      if (!Update.begin(size)) {
        otaReply("ERR no hay lugar (cargar una vez por cable con Partition Scheme: Minimal SPIFFS)");
        return;
      }
      mbedtls_sha256_init(&otaSha);
      mbedtls_sha256_starts(&otaSha, 0);
      otaSize = size;
      otaReceived = 0;
      otaExpectedSha = sha;
      otaActive = true;
      Serial.printf("Actualizacion: recibiendo %u bytes\n", (unsigned)size);
      otaReply("OK");
    } else if (command == "END") {
      if (!otaActive) {
        otaReply("ERR no habia actualizacion");
        return;
      }
      uint8_t digest[32];
      mbedtls_sha256_finish(&otaSha, digest);
      mbedtls_sha256_free(&otaSha);
      char hex[65];
      for (int i = 0; i < 32; i++) sprintf(hex + i * 2, "%02x", digest[i]);
      hex[64] = 0;
      otaActive = false;
      if (otaReceived != otaSize) {
        Update.abort();
        otaReply("ERR faltan datos");
      } else if (otaExpectedSha != String(hex)) {
        Update.abort();
        otaReply("ERR huella distinta (archivo danado)");
      } else if (!Update.end(true)) {
        otaReply(String("ERR al grabar: ") + Update.errorString());
      } else {
        otaReply("DONE");
        restartAt = millis() + 1500;  // reiniciar con el programa nuevo
      }
    } else if (command == "ABORT") {
      otaAbort("pedido de Celta Hub");
      otaReply("ABORTED");
    } else {
      otaReply("ERR orden desconocida");
    }
  }
};

class OtaDataCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    if (!otaActive) return;
    uint8_t* data = c->getData();
    size_t length = c->getLength();
    if (otaReceived + length > otaSize) {
      otaAbort("llegaron datos de mas");
      otaReply("ERR datos de mas");
      return;
    }
    if (Update.write(data, length) != length) {
      otaAbort("error al grabar");
      otaReply(String("ERR al grabar: ") + Update.errorString());
      return;
    }
    mbedtls_sha256_update(&otaSha, data, length);
    otaReceived += length;
    if (millis() - lastOtaLog > 2000) {
      lastOtaLog = millis();
      Serial.printf("Actualizacion: %u%%\n", (unsigned)(otaReceived * 100 / otaSize));
    }
  }
};

// Estado de cada boton para filtrar rebotes (el contacto "vibra" al apretar).
bool stablePressed[BUTTON_COUNT];
bool lastReading[BUTTON_COUNT];
unsigned long lastChange[BUTTON_COUNT];
unsigned long pressedAt[BUTTON_COUNT];
bool holdSent[BUTTON_COUNT];

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* server) override {
    connected = true;
    Serial.println("Celta Hub conectado (Celta Volante v" FIRMWARE_VERSION ")");
  }
  void onDisconnect(BLEServer* server) override {
    connected = false;
    if (otaActive) otaAbort("se corto la conexion");
    Serial.println("Celta Hub desconectado, esperando de nuevo...");
    delay(200);
    BLEDevice::startAdvertising();  // vuelve a quedar visible
  }
};

void sendButton(int number, uint8_t kind) {
  // Mensaje: [numero de boton (1..4), 1 = toque / 2 = mantener]
  uint8_t message[2] = {(uint8_t)number, kind};
  buttonCharacteristic->setValue(message, 2);
  if (connected) {
    buttonCharacteristic->notify();
  }
  Serial.printf("Boton %d %s %s\n", number, kind == PRESS_HOLD ? "MANTENER" : "toque",
                connected ? "enviado" : "(sin conexion)");
  digitalWrite(LED_PIN, LOW);
  delay(80);
  digitalWrite(LED_PIN, connected ? HIGH : LOW);
}

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  for (int i = 0; i < BUTTON_COUNT; i++) {
    pinMode(BUTTON_PINS[i], INPUT_PULLUP);
    stablePressed[i] = false;
    lastReading[i] = false;
    lastChange[i] = 0;
    pressedAt[i] = 0;
    holdSent[i] = false;
  }

  BLEDevice::init("Celta Volante");
  BLEDevice::setMTU(517);  // pedazos grandes: la actualizacion tarda menos
  BLEServer* server = BLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());

  BLEService* service = server->createService(SERVICE_UUID);
  buttonCharacteristic = service->createCharacteristic(
      BUTTON_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  buttonCharacteristic->addDescriptor(new BLE2902());  // permite las notificaciones
  uint8_t initial[2] = {0, 0};
  buttonCharacteristic->setValue(initial, 2);

  BLECharacteristic* versionCharacteristic = service->createCharacteristic(
      VERSION_UUID, BLECharacteristic::PROPERTY_READ);
  versionCharacteristic->setValue(FIRMWARE_VERSION);

  otaControl = service->createCharacteristic(
      OTA_CONTROL_UUID, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_NOTIFY);
  otaControl->addDescriptor(new BLE2902());
  otaControl->setCallbacks(new OtaControlCallbacks());
  BLECharacteristic* otaData = service->createCharacteristic(
      OTA_DATA_UUID, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  otaData->setCallbacks(new OtaDataCallbacks());
  service->start();

  BLEAdvertising* advertising = BLEDevice::getAdvertising();
  advertising->addServiceUUID(SERVICE_UUID);
  advertising->setScanResponse(true);
  BLEDevice::startAdvertising();

  Serial.println("Celta Volante v" FIRMWARE_VERSION " listo. Esperando a Celta Hub...");
}

void loop() {
  unsigned long now = millis();
  // Actualizacion terminada: reiniciar con el programa nuevo.
  if (restartAt != 0 && now >= restartAt) {
    Serial.println("Reiniciando con el programa nuevo...");
    delay(100);
    ESP.restart();
  }
  for (int i = 0; i < BUTTON_COUNT; i++) {
    bool reading = digitalRead(BUTTON_PINS[i]) == LOW;  // LOW = unido a GND = apretado
    if (reading != lastReading[i]) {
      lastReading[i] = reading;
      lastChange[i] = now;
    }
    if (now - lastChange[i] > DEBOUNCE_MS && reading != stablePressed[i]) {
      stablePressed[i] = reading;
      if (reading) {
        // Se apreto: se espera a ver si es toque o mantener.
        pressedAt[i] = now;
        holdSent[i] = false;
      } else if (!holdSent[i]) {
        sendButton(i + 1, PRESS_TAP);  // se solto antes de 0,6 s
      }
    }
    // Sigue apretado 0,6 s: "mantener" (una sola vez por pulsacion).
    if (stablePressed[i] && !holdSent[i] && now - pressedAt[i] >= LONG_PRESS_MS) {
      holdSent[i] = true;
      sendButton(i + 1, PRESS_HOLD);
    }
  }

  // LED: parpadeo rapido actualizando, fijo si hay conexion, parpadeo lento si no.
  if (otaActive) {
    digitalWrite(LED_PIN, (now / 100) % 2 ? HIGH : LOW);
  } else if (connected) {
    digitalWrite(LED_PIN, HIGH);
  } else {
    digitalWrite(LED_PIN, (now / 500) % 2 ? HIGH : LOW);
  }
  delay(5);
}
