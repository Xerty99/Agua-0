/*
  ============================================================
  RECEPTOR ESP8266 - Sensor de Humedad -> Supabase
  ============================================================
  Función:
   1. Se conecta al WiFi del router (necesario para hablar con Supabase).
   2. Descubre en qué canal quedó esa conexión.
   3. Anuncia ese canal por ESP-NOW (broadcast) para que el ESP32-S3
      emisor pueda sincronizarse automáticamente, sin configurar nada
      a mano y sin importar si el router cambia de canal con el tiempo.
   4. Recibe las lecturas de humedad por ESP-NOW.
   5. Sube cada lectura a Supabase por HTTPS.

  IMPORTANTE: este código y el del ESP32-S3 forman un par. Deben
  subirse juntos; uno no funciona sin la lógica correspondiente del otro.
  ============================================================
*/

#include <ESP8266WiFi.h>
#include <espnow.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>

// ============================================================
// CONFIGURACIÓN - EDITA AQUÍ
// ============================================================
const char* ssid     = "Awita";
const char* password = "jamoel10";

const int ID_SECTOR = 0; // Cambiar si manejas múltiples sectores

const char* supabase_url = "https://ippzliasjmyhiulqbsgf.supabase.co/rest/v1/lecturas";
const char* apiKey = "sb_publishable_4XfTiCH3K866tq1PtQ5eqg_WxK-L_fy";

// MAC del ESP32-S3 emisor. OBLIGATORIO completarla para poder
// responderle directamente (unicast) en vez de solo broadcast.
// Súbele primero el código del 32-S3, lee su MAC por Serial, y ponla aquí.
uint8_t macESP32S3[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00}; // <-- COMPLETAR

// ============================================================
// PROTOCOLO INTERNO (debe coincidir EXACTO con el del 32-S3)
// ============================================================
typedef enum : uint8_t {
  MSG_ANUNCIO_CANAL = 0xA1,  // 8266 -> 32-S3 : "este es mi canal"
  MSG_LECTURA       = 0xA2   // 32-S3 -> 8266 : datos de sensor
} TipoMensaje;

typedef struct __attribute__((packed)) {
  uint8_t tipo;       // = MSG_ANUNCIO_CANAL
  uint8_t canal;      // canal WiFi real del 8266 (1-13)
} PaqueteAnuncioCanal;

typedef struct __attribute__((packed)) {
  uint8_t tipo;        // = MSG_LECTURA
  int valorSensor;
  char nivel[12];
} PaqueteLectura;

