#include <WiFiManager.h> // https://github.com/tzapu/WiFiManager
#include <MQTT.h>
#include <time.h>
#include <Preferences.h>

#define MQTT_HOST "broker.emqx.io"
#define MQTT_PORT 1883

// LED blink states used by the blink task (separate WIFI and MQTT states)
enum BlinkState
{
  STATE_WIFI_CONNECTING,
  STATE_WIFI_CONNECTED,
  STATE_MQTT_CONNECTING,
  STATE_MQTT_CONNECTED,
  STATE_FAILED,
  STATE_MQTT_FAILED
};

typedef struct config
{
  uint32_t interval;
  uint32_t duration;
} config_t;

// shared state variable accessible from both setup() and the blink task
volatile BlinkState blinkState = STATE_WIFI_CONNECTING;

MQTTClient client;
unsigned long lastMillis = 0;
WiFiClient wifiClient;

// Relay pin (change if you use a different GPIO). Avoid using LED pin.
#define RELAY_PIN 5

// compile-time defaults used if no schedule is stored or MQTT config
#define DEFAULT_INTERVAL 3600 // one hour between activations
#define DEFAULT_DURATION 30   // relay on for 30 seconds
// default explicit turn-on epoch (user asked for 1772431200)
#define DEFAULT_TURN_ON_AT 1772431200UL

// configuration struct for scheduled on/off
typedef struct
{
  unsigned long interval;     // seconds until next ON
  unsigned long duration;     // seconds to stay ON
  unsigned long next_on_time; // epoch seconds for next ON (or relative seconds if time not available)
  unsigned long off_time;     // epoch seconds when to turn OFF
  bool is_on;
} system_config_t;

// a default instance that will be used on first boot or when prefs are empty
static const system_config_t DEFAULT_CONFIG = {DEFAULT_INTERVAL, DEFAULT_DURATION, DEFAULT_TURN_ON_AT, 0, false};

// current active configuration (starts with defaults)
system_config_t config = DEFAULT_CONFIG;

// NVS (Preferences) for persisting schedule
Preferences prefs;

static void saveSchedule()
{
  // store interval and duration as well so the schedule survives reboots
  prefs.putULong("interval", config.interval);
  prefs.putULong("duration", config.duration);
  // store next_on_time, off_time, and is_on
  prefs.putULong("next_on", config.next_on_time);
  prefs.putULong("off_time", config.off_time);
  prefs.putULong("is_on", config.is_on ? 1 : 0);
}

static void loadSchedule()
{
  // load interval/duration and fall back to defaults if not present
  config.interval = prefs.getULong("interval", DEFAULT_INTERVAL);
  config.duration = prefs.getULong("duration", DEFAULT_DURATION);
  // use DEFAULT_TURN_ON_AT if there is no stored value
  config.next_on_time = prefs.getULong("next_on", DEFAULT_TURN_ON_AT);
  config.off_time = prefs.getULong("off_time", 0);
  config.is_on = prefs.getULong("is_on", 0) ? true : false;
}

// Adjust schedule when system time is ahead of stored next_on_time.
// If the system missed the ON while it was offline (config.is_on == false),
// reschedule the next_on_time relative to now (don't retro-activate the relay).
static void adjustScheduleForMissedOn(unsigned long now)
{
  if (config.interval == 0 || config.next_on_time == 0)
    return;

  if (now > config.next_on_time)
  {
    Serial.print("Now is ahead of next_on_time (missed): ");
    Serial.println(config.next_on_time);
    if (!config.is_on)
    {
      // Missed the ON while system was off — align schedule to now
      config.next_on_time = now + config.interval;
      Serial.print("Rescheduled next ON to: ");
      Serial.println(config.next_on_time);
      saveSchedule();
    }
    else
    {
      // System was marked ON but time moved past off_time — ensure we turn it off
      if (config.off_time > 0 && now >= config.off_time)
      {
        digitalWrite(RELAY_PIN, LOW);
        config.is_on = false;
        config.off_time = 0;
        config.next_on_time = now + config.interval;
        Serial.println("Off time passed while active; turning OFF and rescheduling");
        saveSchedule();
      }
    }
  }
}

// simple parser to extract integer value for a key in a JSON-like string
static unsigned long parseNumber(const char *src, const char *key)
{
  const char *p = strstr(src, key);
  if (!p)
    return 0;
  p = strchr(p, ':');
  if (!p)
    return 0;
  p++; // move past ':'
  // skip whitespace
  while (*p && isspace((unsigned char)*p))
    p++;
  // parse unsigned long
  return strtoul(p, NULL, 10);
}

