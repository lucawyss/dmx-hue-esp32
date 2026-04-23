# dmx-hue-esp32
Art-Net node to control Philips Hue lights with DMX on a ESP32 controller

Inspired by [dmx-hue](https://github.com/sinedied/dmx-hue)

Dependancies :
  - [ArtNet by hideakitai](https://github.com/hideakitai/ArtNet)
  - [ArduinoJson](https://github.com/bblanchon/ArduinoJson)
  - [ESP32 Library for Arduino](https://github.com/espressif/arduino-esp32)

Supported microcontroller :
  - ESP32, ESP32-S2, ESP32-S3, ESP32-C3, ESP32-C6
  - ESP32-C5  (need Arduino-ESP32 dev ≥ 3.x)
  - ESP8266   (Wemos D1 Mini, NodeMCU…)

Supported bulbs :
  - Philips Hue Ambient White bulbs

1. GET A HUE API KEY
  In Chrome, go to http://<BRIDGE_IP>/debug/clip.html
  Click the bridge button, then:
  POST http://<BRIDGE_IP>/api
  Body: {"devicetype":"artnet_bridge#esp"}
  The response contains your "username" (= API key).

2. CONFIGURATION ON THE SCRIPT
   The WIFI setting and the BRIDGE settings are at the start of the script.

3. AUTOMATIC DISCOVERY
  At boot, the script queries GET /api/<key>/lights,
  sorts the bulbs by ascending ID, and assigns
  2 consecutive DMX channels to each.
  The table is displayed on the serial port.

4. DMX MAPPING (2 channels per bulb)
Channel N+0: Dimmer 0 = off, 1-255 = bright, 1-254 = on
Channel N+1: Temperature 0 = cool → 255 = warm

5. TEMPERATURE RANGE
Hue White Ambiance: CT_COLD=153, CT_WARM=500
Hue White (simple): CT_COLD=370, CT_WARM=500

6. HUE API LIMITS
~10 requests/sec max → HUE_MIN_INTERVAL_MS = 100 ms
