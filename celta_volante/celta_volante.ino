/*
 * Auto Hub Volante - ESP32 (WROOM-32)  (antes "Celta Volante")
 *
 * VERSION: ver FIRMWARE_VERSION abajo. Subirla en cada cambio y anotarla en
 * el historial de esp32/README.md. Se ve en el Monitor serie al arrancar y
 * en el celular (Celta Hub > ⚙ > Volante ESP32).
 *
 * Lee los comandos del volante Chevrolet (Onix / Tracker) y los manda por
 * Bluetooth (BLE) a Auto Hub, que ejecuta la accion configurada en el
 * celular (Volante > perfiles).
 *
 * COMANDOS DEL VOLANTE (desde 1.5.0): cada lado es una "escalera de
 * resistencias": un solo cable de senal y cada tecla da otra resistencia.
 *   Izquierdo (conector blanco): GRIS -> GPIO 34, NEGRO -> GND,
 *     4,7 kOhm entre GPIO 34 y 3,3 V, 100 nF entre GPIO 34 y GND.
 *     Reposo 8,78k | RES+ 5,38k (1) | SET- 3,64k (2) | Crucero 2,59k (3) | Cancelar 1,40k (4)
 *   Derecho (conector negro): VIOLETA -> GPIO 35, NEGRO -> GND,
 *     2,2 kOhm entre GPIO 35 y 3,3 V, 100 nF entre GPIO 35 y GND.
 *     Reposo abierto | Vol+ 0,31k (5) | Vol- 0,63k (6) | SRC arriba 1,01k (7)
 *     | SRC abajo 1,50k (8) | SRC apretado 2,31k (9) | Telefono 3,82k (10) | Silencio 8,88k (11)
 *   Luz de las teclas: VERDE (+) y MARRON (-), no pasa por el ESP32.
 *   Una lectura cuenta solo si queda a +-12 % del valor de la tecla y estable
 *   30 ms: los valores intermedios (tecla apretada a medias) se ignoran.
 *
 * Botones DIGITALES (etapa 1, siguen andando): 1 = GPIO 25, 2 = GPIO 26,
 *   3 = GPIO 27, 4 = GPIO 14; "apretar" = unir el pin con GND.
 *
 * Toque o mantener: si se suelta antes de 0,6 s se manda "toque" al soltar;
 * si se mantiene 0,6 s se manda "mantener" en ese momento (sin esperar a
 * soltar). Cada uno hace su propia accion en Auto Hub. Volumen + y - siguen
 * mandando "repetir" cada 0,2 s mientras se mantienen.
 *
 * Mensaje (caracteristica de botones): [numero, tipo]. Tipo 1 = toque,
 * 2 = mantener, 3 = repetir (mantenido), 4 = aviso del comando (numero 1 =
 * izquierdo desconectado, 2 = izquierdo conectado de nuevo).
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

#define FIRMWARE_VERSION "1.5.1"

const int BUTTON_PINS[] = {25, 26, 27, 14};
const int BUTTON_COUNT = sizeof(BUTTON_PINS) / sizeof(BUTTON_PINS[0]);
const int LED_PIN = 2;
const unsigned long DEBOUNCE_MS = 30;
const unsigned long LONG_PRESS_MS = 600;  // mantener apretado este tiempo = "mantener"

// Tipo de pulsacion que se manda (segundo byte del mensaje).
const uint8_t PRESS_TAP = 1;
const uint8_t PRESS_HOLD = 2;
const uint8_t PRESS_REPEAT = 3;
const uint8_t PAD_STATUS = 4;

// ---- Comandos del volante (escaleras de resistencias) ----

struct LadderKey {
  float ohms;      // resistencia medida de la tecla
  uint8_t number;  // boton que se manda a Auto Hub
};

struct Ladder {
  const char* name;
  int pin;
  float pullUp;          // resistencia entre el pin y 3,3 V
  float restOhms;        // reposo (0 = abierto)
  const LadderKey* keys;
  int keyCount;
  // Estado (rebote, toque / mantener / repetir).
  uint8_t candidate;
  unsigned long candidateSince;
  uint8_t stable;
  unsigned long pressedAt;
  bool holdSent;
  unsigned long lastRepeat;
  bool open;
  unsigned long openSince;
  bool reportedOpen;
};

const LadderKey LEFT_KEYS[] = {{5380, 1}, {3641, 2}, {2594, 3}, {1397, 4}};
const LadderKey RIGHT_KEYS[] = {{310, 5}, {631, 6}, {1006, 7}, {1503, 8}, {2310, 9}, {3820, 10}, {8880, 11}};

Ladder ladders[] = {
  {"izquierdo", 34, 4700, 8780, LEFT_KEYS, 4},
  {"derecho", 35, 2200, 0, RIGHT_KEYS, 7},
};
const int LADDER_COUNT = sizeof(ladders) / sizeof(ladders[0]);

const float KEY_TOLERANCE = 0.12;            // +-12 % del valor de la tecla
const uint32_t OPEN_MV = 2950;               // mas que esto = nada conectado / nada apretado (el ADC no llega a 3,3 V)
const unsigned long STUCK_MS = 300;          // valor intermedio tanto tiempo con una tecla apretada = se solto
const unsigned long BOOT_IGNORE_MS = 800;    // al arrancar se ignoran las lecturas (cargan los capacitores)
bool diagnostics = true;                     // muestra en el Monitor serie lo que lee cada comando
const unsigned long LADDER_STABLE_MS = 30;   // lectura estable antes de aceptarla
const unsigned long REPEAT_MS = 200;         // volumen mantenido: cada 0,2 s
const unsigned long OPEN_REPORT_MS = 2000;   // comando izquierdo sin senal tanto tiempo = desconectado

bool repeats(uint8_t number) { return number == 5 || number == 6; }

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
      otaAbort("pedido de Auto Hub");
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
    Serial.println("Auto Hub conectado (Auto Hub Volante v" FIRMWARE_VERSION ")");
  }
  void onDisconnect(BLEServer* server) override {
    connected = false;
    if (otaActive) otaAbort("se corto la conexion");
    Serial.println("Auto Hub desconectado, esperando de nuevo...");
    delay(200);
    BLEDevice::startAdvertising();  // vuelve a quedar visible
  }
};

void sendButton(int number, uint8_t kind) {
  // Mensaje: [numero de boton (1..11), 1 = toque / 2 = mantener / 3 = repetir]
  uint8_t message[2] = {(uint8_t)number, kind};
  buttonCharacteristic->setValue(message, 2);
  if (connected) {
    buttonCharacteristic->notify();
  }
  Serial.printf("Boton %d %s %s\n", number,
                kind == PRESS_HOLD ? "MANTENER" : kind == PRESS_REPEAT ? "repetir" : "toque",
                connected ? "enviado" : "(sin conexion)");
  if (kind == PRESS_REPEAT) return;  // sin parpadeo (no frena la repeticion)
  digitalWrite(LED_PIN, LOW);
  delay(80);
  digitalWrite(LED_PIN, connected ? HIGH : LOW);
}

void sendPadStatus(uint8_t number) {
  uint8_t message[2] = {number, PAD_STATUS};
  buttonCharacteristic->setValue(message, 2);
  if (connected) buttonCharacteristic->notify();
}

uint32_t lastMv[2] = {0, 0};
unsigned long midSince[2] = {0, 0};  // desde cuando se lee un valor intermedio

/** Resistencia que ve el pin (ohms), o -1 si esta abierto (sin nada conectado). */
float readOhms(const Ladder& l, uint32_t& mvOut) {
  uint32_t mv = 0;
  for (int i = 0; i < 8; i++) mv += analogReadMilliVolts(l.pin);
  mv /= 8;
  mvOut = mv;
  if (mv >= OPEN_MV) return -1;  // cerca de 3,3 V: abierto (el ADC del ESP32 llega a ~3,1 V)
  return l.pullUp * mv / (3300.0 - mv);
}

