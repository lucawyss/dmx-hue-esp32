/*
 * ============================================================
 *  ArtNet → Philips Hue Bridge  (ampoules Blanc Ambiance)
 *  Plateformes supportées :
 *    - ESP32, ESP32-S2, ESP32-S3, ESP32-C3, ESP32-C6
 *    - ESP32-C5  (nécessite le core Arduino-ESP32 dev ≥ 3.x)
 *    - ESP8266   (Wemos D1 Mini, NodeMCU…)
 *
 *  ► INSTALLATION DU CORE POUR ESP32-C5
 *    Dans l'IDE Arduino → Préférences → URL de gestionnaire :
 *    https://espressif.github.io/arduino-esp32/package_esp32_dev_index.json
 *    Puis : Outils → Type de carte → Gestionnaire de cartes
 *    Chercher "esp32" par Espressif → installer la version dev (≥ 3.x)
 *    Sélectionner la carte : "ESP32C5 Dev Module"
 *
 *  Mapping DMX automatique (2 canaux par ampoule) :
 *    Ch N+0 : Intensité   (0 = éteint, 1-255 → bri 1-254)
 *    Ch N+1 : Température (0 = froid CT_COLD_MIRED,
 *                          255 = chaud CT_WARM_MIRED)
 *
 *  Dépendances :
 *    - ArtNet     par hideakitai  → "ArtNet hideakitai"
 *    - ArduinoJson                → "ArduinoJson"
 *    - Core ESP32 dev ou ESP8266
 * ============================================================
 */

// ═══════════════════════════════════════════════════════════════
//  Détection de plateforme
//  L'ESP32-C5 est détecté comme ESP32 (CONFIG_IDF_TARGET_ESP32C5).
//  On unifie tous les ESP32 sous un seul bloc.
// ═══════════════════════════════════════════════════════════════
#if defined(ESP8266)
  // ── ESP8266 ─────────────────────────────────────────────────
  #include <ESP8266WiFi.h>
  #include <ESP8266HTTPClient.h>
  #include <WiFiClient.h>
  #define PLATFORM_NAME "ESP8266"

#elif defined(ESP32)
  // ── Toute la famille ESP32 (dont C5, C6, S3, etc.) ──────────
  #include <WiFi.h>
  #include <HTTPClient.h>
  #include <WiFiClient.h>

  // Identification précise pour le port série
  #if defined(CONFIG_IDF_TARGET_ESP32C5)
    #define PLATFORM_NAME "ESP32-C5"
  #elif defined(CONFIG_IDF_TARGET_ESP32C6)
    #define PLATFORM_NAME "ESP32-C6"
  #elif defined(CONFIG_IDF_TARGET_ESP32S3)
    #define PLATFORM_NAME "ESP32-S3"
  #elif defined(CONFIG_IDF_TARGET_ESP32S2)
    #define PLATFORM_NAME "ESP32-S2"
  #elif defined(CONFIG_IDF_TARGET_ESP32C3)
    #define PLATFORM_NAME "ESP32-C3"
  #else
    #define PLATFORM_NAME "ESP32"
  #endif

#else
  #error "Plateforme non supportée. Utilisez un ESP32 (dont C5) ou un ESP8266."
#endif

#include <ArtnetWiFi.h>
#include <ArduinoJson.h>

// ═══════════════════════════════════════════════════════════════
//  ► CONFIGURATION — À ADAPTER À VOTRE INSTALLATION
// ═══════════════════════════════════════════════════════════════

// Wi-Fi
const char* WIFI_SSID     = "WIFI_NAME";
const char* WIFI_PASSWORD = "PASSWORD";

// IP statique de l'ESP
IPAddress STATIC_IP (10, 10, 10, 3);   // IP fixe de l'ESP
IPAddress GATEWAY (10, 10, 10, 1);      // Passerelle (box/routeur)
IPAddress SUBNET (255, 0, 0, 0);        // Masque de sous-réseau
IPAddress DNS_PRIMARY (10, 10, 10, 1);   // DNS primaire
IPAddress DNS_SECOND ( 8,   8,   4,   4);   // DNS secondaire

// Pont Philips Hue
const char* HUE_BRIDGE_IP = "10.10.10.2";   // IP de votre pont Hue
const char* HUE_API_KEY   = "HUE_BRIDGE_API_KEY";  // Clé obtenue via /api