// Broadcast: dirección especial que reciben todos los dispositivos ESP-NOW
uint8_t macBroadcast[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// ============================================================
// VARIABLES DE ESTADO
// ============================================================
volatile bool datosPendientes = false;
PaqueteLectura lecturaRecibida;

unsigned long ultimoAnuncio = 0;
const unsigned long intervaloAnuncio = 2000; // re-anunciar cada 2s

bool peerEmisorRegistrado = false;

// ============================================================
// ESP-NOW: callback de recepción
// ============================================================
void onDataRecv(uint8_t *mac, uint8_t *incomingData, uint8_t len) {
  if (len < 1) return;

  uint8_t tipo = incomingData[0];

  if (tipo == MSG_LECTURA && len == sizeof(PaqueteLectura)) {
    memcpy(&lecturaRecibida, incomingData, sizeof(PaqueteLectura));
    datosPendientes = true;
  }
  // Cualquier otro tipo de mensaje recibido aquí se ignora:
  // el 8266 no espera anuncios de canal de nadie (es él quien los manda).
}

void onDataSent(uint8_t *mac_addr, uint8_t status) {
  // No se requiere acción; placeholder para diagnóstico si se necesita.
}

// ============================================================
// ESP-NOW: inicialización / reinicialización
// ============================================================
bool iniciarEspNow() {
  if (esp_now_init() != 0) {
    Serial.println("[ESP-NOW] Error al iniciar.");
    return false;
  }
  esp_now_set_self_role(ESP_NOW_ROLE_COMBO); // envía Y recibe
  esp_now_register_recv_cb(onDataRecv);
  esp_now_register_send_cb(onDataSent);

  // Peer de broadcast, para poder mandar el anuncio de canal a cualquiera
  esp_now_add_peer(macBroadcast, ESP_NOW_ROLE_COMBO, WiFi.channel(), NULL, 0);

  peerEmisorRegistrado = false;

  Serial.println("[ESP-NOW] Inicializado.");
  return true;
}

// Agrega al 32-S3 como peer unicast una vez que sabemos su MAC y canal
void registrarPeerEmisorSiHaceFalta() {
  bool macConfigurada = false;
  for (int i = 0; i < 6; i++) {
    if (macESP32S3[i] != 0x00) { macConfigurada = true; break; }
  }
  if (!macConfigurada) return; // el usuario no completó la MAC todavía

  if (!peerEmisorRegistrado) {
    esp_now_add_peer(macESP32S3, ESP_NOW_ROLE_COMBO, WiFi.channel(), NULL, 0);
    peerEmisorRegistrado = true;
    Serial.println("[ESP-NOW] Peer del ESP32-S3 registrado.");
  }
}

// Envía el anuncio de canal, tanto a broadcast como directo al 32-S3 si se conoce su MAC
void anunciarCanal() {
  PaqueteAnuncioCanal pkt;
  pkt.tipo = MSG_ANUNCIO_CANAL;
  pkt.canal = (uint8_t)WiFi.channel();

  esp_now_send(macBroadcast, (uint8_t*)&pkt, sizeof(pkt));

  bool macConfigurada = false;
  for (int i = 0; i < 6; i++) {
    if (macESP32S3[i] != 0x00) { macConfigurada = true; break; }
  }
  if (macConfigurada) {
    esp_now_send(macESP32S3, (uint8_t*)&pkt, sizeof(pkt));
  }
}

// ============================================================
// SETUP
// ============================================================
void setup() {
  Serial.begin(9600);
  Serial.println("\n\n=== Receptor ESP8266 iniciando ===");

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  Serial.print("Conectando a WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nConectado a WiFi.");
  Serial.print("MAC del ESP8266 (cópiala en el código del ESP32-S3): ");
  Serial.println(WiFi.macAddress());
  Serial.print("Canal WiFi real asignado por el router: ");
  Serial.println(WiFi.channel());

  iniciarEspNow();
  registrarPeerEmisorSiHaceFalta();

  Serial.println("Anunciando canal por ESP-NOW cada 2s para sincronizar al ESP32-S3...");
}

// ============================================================
// LOOP
// ============================================================
void loop() {
  unsigned long ahora = millis();

  // Reintentar registrar el peer del emisor en caso de que el usuario
  // haya dejado la MAC en ceros y la corrija recompilando luego.
  registrarPeerEmisorSiHaceFalta();

  // Anuncio periódico del canal (broadcast). Esto permite que el
  // ESP32-S3 se sincronice incluso si arranca después que el 8266,
  // o si el router cambió de canal y el 32-S3 quedó desincronizado.
  if (ahora - ultimoAnuncio >= intervaloAnuncio) {
    ultimoAnuncio = ahora;
    anunciarCanal();
  }

  // Procesar lectura recibida (fuera del callback, para evitar
  // problemas de watchdog / memoria durante el handshake SSL).
  if (datosPendientes) {
    datosPendientes = false;
    procesarYsubirLectura();
  }
}

// ============================================================
// SUBIDA A SUPABASE
// ============================================================
void procesarYsubirLectura() {
  Serial.print("Lectura recibida -> Valor: ");
  Serial.print(lecturaRecibida.valorSensor);
  Serial.print(" | Nivel: ");
  Serial.println(lecturaRecibida.nivel);

  int humedad_pct = map(lecturaRecibida.valorSensor, 0, 4095, 0, 100);
  humedad_pct = constrain(humedad_pct, 0, 100);

  // Liberar RAM crítica para el handshake TLS, desactivando ESP-NOW
  // temporalmente (igual que en la versión original).
  esp_now_unregister_recv_cb();
  esp_now_unregister_send_cb();
  esp_now_deinit();
  Serial.println("ESP-NOW pausado temporalmente para la subida HTTPS...");

  std::unique_ptr<BearSSL::WiFiClientSecure> client(new BearSSL::WiFiClientSecure);
  client->setInsecure();
  client->setBufferSizes(512, 512);

  HTTPClient https;
  if (https.begin(*client, supabase_url)) {
    https.addHeader("Content-Type", "application/json");
    https.addHeader("apikey", apiKey);
    https.addHeader("Authorization", String("Bearer ") + apiKey);
    https.addHeader("Prefer", "return=minimal");

    String jsonPayload = "{\"sector\":" + String(ID_SECTOR) +
                          ",\"humedad_pct\":" + String(humedad_pct) + "}";

    Serial.print("POST a Supabase: ");
    Serial.println(jsonPayload);

    int httpCode = https.POST(jsonPayload);

    if (httpCode > 0) {
      Serial.printf("[HTTPS] Código de respuesta: %d\n", httpCode);
      if (httpCode == 201) {
        Serial.println("Insertado correctamente en Supabase.");
      } else {
        // Mostrar el cuerpo de la respuesta ayuda mucho a diagnosticar
        // errores de RLS, columnas, o API key en Supabase.
        String resp = https.getString();
        Serial.print("Respuesta del servidor: ");
        Serial.println(resp);
      }
    } else {
      Serial.printf("[HTTPS] Falló el envío: %s\n", https.errorToString(httpCode).c_str());
    }
    https.end();
  } else {
    Serial.println("[HTTPS] No se pudo conectar al servidor.");
  }

  // Reactivar ESP-NOW para seguir escuchando al ESP32-S3
  iniciarEspNow();
  registrarPeerEmisorSiHaceFalta();
}