/** Diagnostico: lo que lee el comando, cuando cambia (para ajustar los valores de las teclas). */
void logReading(const Ladder& l, int index, uint32_t mv, float ohms, uint8_t key) {
  if (!diagnostics) return;
  uint32_t prev = lastMv[index];
  if ((mv > prev ? mv - prev : prev - mv) < 60) return;  // cambio chico: no se muestra
  lastMv[index] = mv;
  if (ohms < 0) {
    Serial.printf("  [%s] %u mV = abierto (nada apretado)\n", l.name, (unsigned)mv);
  } else if (key == 255) {
    Serial.printf("  [%s] %u mV = %.2f kOhm -> valor intermedio (se ignora)\n", l.name, (unsigned)mv, ohms / 1000.0);
  } else if (key == 0) {
    Serial.printf("  [%s] %u mV = %.2f kOhm -> reposo\n", l.name, (unsigned)mv, ohms / 1000.0);
  } else {
    Serial.printf("  [%s] %u mV = %.2f kOhm -> boton %d\n", l.name, (unsigned)mv, ohms / 1000.0, key);
  }
}

/** Tecla que corresponde a esa resistencia: numero, 0 = ninguna, 255 = valor intermedio (se ignora). */
uint8_t matchKey(const Ladder& l, float ohms) {
  if (ohms < 0) return 0;
  // Comando sin resistencia de reposo (derecho): muy por encima de la tecla mas alta = nada apretado.
  if (l.restOhms == 0) {
    float highest = 0;
    for (int i = 0; i < l.keyCount; i++) highest = max(highest, l.keys[i].ohms);
    if (ohms > highest * 1.5) return 0;
  }
  for (int i = 0; i < l.keyCount; i++) {
    float k = l.keys[i].ohms;
    if (ohms > k * (1 - KEY_TOLERANCE) && ohms < k * (1 + KEY_TOLERANCE)) return l.keys[i].number;
  }
  if (l.restOhms > 0 && ohms > l.restOhms * (1 - KEY_TOLERANCE) && ohms < l.restOhms * (1 + KEY_TOLERANCE)) return 0;
  return 255;
}