// return current time in seconds; if NTP/RTC not set yet then use uptime
static unsigned long getCurrentTime()
{
  time_t t = time(nullptr);
  if (t >= 100000)
  {
    return (unsigned long)t;
  }
  // no valid epoch, fall back to relative seconds since boot
  return millis() / 1000;
}

// forward declaration of blink task
void blinkTask(void *param);

// attempt a single MQTT connection; returns true on success
bool connectToMqtt()
{
  if (client.connected())
    return true;

  Serial.println("Connecting to MQTT...");
  // indicate MQTT connection attempt
  blinkState = STATE_MQTT_CONNECTING;

  if (client.connect("cardoz"))
  {
    Serial.println("connected!");
    blinkState = STATE_MQTT_CONNECTED;
    client.subscribe("/cardoz/config");
    client.subscribe("/cardoz/control");
    return true;
  }
  else
  {
    Serial.println("MQTT connect failed");
    blinkState = STATE_MQTT_FAILED;
    return false;
  }
}

void messageReceived(String &topic, String &payload)
{
  Serial.println("incoming: " + topic + " - " + payload);

  const char *cstr = payload.c_str();

  // handle config messages
  if (topic.equals("/cardoz/config"))
  {
    // parse simple JSON-like payload for interval and duration (seconds)
    // example payload: {"interval":3600,"duration":30}
    unsigned long interval = parseNumber(cstr, "interval");
    unsigned long duration = parseNumber(cstr, "duration");

    // check for explicit TURN_ON_AT epoch time
    // example: {"TURN_ON_AT":1708532400,"duration":30}
    unsigned long turn_on_at = parseNumber(cstr, "TURN_ON_AT");

    if (interval == 0 || duration == 0)
    {
      Serial.println("Invalid interval/duration in payload");
      return;
    }

    config.interval = interval;
    config.duration = duration;
    time_t now = time(nullptr);
    if (now < 100000)
    {
      Serial.println("System time not set yet; scheduling will start after time sync");
      // mark next_on_time as 0; loop() will initialize once time is available
      config.next_on_time = 0;
    }
    else
    {
      // use explicit TURN_ON_AT if provided; otherwise use now + interval
      if (turn_on_at > 0)
      {
        config.next_on_time = turn_on_at;
        Serial.print("Set explicit TURN_ON_AT: ");
        Serial.println(config.next_on_time);
      }
      else
      {
        // schedule first turn-on at now + interval
        config.next_on_time = (unsigned long)now + config.interval;
      }
      config.off_time = 0;
      config.is_on = false;
      Serial.print("Scheduled next ON at epoch: ");
      Serial.println(config.next_on_time);
      // persist schedule
      saveSchedule();
    }

    return;
  }

  // handle direct control messages
  if (topic.equals("/cardoz/control"))
  {
    // payload can be just ON/OFF or JSON like {"output":"ON"}
    char valbuf[16] = {0};
    const char *p = strstr(cstr, "output");
    if (!p)
    {
      // use payload directly
      strncpy(valbuf, cstr, sizeof(valbuf) - 1);
    }
    else
    {
      p = strchr(p, ':');
      if (p)
      {
        p++; // move past ':'
        while (*p && isspace((unsigned char)*p))
          p++;
        if (*p == '"' || *p == '\'')
          p++;
        int i = 0;
        while (*p && *p != '"' && *p != '\'' && *p != ',' && !isspace((unsigned char)*p) && i < (int)sizeof(valbuf) - 1)
        {
          valbuf[i++] = *p++;
        }
        valbuf[i] = '\0';
      }
    }

    // normalize to uppercase
    for (int i = 0; valbuf[i]; i++)
      valbuf[i] = toupper((unsigned char)valbuf[i]);

    if (strcmp(valbuf, "ON") == 0)
    {
      digitalWrite(RELAY_PIN, HIGH);
      config.is_on = true;
      config.off_time = 0;
      Serial.println("Control: OUTPUT ON");
      if (client.connected())
      {
        client.publish("/cardoz/ack", "ON");
      }
      // persist immediate control change
      saveSchedule();
    }
    else if (strcmp(valbuf, "OFF") == 0)
    {
      digitalWrite(RELAY_PIN, LOW);
      config.is_on = false;
      config.off_time = 0;
      Serial.println("Control: OUTPUT OFF");
      if (client.connected())
      {
        client.publish("/cardoz/ack", "OFF");
      }
      // persist immediate control change
      saveSchedule();
    }
    else
    {
      Serial.println("Unknown control value");
    }

    return;
  }

  // Note: Do not use the client in the callback to publish, subscribe or
  // unsubscribe as it may cause deadlocks when other things arrive while
  // sending and receiving acknowledgments. Instead, change a global variable,
  // or push to a queue and handle it in the loop after calling `client.loop()`.
}

