
#include <WiFi.h>
#include <PubSubClient.h>
#include <math.h>

// ==================================================
// 1. Wi-Fi Configuration
// ==================================================

const char* WIFI_SSID     = "Rajib Hasan";
const char* WIFI_PASSWORD = "ra7282ju22";

// ==================================================
// 2. MQTT Configuration
// ==================================================

const char* MQTT_SERVER = "broker.hivemq.com";
const uint16_t MQTT_PORT = 1883;

const char* MQTT_TOPIC = "vehicle/accident";

const char* DEVICE_ID = "esp32-dummy-01";

// ==================================================
// 3. Objects
// ==================================================

WiFiClient espClient;
PubSubClient mqtt(espClient);

// ==================================================
// 4. Dummy GPS Route (Dhaka)
// ==================================================

struct GPSPoint {
  float lat;
  float lon;
};

GPSPoint route[] = {
  {23.8103, 90.4125},
  {23.7940, 90.4043},
  {23.7808, 90.4167},
  {23.7731, 90.3995},
  {23.7465, 90.3760},
  {23.7380, 90.3830},
  {23.7561, 90.3872},
  {23.7806, 90.3990},
  {23.7950, 90.4150},
  {23.8103, 90.4125}
};

const int ROUTE_SIZE = sizeof(route) / sizeof(route[0]);

int routeIndex = 0;
float routeProgress = 0.0;

// ==================================================
// 5. Timing
// ==================================================

unsigned long previousPublish = 0;

const unsigned long PUBLISH_INTERVAL = 1000;

// ==================================================
// 6. Dummy Sensor Variables
// ==================================================

float temperature = 29.0;

float ax = 0.0;
float ay = 0.0;
float az = 1.0;

float gx = 0.0;
float gy = 0.0;
float gz = 0.0;

float vibration = 0.10;

float anomalyScore = 0.02;
float threshold = 0.12;

float speedKmh = 30.0;

// ==================================================
// 7. Wi-Fi Connection
// ==================================================

void connectWiFi() {

  if (WiFi.status() == WL_CONNECTED) {
    return;
  }

  Serial.println();
  Serial.print("Connecting to Wi-Fi");

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  while (WiFi.status() != WL_CONNECTED) {

    delay(500);
    Serial.print(".");
  }

  Serial.println();
  Serial.println("Wi-Fi connected!");

  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());
}

// ==================================================
// 8. MQTT Connection
// ==================================================

void connectMQTT() {

  while (!mqtt.connected()) {

    Serial.print("Connecting to MQTT...");

    String clientId = "ESP32-";
    clientId += String((uint32_t)ESP.getEfuseMac(), HEX);

    if (mqtt.connect(clientId.c_str())) {

      Serial.println("Connected!");

    } else {

      Serial.print("Failed. State: ");
      Serial.println(mqtt.state());

      delay(2000);
    }
  }
}

// ==================================================
// 9. Generate Dummy GPS
// ==================================================

void generateGPS(float &lat, float &lon) {

  GPSPoint start = route[routeIndex];

  int nextIndex = (routeIndex + 1) % ROUTE_SIZE;

  GPSPoint end = route[nextIndex];

  lat = start.lat +
        (end.lat - start.lat) * routeProgress;

  lon = start.lon +
        (end.lon - start.lon) * routeProgress;

  routeProgress += 0.05;

  if (routeProgress >= 1.0) {

    routeProgress = 0.0;

    routeIndex = nextIndex;
  }
}

// ==================================================
// 10. Generate Dummy Sensor Data
// ==================================================

void generateSensorData() {

  float t = millis() / 1000.0;

  // Temperature simulation
  temperature = 29.0 + sin(t * 0.10) * 2.0;

  // Accelerometer simulation
  ax = sin(t * 1.5) * 0.15;
  ay = cos(t * 1.2) * 0.15;
  az = 1.0 + sin(t * 0.8) * 0.05;

  // Gyroscope simulation
  gx = sin(t * 1.0) * 5.0;
  gy = cos(t * 1.3) * 5.0;
  gz = sin(t * 0.7) * 5.0;

  // Vibration simulation
  vibration = 0.10 + fabs(sin(t * 2.0)) * 0.10;

  // AI anomaly score simulation
  anomalyScore = 0.02 + fabs(sin(t * 0.5)) * 0.04;

  threshold = 0.12;

  // Vehicle speed simulation
  speedKmh = 30.0 + sin(t * 0.3) * 8.0;
}

// ==================================================
// 11. Publish Telemetry
// ==================================================

void publishTelemetry() {

  float latitude;
  float longitude;

  generateGPS(latitude, longitude);

  generateSensorData();

  // Keep payload compact for PubSubClient's
  // default 256-byte MQTT packet buffer.

  char payload[240];

  snprintf(
    payload,
    sizeof(payload),

    "{\"device_id\":\"esp32\",\"status\":\"normal\","
    "\"ax\":%.2f,\"ay\":%.2f,\"az\":%.2f,"
    "\"gx\":%.1f,\"gy\":%.1f,\"gz\":%.1f,"
    "\"temp_c\":%.1f,\"vibration\":%.2f,"
    "\"anomaly_score\":%.2f,\"threshold\":%.2f,"
    "\"speed_kmh\":%.1f,\"sats\":8,\"gps_fix\":true,"
    "\"lat\":%.5f,\"lon\":%.5f}",

    ax, ay, az,
    gx, gy, gz,
    temperature,
    vibration,
    anomalyScore,
    threshold,
    speedKmh,
    latitude,
    longitude
  );

  bool success = mqtt.publish(MQTT_TOPIC, payload);

  Serial.println("--------------------------------");

  Serial.print("MQTT Publish: ");
  Serial.println(success ? "SUCCESS" : "FAILED");

  Serial.print("Payload size: ");
  Serial.println(strlen(payload));

  Serial.print("Temperature: ");
  Serial.println(temperature);

  Serial.print("GPS: ");
  Serial.print(latitude, 5);
  Serial.print(", ");
  Serial.println(longitude, 5);

  Serial.print("Anomaly Score: ");
  Serial.println(anomalyScore);

  Serial.println(payload);
}

// ==================================================
// 12. Setup
// ==================================================

void setup() {

  Serial.begin(115200);

  delay(1000);

  Serial.println();
  Serial.println("================================");
  Serial.println(" Vehicle Accident Detection");
  Serial.println(" ESP32 Dummy Telemetry Publisher");
  Serial.println("================================");

  connectWiFi();

  // MQTT configuration
  mqtt.setServer(MQTT_SERVER, MQTT_PORT);

  connectMQTT();

  Serial.println("System ready!");
}

// ==================================================
// 13. Main Loop
// ==================================================

void loop() {

  // Maintain Wi-Fi connection
  if (WiFi.status() != WL_CONNECTED) {

    connectWiFi();
  }

  // Maintain MQTT connection
  if (!mqtt.connected()) {

    connectMQTT();
  }

  mqtt.loop();

  // Publish every 1 second
  unsigned long currentMillis = millis();

  if (currentMillis - previousPublish >= PUBLISH_INTERVAL) {

    previousPublish = currentMillis;

    publishTelemetry();
  }
}