void pollLadder(Ladder& l, int index, unsigned long now) {
  uint32_t mv = 0;
  float ohms = readOhms(l, mv);
  if (now < BOOT_IGNORE_MS) return;
  // Comando izquierdo: en reposo marca 8,78k; abierto = cable suelto.
  if (l.restOhms > 0) {
    bool isOpen = ohms < 0;
    if (isOpen && !l.open) l.openSince = now;
    l.open = isOpen;
    if (isOpen && !l.reportedOpen && now - l.openSince > OPEN_REPORT_MS) {
      l.reportedOpen = true;
      Serial.printf("Comando %s desconectado (revisar el cable de senal)\n", l.name);
      sendPadStatus(1);
    } else if (!isOpen && l.reportedOpen) {
      l.reportedOpen = false;
      Serial.printf("Comando %s conectado de nuevo\n", l.name);
      sendPadStatus(2);
    }
  }
  uint8_t key = matchKey(l, ohms);
  logReading(l, index, mv, ohms, key);
  if (key == 255) {
    // Valor intermedio: no cambia nada, salvo que dure (la tecla ya se solto).
    if (midSince[index] == 0) midSince[index] = now;
    if (l.stable != 0 && now - midSince[index] > STUCK_MS) key = 0;
    else return;
  } else {
    midSince[index] = 0;
  }
  if (key != l.candidate) {
    l.candidate = key;
    l.candidateSince = now;
  }
  if (now - l.candidateSince >= LADDER_STABLE_MS && l.candidate != l.stable) {
    // Se solto (o se paso directo a otra tecla): toque si no llego a mantener.
    if (l.stable != 0 && !l.holdSent) sendButton(l.stable, PRESS_TAP);
    l.stable = l.candidate;
    if (l.stable != 0) {
      l.pressedAt = now;
      l.holdSent = false;
    }
  }
  if (l.stable != 0 && !l.holdSent && now - l.pressedAt >= LONG_PRESS_MS) {
    l.holdSent = true;
    l.lastRepeat = now;
    sendButton(l.stable, PRESS_HOLD);
  } else if (l.stable != 0 && l.holdSent && repeats(l.stable) && now - l.lastRepeat >= REPEAT_MS) {
    l.lastRepeat = now;
    sendButton(l.stable, PRESS_REPEAT);
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  for (int i = 0; i < LADDER_COUNT; i++) {
    pinMode(ladders[i].pin, INPUT);
    analogSetPinAttenuation(ladders[i].pin, ADC_11db);  // hasta ~3,1 V
    ladders[i].candidate = 0;
    ladders[i].candidateSince = 0;
    ladders[i].stable = 0;
    ladders[i].pressedAt = 0;
    ladders[i].holdSent = false;
    ladders[i].lastRepeat = 0;
    ladders[i].open = false;
    ladders[i].openSince = 0;
    ladders[i].reportedOpen = false;
  }
  for (int i = 0; i < BUTTON_COUNT; i++) {
    pinMode(BUTTON_PINS[i], INPUT_PULLUP);
    stablePressed[i] = false;
    lastReading[i] = false;
    lastChange[i] = 0;
    pressedAt[i] = 0;
    holdSent[i] = false;
  }

  BLEDevice::init("Auto Hub Volante");  // nombre Bluetooth (desde 1.4.0; antes "Celta Volante")
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

  Serial.println("Auto Hub Volante v" FIRMWARE_VERSION " listo. Esperando a Auto Hub...");
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

  // Comandos del volante (no durante una actualizacion: el Bluetooth va primero).
  if (!otaActive) {
    for (int i = 0; i < LADDER_COUNT; i++) pollLadder(ladders[i], i, now);
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