void setup()
{
  // WiFi.mode(WIFI_STA); // explicitly set mode, esp defaults to STA+AP
  // it is a good practice to make sure your code sets wifi mode how you want it.

  // put your setup code here, to run once:
  Serial.begin(115200);

  // onboard LED initialization (DoIT ESP32 DevKit usually uses GPIO2)
  pinMode(LED_BUILTIN, OUTPUT);

  // relay pin setup
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);

  // start the blink thread before attempting WiFi connection
  xTaskCreate(blinkTask, "blink", 1024, nullptr, 1, nullptr);

  // --- configuration loading happens before WiFi so schedule can run when offline ---
  prefs.begin("home_irrigator", false);
  loadSchedule();
  // if prefs were empty we now have DEFAULT_CONFIG values, including default turn-on
  Serial.print("Initial schedule interval ");
  Serial.print(config.interval);
  Serial.print("s, duration ");
  Serial.print(config.duration);
  Serial.print("s, next_on_time ");
  Serial.println(config.next_on_time);
  // perform initial adjustments with whatever time source we have now
  unsigned long startupNow = getCurrentTime();
  // if we are using the special default epoch value, convert it to a usable time
  if (config.next_on_time == DEFAULT_TURN_ON_AT)
  {
    if (startupNow < 100000) // still using uptime (no NTP yet)
    {
      config.next_on_time = startupNow + config.interval;
      Serial.println("Offline startup: applied relative schedule from default");
    }
    else if (startupNow > DEFAULT_TURN_ON_AT)
    {
      // once clock is synced and we've already passed the epoch
      config.next_on_time = startupNow + config.interval;
      Serial.println("Default epoch passed; rescheduled relative to now");
    }
    // otherwise leave the default epoch in place and schedule will fire when time catches up
  }
  // store back any fixes
  adjustScheduleForMissedOn(startupNow);
  // enforce relay state if necessary
  if (config.is_on && config.off_time > 0)
  {
    if (startupNow < config.off_time)
    {
      digitalWrite(RELAY_PIN, HIGH);
      Serial.print("Restored relay ON until epoch: ");
      Serial.println(config.off_time);
    }
    else
    {
      digitalWrite(RELAY_PIN, LOW);
      config.is_on = false;
      config.off_time = 0;
      saveSchedule();
    }
  }

  // WiFiManager, Local initialization. Once its business is done, there is no need to keep it around
  WiFiManager wm;

  // reset settings - wipe stored credentials for testing
  // these are stored by the esp library
  // wm.resetSettings();

  // Automatically connect using saved credentials,
  // if connection fails, it starts an access point with the specified name ( "AutoConnectAP"),
  // if empty will auto generate SSID, if password is blank it will be anonymous AP (wm.autoConnect())
  // then goes into a blocking loop awaiting configuration and will return success result

  bool res;

  // res = wm.autoConnect(); // auto generated AP name from chipid
  // res = wm.autoConnect("AutoConnectAP"); // anonymous ap
  res = wm.autoConnect("AutoConnectAP", "password"); // password protected ap

  if (!res)
  {
    Serial.println("Failed to connect");
    blinkState = STATE_FAILED;
    // ESP.restart();
  }
  else
  {
    // if you get here you have connected to the WiFi
    Serial.println("connected...yeey :)");
    blinkState = STATE_WIFI_CONNECTED;

    // configure NTP (UTC) and wait for system time to be set
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    Serial.println("Waiting for NTP time sync...");
    time_t now = time(nullptr);
    int retry = 0;
    const int retry_count = 20;
    while (now < 100000 && retry < retry_count)
    {
      Serial.print(".");
      delay(500);
      now = time(nullptr);
      retry++;
    }
    if (now < 100000)
    {
      Serial.println("\nFailed to obtain time");
    }
    else
    {
      Serial.print("Current epoch: ");
      Serial.println((unsigned long)now);
    }
    // after syncing time it's worth re-checking schedule in case the clock jumped
    unsigned long syncedNow = (unsigned long)now;
    Serial.println("Re‑adjusting schedule after NTP sync");
    adjustScheduleForMissedOn(syncedNow);
    if (config.is_on && config.off_time > 0)
    {
      if (syncedNow < config.off_time)
      {
        // leave relay as is
      }
      else
      {
        digitalWrite(RELAY_PIN, LOW);
        config.is_on = false;
        config.off_time = 0;
        saveSchedule();
      }
    }
  }

  client.begin("broker.emqx.io", 1883, wifiClient);
  client.onMessage(messageReceived);

  // ensure MQTT is connected before leaving setup (blocks)
  while (!connectToMqtt())
  {
    delay(1000);
  }
}