// Univers ArtNet à écouter
const uint16_t ARTNET_UNIVERSE = 0;

// Adresse DMX de départ (0-based, ex: 0 = DMX 1)
const uint16_t DMX_START_ADDRESS = 0;

// Plage de température en Mired
//   Hue White Ambiance : 153 (6500 K) → 500 (2000 K)
//   Hue White simple   : 370 (2700 K) → 500 (2000 K)
const uint16_t CT_COLD_MIRED = 153;
const uint16_t CT_WARM_MIRED = 500;

// Throttle HTTP par ampoule (ms) — 50 ms ≈ 20 req/s, dans la limite Hue
const uint16_t HUE_MIN_INTERVAL_MS = 50;

// Nombre maximum d'ampoules supportées
#define MAX_LIGHTS 64

// ═══════════════════════════════════════════════════════════════
//  Structure ampoule
// ═══════════════════════════════════════════════════════════════
struct HueLight {
  char     lightId[8];
  uint16_t dmxStart;
};

// ═══════════════════════════════════════════════════════════════
//  Variables globales
// ═══════════════════════════════════════════════════════════════
HueLight lights[MAX_LIGHTS];
uint8_t  numLights = 0;

// Dernier état DMX reçu (mis à jour dans le callback, lu dans loop)
volatile uint8_t dmxDimmer[MAX_LIGHTS];
volatile uint8_t dmxCt    [MAX_LIGHTS];
volatile bool    dmxDirty [MAX_LIGHTS];

// Dernier état envoyé (pour éviter les doublons)
uint8_t sentDimmer[MAX_LIGHTS];
uint8_t sentCt    [MAX_LIGHTS];

// Timestamp dernier envoi HTTP par ampoule
unsigned long lastSentMs[MAX_LIGHTS] = {0};

// Connexions TCP persistantes (keep-alive), une par ampoule
WiFiClient hueClients[MAX_LIGHTS];

ArtnetWiFiReceiver artnet;

// ═══════════════════════════════════════════════════════════════
//  Prototypes
// ═══════════════════════════════════════════════════════════════
bool discoverLights();
void onArtDmx(const uint8_t* data, uint16_t size,
              const ArtDmxMetadata& meta, const ArtNetRemoteInfo& remote);
bool sendHueCommand(uint8_t idx, uint8_t dimmer, uint8_t ctDmx);

// ═══════════════════════════════════════════════════════════════
//  SETUP
// ═══════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  delay(500);   // Laisse le temps au port série de s'initialiser
  Serial.printf("\n\n=== ArtNet → Philips Hue Bridge [%s] ===\n",
                PLATFORM_NAME);

  // ── Wi-Fi IP statique ────────────────────────────────────────
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);   // Efface toute connexion précédente en mémoire
  delay(100);

  if (!WiFi.config(STATIC_IP, GATEWAY, SUBNET, DNS_PRIMARY, DNS_SECOND))
    Serial.println("[WARN] IP statique echouee — DHCP en fallback");

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connexion Wi-Fi");
  uint8_t attempts = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print('.');
    if (++attempts > 40) {
      Serial.println("\n[ERREUR] Impossible de se connecter au Wi-Fi. Redemarrage...");
      delay(1000);
      ESP.restart();
    }
  }
  Serial.println();
  Serial.printf("IP        : %s\n", WiFi.localIP().toString().c_str());
  Serial.printf("Passerelle: %s\n", WiFi.gatewayIP().toString().c_str());
  Serial.printf("Masque    : %s\n", WiFi.subnetMask().toString().c_str());
  Serial.printf("RSSI      : %d dBm\n", WiFi.RSSI());

  // ── Découverte automatique des ampoules ──────────────────────
  Serial.println("\nDecouverte des ampoules Hue...");
  while (!discoverLights()) {
    Serial.println("Echec — nouvel essai dans 5 s...");
    delay(5000);
  }

  // ── Init états ───────────────────────────────────────────────
  memset((void*)dmxDimmer, 0, sizeof(dmxDimmer));
  memset((void*)dmxCt,     0, sizeof(dmxCt));
  memset((void*)dmxDirty,  0, sizeof(dmxDirty));
  memset(sentDimmer,       0, sizeof(sentDimmer));
  memset(sentCt,           0, sizeof(sentCt));

  // ── ArtNet ───────────────────────────────────────────────────
  artnet.begin();
  artnet.subscribeArtDmxUniverse(ARTNET_UNIVERSE, onArtDmx);
  Serial.printf("Ecoute ArtNet UDP 6454, univers %d\n\n", ARTNET_UNIVERSE);
}