void loop()
{
  client.loop();

  if (!client.connected())
  {
    // try reconnecting with a short backoff to avoid busy-looping
    if (!connectToMqtt())
    {
      // failed again, blink state already updated; wait before retrying
      vTaskDelay(5000 / portTICK_PERIOD_MS);
      return; // exit this iteration of Arduino loop()
    }
  }

  // Publish a heartbeat message every 30 seconds
  unsigned long nowMillis = millis();
  if (nowMillis - lastMillis > 30000)
  {
    lastMillis = nowMillis;
    client.publish("/cardoz/heartbeat", "alive");
  }

  // scheduling: use current time (epoch or uptime) to control relay based on config
  unsigned long now = getCurrentTime();

  // if we have a configuration but next_on_time is not initialized, set it now
  if (config.interval > 0 && config.next_on_time == 0)
  {
    config.next_on_time = now + config.interval;
    Serial.print("Initialized scheduling, next ON at: ");
    Serial.println(config.next_on_time);
  }

  // if now is ahead of next_on_time (e.g., device booted after scheduled ON), adjust
  adjustScheduleForMissedOn(now);

  // turn ON when it's time
  if (!config.is_on && config.next_on_time > 0 && now >= config.next_on_time)
  {
    digitalWrite(RELAY_PIN, HIGH);
    config.is_on = true;
    config.off_time = now + config.duration;
    // reset next_on_time to after this duration + interval
    config.next_on_time = now + config.duration + config.interval;
    Serial.print("Turned ON at epoch: ");
    Serial.println(now);
    Serial.print("Scheduled OFF at epoch: ");
    Serial.println(config.off_time);
    Serial.print("Next ON scheduled at epoch: ");
    Serial.println(config.next_on_time);
    if (client.connected())
    {
      client.publish("/cardoz/ack", "ON");
    }
    // persist schedule changes
    saveSchedule();
  }

  // turn OFF when duration elapsed
  if (config.is_on && config.off_time > 0 && now >= config.off_time)
  {
    digitalWrite(RELAY_PIN, LOW);
    config.is_on = false;
    config.off_time = 0;
    Serial.print("Turned OFF at epoch: ");
    Serial.println(now);
    if (client.connected())
    {
      client.publish("/cardoz/ack", "OFF");
    }
    // persist schedule changes
    saveSchedule();
  }

  // main work can go here; blink task runs independently
  vTaskDelay(1000 / portTICK_PERIOD_MS);
}

// blink task implementation
void blinkTask(void *param)
{
  (void)param;
  while (true)
  {
    switch (blinkState)
    {
    case STATE_WIFI_CONNECTING:
      digitalWrite(LED_BUILTIN, HIGH);
      vTaskDelay(500 / portTICK_PERIOD_MS);
      digitalWrite(LED_BUILTIN, LOW);
      vTaskDelay(500 / portTICK_PERIOD_MS);
      break;
    case STATE_WIFI_CONNECTED:
      digitalWrite(LED_BUILTIN, HIGH);
      vTaskDelay(100 / portTICK_PERIOD_MS);
      digitalWrite(LED_BUILTIN, LOW);
      vTaskDelay(100 / portTICK_PERIOD_MS);
      break;
    case STATE_MQTT_CONNECTING:
      // faster blink to indicate MQTT handshake
      digitalWrite(LED_BUILTIN, HIGH);
      vTaskDelay(150 / portTICK_PERIOD_MS);
      digitalWrite(LED_BUILTIN, LOW);
      vTaskDelay(150 / portTICK_PERIOD_MS);
      break;
    case STATE_MQTT_CONNECTED:
      // short single blink every 2 seconds to indicate active MQTT
      digitalWrite(LED_BUILTIN, HIGH);
      vTaskDelay(50 / portTICK_PERIOD_MS);
      digitalWrite(LED_BUILTIN, LOW);
      vTaskDelay(1950 / portTICK_PERIOD_MS);
      break;
    case STATE_FAILED:
      digitalWrite(LED_BUILTIN, HIGH);
      vTaskDelay(1000 / portTICK_PERIOD_MS);
      digitalWrite(LED_BUILTIN, LOW);
      vTaskDelay(1000 / portTICK_PERIOD_MS);
      break;
    case STATE_MQTT_FAILED:
      // very slow blink for MQTT failures
      digitalWrite(LED_BUILTIN, HIGH);
      vTaskDelay(2000 / portTICK_PERIOD_MS);
      digitalWrite(LED_BUILTIN, LOW);
      vTaskDelay(2000 / portTICK_PERIOD_MS);
      break;
    }
  }
}