// ═══════════════════════════════════════════════════════════════
//  LOOP — Batch dispatch
// ═══════════════════════════════════════════════════════════════
void loop() {
  // Reconnexion Wi-Fi si nécessaire
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Wi-Fi perdu, reconnexion...");
    WiFi.reconnect();
    uint8_t attempts = 0;
    while (WiFi.status() != WL_CONNECTED && ++attempts < 20)
      delay(500);
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("Reconnexion echouee, redemarrage...");
      ESP.restart();
    }
    return;
  }

  // 1. Vider la file UDP
  artnet.parse();

  // 2. Envoyer les commandes Hue pour les ampoules modifiées
  unsigned long now = millis();
  for (uint8_t i = 0; i < numLights; i++) {
    if (!dmxDirty[i]) continue;
    if ((now - lastSentMs[i]) < HUE_MIN_INTERVAL_MS) continue;

    uint8_t dim = dmxDimmer[i];
    uint8_t ct  = dmxCt[i];

    dmxDirty[i] = false;   // Reset avant envoi pour ne pas rater une màj

    if (sendHueCommand(i, dim, ct)) {
      sentDimmer[i] = dim;
      sentCt[i]     = ct;
      lastSentMs[i] = millis();
    } else {
      dmxDirty[i] = true;  // Échec → réessayer au prochain cycle
    }
  }
}

// ═══════════════════════════════════════════════════════════════
//  Callback ArtNet
// ═══════════════════════════════════════════════════════════════
void onArtDmx(const uint8_t* data, uint16_t size,
              const ArtDmxMetadata& /*meta*/,
              const ArtNetRemoteInfo& /*remote*/) {

  for (uint8_t i = 0; i < numLights; i++) {
    uint16_t base = lights[i].dmxStart;
    if (base + 1 >= size) continue;

    uint8_t dim = data[base + 0];
    uint8_t ct  = data[base + 1];

    if (dim != sentDimmer[i] || ct != sentCt[i]) {
      dmxDimmer[i] = dim;
      dmxCt[i]     = ct;
      dmxDirty[i]  = true;
    }
  }
}

// ═══════════════════════════════════════════════════════════════
//  Envoi HTTP PUT avec keep-alive
// ═══════════════════════════════════════════════════════════════
bool sendHueCommand(uint8_t idx, uint8_t dimmer, uint8_t ctDmx) {
  HTTPClient http;

  char url[128];
  snprintf(url, sizeof(url),
           "http://%s/api/%s/lights/%s/state",
           HUE_BRIDGE_IP, HUE_API_KEY, lights[idx].lightId);

  http.begin(hueClients[idx], url);
  http.addHeader("Content-Type", "application/json");
  http.setReuse(true);

  StaticJsonDocument<96> doc;
  if (dimmer == 0) {
    doc["on"] = false;
  } else {
    doc["on"]             = true;
    doc["bri"]            = (uint8_t)map(dimmer, 1, 255, 1, 254);
    doc["ct"]             = (uint16_t)map((long)ctDmx, 0, 255,
                                          CT_COLD_MIRED, CT_WARM_MIRED);
    doc["transitiontime"] = 0;
  }

  String payload;
  serializeJson(doc, payload);

  int httpCode = http.PUT(payload);
  bool ok = (httpCode == HTTP_CODE_OK);

  if (ok) {
    uint16_t ctMired = (uint16_t)map((long)ctDmx, 0, 255,
                                     CT_COLD_MIRED, CT_WARM_MIRED);
    Serial.printf("[OK] Lampe %-3s bri=%3d ct=%4dMired (~%4dK)\n",
                  lights[idx].lightId, dimmer, ctMired,
                  ctMired > 0 ? (uint16_t)(1000000UL / ctMired) : 0);
  } else {
    Serial.printf("[ERR] Lampe %s HTTP %d : %s\n",
                  lights[idx].lightId, httpCode,
                  http.getString().c_str());
  }

  return ok;
}

// ═══════════════════════════════════════════════════════════════
//  Découverte automatique des ampoules via GET /api/<key>/lights
// ═══════════════════════════════════════════════════════════════
bool discoverLights() {
  WiFiClient client;
  HTTPClient http;

  char url[128];
  snprintf(url, sizeof(url), "http://%s/api/%s/lights",
           HUE_BRIDGE_IP, HUE_API_KEY);

  http.begin(client, url);
  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("Erreur HTTP decouverte : %d\n", httpCode);
    http.end();
    return false;
  }

  String body = http.getString();
  http.end();

  DynamicJsonDocument filter(64);
  filter["*"]["name"] = true;

  DynamicJsonDocument doc(4096);
  DeserializationError err = deserializeJson(doc, body,
                               DeserializationOption::Filter(filter));
  if (err) {
    Serial.print("Erreur JSON : "); Serial.println(err.c_str());
    return false;
  }

  uint16_t ids[MAX_LIGHTS];
  uint8_t  count = 0;
  for (JsonPair kv : doc.as<JsonObject>()) {
    if (count >= MAX_LIGHTS) break;
    ids[count++] = (uint16_t)atoi(kv.key().c_str());
  }
  if (count == 0) {
    Serial.println("Aucune ampoule trouvee.");
    return false;
  }

  // Tri par ID croissant
  for (uint8_t i = 0; i < count - 1; i++)
    for (uint8_t j = 0; j < count - 1 - i; j++)
      if (ids[j] > ids[j+1]) {
        uint16_t t = ids[j]; ids[j] = ids[j+1]; ids[j+1] = t;
      }

  numLights = count;
  uint16_t cursor = DMX_START_ADDRESS;

  Serial.println("+--------+------------------------------------------+");
  Serial.println("|  ID    |  Dim (DMX)    CT (DMX)    Nom            |");
  Serial.println("+--------+------------------------------------------+");
  for (uint8_t i = 0; i < numLights; i++) {
    snprintf(lights[i].lightId, sizeof(lights[i].lightId), "%d", ids[i]);
    lights[i].dmxStart = cursor;
    const char* name = doc[lights[i].lightId]["name"] | "?";
    Serial.printf("|  %-5s  |  DMX %3d      DMX %3d     %-14s|\n",
                  lights[i].lightId, cursor + 1, cursor + 2, name);
    cursor += 2;
  }
  Serial.println("+--------+------------------------------------------+");
  Serial.printf("%d ampoule(s), canaux DMX 1 a %d\n\n",
                numLights, numLights * 2);
  return true;
}

// ═══════════════════════════════════════════════════════════════
//  README / GUIDE DE DÉMARRAGE RAPIDE
// ═══════════════════════════════════════════════════════════════
/*
 * ┌─────────────────────────────────────────────────────────────┐
 * │  1. INSTALLER LES BIBLIOTHÈQUES                             │
 * │     Outils → Gérer les bibliothèques :                      │
 * │     • "ArtNet" par hideakitai                               │
 * │     • "ArduinoJson" par Benoit Blanchon                     │
 * │                                                             │
 * │  2. OBTENIR UNE CLÉ API HUE                                 │
 * │     dans Chrome, allez à http://<IP_PONT>/debug/clip.html   │
 * │     Appuyez sur le bouton du pont, puis :                   │
 * │     POST http://<IP_PONT>/api                               │
 * │     Body : {"devicetype":"artnet_bridge#esp"}               │
 * │     La réponse contient votre "username" (= clé API).       │
 * │                                                             │
 * │  3. DÉCOUVERTE AUTOMATIQUE                                  │
 * │     Au boot, le script interroge GET /api/<key>/lights,     │
 * │     trie les ampoules par ID croissant et assigne           │
 * │     2 canaux DMX consécutifs à chacune.                     │
 * │     Le tableau est affiché sur le port série.               │
 * │                                                             │
 * │  4. MAPPING DMX (2 canaux par ampoule)                      │
 * │     Canal N+0 : Dimmer      0 = off, 1-255 = bri 1-254      │
 * │     Canal N+1 : Température 0 = froid → 255 = chaud         │
 * │                                                             │
 * │  5. PLAGE DE TEMPÉRATURE                                    │
 * │     Hue White Ambiance : CT_COLD=153, CT_WARM=500 Mired     │
 * │     Hue White (simple) : CT_COLD=370, CT_WARM=500 Mired     │
 * │                                                             │
 * │  6. LIMITES DE L'API HUE                                    │
 * │     ~10 requêtes/sec max → HUE_MIN_INTERVAL_MS = 100 ms     │
 * └─────────────────────────────────────────────────────────────┘
 */

