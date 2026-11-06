// FumeLink - an ESP32 that joins an AiXun T420D's "FumeSuck Link" the way an ES02
// fume extractor does, and switches a relay so any extractor follows the iron.
//
// Protocol (ES02 1.06 + T420DS 1.05 firmware, Ghidra; verified live 2026-11-05):
//  * The station advertises "T420D_\x01" with service 0x00EE: EE01 = read (station
//    state, TLV list), EE02 = write. The extractor is the BLE client.
//  * Hello, written to EE02:  '1' <len> "ES02_<12 hex>"  '0' 01 01
//    The station checks the name starts with "ES02" (others: "BS08", "LOCK").
//    EE01 record '0' becomes 02 = "connection successful".
//  * Subscribe, written to EE02: '_' 01 'B'  -> EE01 now carries a 'B' record.
//  * 'B' record: 04 <mask> 00 <v0..v7>. Mask bit 6 (0x40) = extractor power:
//    set while a handle is out of the stand, cleared when it goes back.
//  * '9' record: 02 <nch> then per channel [flag][temp LE16][setpoint LE16][power%],
//    flag 1 = standby, 2 = working.
//
// The GATT client talks to NimBLE directly: the Arduino BLEClient aborts the connect
// when the station starts the MTU exchange itself (BLE_HS_EALREADY), leaving the link
// half open. Scanning still uses the Arduino BLEScan.
//
// WiFi: the board runs its own hotspot "FumeLink" (default password "fumelink" - change
// it on the page). http://192.168.4.1 shows live status and sets the run-on time, the
// hotspot password, manual fan on/off, and takes firmware updates (OTA, .bin upload).
// Settings live in NVS ("fumelink" namespace), never in this file.
//
// Serial (115200) logs every state change. Commands: relay on|off, runon <ms>, appass <pw>
// Status LED: blue = searching, green = linked, red = extracting, orange = run-on.
//
// Board: Waveshare ESP32-S3-Zero (WS2812 on GPIO21), arduino-esp32 3.3.x
// FQBN esp32:esp32:esp32s3:CDCOnBoot=cdc,USBMode=hwcdc,FlashSize=4M

#include <BLEDevice.h>
#include <BLEScan.h>
#include <host/ble_hs.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <Update.h>

// ---- configuration -------------------------------------------------------
// GPIO4 drives an NPN that pulls the extractor's speed input (pot wiper, via 1 k in
// series) to 0 V: pin HIGH = fan stopped, pin LOW = fan at the pot's speed.
// 4k7 GPIO->base, 10k base->GND, emitter to the controller's GND.
static const int RELAY_PIN = 4;
static const bool RELAY_ACTIVE_HIGH = false;  // "extractor on" = pin LOW
static uint32_t runOnMs = 10000;          // default run-on after the iron goes idle (NVS overrides)
static const char *AP_SSID = "FumeLink";
static const char *AP_DEFAULT_PASS = "fumelink";  // public default: change it on the page
static const int LED_PIN = 21;            // S3-Zero onboard WS2812
static const uint8_t LED_LEVEL = 40;      // brightness 0-255
static const int BUTTON_PIN = 0;          // S3-Zero BOOT button: press toggles the fan by hand
static const char *STATION_PREFIX = "T420D_";
static const uint32_t POLL_MS = 300;
// ---------------------------------------------------------------------------

static const ble_uuid16_t SVC_UUID = BLE_UUID16_INIT(0x00EE);

// Shared with NimBLE host-task callbacks.
static volatile uint16_t connHandle = BLE_HS_CONN_HANDLE_NONE;
static volatile int connStatus = 0;
static volatile bool connEvent = false, discEvent = false;
static volatile int discReason = 0;
static volatile bool opDone = false;
static volatile int opStatus = 0;
static volatile uint16_t svcStart = 0, svcEnd = 0, hEE01 = 0, hEE02 = 0;
static uint8_t readBuf[256];
static volatile size_t readLen = 0;

static ble_addr_t stationAddr;
static bool haveStation = false;
static bool linked = false;
static bool relayOn = false;
static uint32_t offAt = 0;  // 0 = no pending run-on timer
static uint8_t lastMask = 0xFF;
static uint8_t lastFlags[2] = {0, 0};

// Read by the web task; written by the main loop.
static volatile uint8_t chFlag[2] = {0, 0};
static volatile uint16_t chTemp[2] = {0, 0}, chSet[2] = {0, 0};
static volatile int webFanCmd = -1;  // set by the web task: 0 = off, 1 = on

static Preferences prefs;
static WebServer web(80);
static String apPass;

static void logLine(const String &msg) {
  Serial.printf("%10lu ms  %s\n", (unsigned long)millis(), msg.c_str());
}

static void updateLed() {
  uint8_t r = 0, g = 0, b = 0;
  if (relayOn && offAt) { r = LED_LEVEL; g = LED_LEVEL / 3; }  // orange: run-on
  else if (relayOn) r = LED_LEVEL;                              // red: extracting
  else if (linked) g = LED_LEVEL;                               // green: linked, idle
  else b = LED_LEVEL / 2;                                       // blue: searching
  rgbLedWrite(LED_PIN, r, g, b);
}

static void setRelay(bool on, const char *why) {
  offAt = 0;
  if (on != relayOn) logLine(String("RELAY ") + (on ? "ON" : "OFF") + "  (" + why + ")");
  relayOn = on;
  if (RELAY_PIN >= 0) digitalWrite(RELAY_PIN, (on == RELAY_ACTIVE_HIGH) ? HIGH : LOW);
}

static void startRunOn(const char *why) {
  if (relayOn && !offAt) {
    offAt = millis() + runOnMs;
    if (!offAt) offAt = 1;
    logLine(String(why) + " - relay off in " + String(runOnMs) + " ms");
  }
}

static String helloName() {
  uint8_t m[6];
  sscanf(BLEDevice::getAddress().toString().c_str(), "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
         &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]);
  char b[24];
  snprintf(b, sizeof b, "ES02_%02X%02X%02X%02X%02X%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
  return String(b);
}

// ---- NimBLE callbacks (host task) ------------------------------------------
static int gapEvent(struct ble_gap_event *e, void *) {
  switch (e->type) {
    case BLE_GAP_EVENT_CONNECT:
      connStatus = e->connect.status;
      if (e->connect.status == 0) connHandle = e->connect.conn_handle;
      connEvent = true;
      break;
    case BLE_GAP_EVENT_DISCONNECT:
      connHandle = BLE_HS_CONN_HANDLE_NONE;
      discReason = e->disconnect.reason;
      discEvent = true;
      opStatus = BLE_HS_ENOTCONN;  // unblock any pending GATT wait
      opDone = true;
      break;
    default:
      break;  // MTU exchange etc. need nothing from us
  }
  return 0;
}

static int svcCb(uint16_t, const struct ble_gatt_error *err, const struct ble_gatt_svc *svc, void *) {
  if (err->status == 0) {
    svcStart = svc->start_handle;
    svcEnd = svc->end_handle;
  } else {
    opStatus = err->status == BLE_HS_EDONE ? 0 : err->status;
    opDone = true;
  }
  return 0;
}

static int chrCb(uint16_t, const struct ble_gatt_error *err, const struct ble_gatt_chr *chr, void *) {
  if (err->status == 0) {
    uint16_t u = ble_uuid_u16(&chr->uuid.u);
    if (u == 0xEE01) hEE01 = chr->val_handle;
    if (u == 0xEE02) hEE02 = chr->val_handle;
  } else {
    opStatus = err->status == BLE_HS_EDONE ? 0 : err->status;
    opDone = true;
  }
  return 0;
}

static int writeCb(uint16_t, const struct ble_gatt_error *err, struct ble_gatt_attr *, void *) {
  opStatus = err->status;
  opDone = true;
  return 0;
}

static int readCb(uint16_t, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *) {
  if (err->status == 0 && attr) {
    uint16_t n = OS_MBUF_PKTLEN(attr->om);
    if (attr->offset + n <= sizeof readBuf) {
      os_mbuf_copydata(attr->om, 0, n, readBuf + attr->offset);
      readLen = attr->offset + n;
    }
    return 0;
  }
  opStatus = err->status == BLE_HS_EDONE ? 0 : err->status;
  opDone = true;
  return 0;
}
// ---------------------------------------------------------------------------

static void opBegin() {
  opDone = false;
  opStatus = 0;
}

static int opWait(uint32_t timeoutMs) {
  uint32_t t0 = millis();
  while (!opDone && millis() - t0 < timeoutMs) delay(5);
  return opDone ? opStatus : BLE_HS_ETIMEOUT;
}

static bool gattWrite(const uint8_t *d, size_t n) {
  opBegin();
  int rc = ble_gattc_write_flat(connHandle, hEE02, d, n, writeCb, nullptr);
  if (rc == 0) rc = opWait(3000);
  if (rc) logLine("write failed rc=" + String(rc));
  return rc == 0;
}

static bool gattRead() {
  readLen = 0;
  opBegin();
  int rc = ble_gattc_read_long(connHandle, hEE01, 0, readCb, nullptr);
  if (rc == 0) rc = opWait(3000);
  if (rc) logLine("read failed rc=" + String(rc));
  return rc == 0;
}

static void disconnectStation() {
  if (connHandle != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(connHandle, BLE_ERR_REM_USER_CONN_TERM);
}

// Walk the TLV list from EE01 and act on the '0' (link), '9' (channels) and 'B' (power) records.
static void handleState(const uint8_t *d, size_t n) {
  for (size_t i = 0; i + 1 < n; i += d[i + 1] + 2) {
    uint8_t tag = d[i], len = d[i + 1];
    const uint8_t *v = d + i + 2;
    if (i + 2 + len > n) break;
    if (tag == '0' && len >= 1) {
      bool ok = v[0] == 0x02;
      if (ok != linked) logLine(ok ? "LINKED (station accepted us)" : "link state " + String(v[0]));
      linked = ok;
    } else if (tag == '9' && len >= 2) {
      for (uint8_t ch = 0; ch < 2 && 2 + 6 * ch + 5 < len; ch++) {
        const uint8_t *c = v + 2 + 6 * ch;
        chFlag[ch] = c[0];
        chTemp[ch] = c[1] | c[2] << 8;
        chSet[ch] = c[3] | c[4] << 8;
        if (c[0] != lastFlags[ch]) {
          logLine("CH" + String(ch + 1) + (c[0] == 2 ? " working" : " standby") + "  T=" +
                  String(c[1] | c[2] << 8) + " set=" + String(c[3] | c[4] << 8));
          lastFlags[ch] = c[0];
        }
      }
    } else if (tag == 'B' && len >= 2 && v[0] == 0x04) {
      uint8_t mask = v[1];
      // Act on changes only, so a manual button toggle holds until the iron changes state.
      if (mask != lastMask) {
        logLine("B mask 0x" + String(mask, HEX));
        lastMask = mask;
        if (mask & 0x40) setRelay(true, "iron working");
        else startRunOn("iron idle");
      }
    }
  }
}

class ScanCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice dev) override {
    if (haveStation) return;
    if (dev.haveName() && String(dev.getName().c_str()).startsWith(STATION_PREFIX)) {
      memcpy(stationAddr.val, dev.getAddress().getNative(), 6);
      stationAddr.type = dev.getAddressType();
      haveStation = true;
      logLine("found station " + String(dev.getAddress().toString().c_str()) + " rssi " + String(dev.getRSSI()));
      BLEDevice::getScan()->stop();
    }
  }
};

// Nothing to do: start() clears the previous results itself. Clearing here too (from the
// NimBLE task) raced start() in loop() and double-freed the results (CORRUPT HEAP).
static void scanDone(BLEScanResults) {}

static bool connectStation() {
  BLEDevice::getScan()->stop();
  delay(200);
  connEvent = discEvent = false;
  int rc = ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &stationAddr, 10000, nullptr, gapEvent, nullptr);
  if (rc) {
    logLine("ble_gap_connect rc=" + String(rc));
    return false;
  }
  uint32_t t0 = millis();
  while (!connEvent && millis() - t0 < 11000) delay(5);
  if (!connEvent || connStatus != 0) {
    logLine("connect failed status=" + String(connStatus));
    if (!connEvent) ble_gap_conn_cancel();
    return false;
  }
  logLine("connected, handle " + String(connHandle));

  svcStart = svcEnd = hEE01 = hEE02 = 0;
  opBegin();
  rc = ble_gattc_disc_svc_by_uuid(connHandle, &SVC_UUID.u, svcCb, nullptr);
  if (rc == 0) rc = opWait(5000);
  if (rc || !svcStart) {
    logLine("service 0x00EE not found rc=" + String(rc));
    disconnectStation();
    return false;
  }
  opBegin();
  rc = ble_gattc_disc_all_chrs(connHandle, svcStart, svcEnd, chrCb, nullptr);
  if (rc == 0) rc = opWait(5000);
  if (rc || !hEE01 || !hEE02) {
    logLine("EE01/EE02 not found rc=" + String(rc));
    disconnectStation();
    return false;
  }

  String name = helloName();
  uint8_t hello[40];
  size_t k = 0;
  hello[k++] = '1';
  hello[k++] = name.length();
  memcpy(hello + k, name.c_str(), name.length());
  k += name.length();
  hello[k++] = '0';
  hello[k++] = 0x01;
  hello[k++] = 0x01;
  if (!gattWrite(hello, k)) { disconnectStation(); return false; }
  logLine("hello sent as " + name);
  delay(1000);
  const uint8_t subscribe[] = {'_', 0x01, 'B'};
  if (!gattWrite(subscribe, sizeof subscribe)) { disconnectStation(); return false; }
  logLine("subscribed to 'B'");
  return true;
}

static void saveRunOn(uint32_t ms) {
  runOnMs = constrain(ms, 0UL, 3600000UL);
  prefs.putUInt("runon", runOnMs);
  logLine("run-on " + String(runOnMs) + " ms (saved)");
}

static void startAp() {
  WiFi.softAP(AP_SSID, apPass.c_str());
  logLine("hotspot " + String(AP_SSID) + " up at " + WiFi.softAPIP().toString());
}

static bool saveApPass(const String &pw) {
  if (pw.length() < 8 || pw.length() > 63) return false;  // WPA2 limits
  apPass = pw;
  prefs.putString("appass", apPass);
  logLine("hotspot password changed");
  return true;
}

static void handleCommand(String line) {
  line.trim();
  if (line == "relay on") setRelay(true, "serial");
  else if (line == "relay off") setRelay(false, "serial");
  else if (line.startsWith("runon ")) saveRunOn(line.substring(6).toInt());
  else if (line.startsWith("appass ")) {
    if (saveApPass(line.substring(7))) startAp();
    else logLine("password must be 8-63 characters");
  } else if (line.length()) logLine("? relay on|off | runon <ms> | appass <8-63 chars>");
}

// ---- web page (own task, so it answers even while the BLE side is busy) ----
static const char PAGE[] PROGMEM = R"HTML(<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>FumeLink</title>
<style>body{font-family:system-ui,sans-serif;max-width:30em;margin:1em auto;padding:0 1em;background:#111;color:#eee}
h1{font-size:1.4em}section{background:#222;border-radius:8px;padding:.8em 1em;margin:.8em 0}
.big{font-size:1.6em;font-weight:600}.on{color:#f55}.off{color:#5c5}.dim{color:#999}
button,input[type=submit]{font-size:1em;padding:.5em 1em;margin:.2em;border-radius:6px;border:0;background:#446;color:#fff}
input[type=number],input[type=password]{font-size:1em;padding:.4em;width:8em}table{width:100%}td{padding:.2em 0}</style></head>
<body><h1>FumeLink</h1>
<section><div class="big" id="fan">…</div><div id="link" class="dim"></div>
<table><tr><td>Channel 1</td><td id="c0"></td></tr><tr><td>Channel 2</td><td id="c1"></td></tr></table>
<button onclick="fan(1)">Fan ON</button><button onclick="fan(0)">Fan OFF</button>
<div class="dim">Manual switch holds until the iron next changes state.</div></section>
<section><form method="post" action="/set">Run-on after the iron goes idle:
<input type="number" name="runon" min="0" max="3600" step="1" id="ro"> s <input type="submit" value="Save"></form></section>
<section><form method="post" action="/appass">Hotspot password (8-63 chars):
<input type="password" name="pw" minlength="8" maxlength="63"> <input type="submit" value="Change"></form>
<div class="dim">You will be disconnected; reconnect with the new password.</div></section>
<section><form method="post" action="/update" enctype="multipart/form-data">Firmware update (.bin):
<input type="file" name="fw" accept=".bin"> <input type="submit" value="Upload"></form>
<div class="dim">A failed upload keeps the current firmware.</div></section>
<script>
function fan(v){fetch('/fan?on='+v,{method:'POST'}).then(tick)}
let roSet=false;
function tick(){fetch('/status').then(r=>r.json()).then(s=>{
 const f=document.getElementById('fan');
 f.textContent=s.fan?(s.runon_left>0?'Fan ON - stops in '+Math.ceil(s.runon_left/1000)+' s':'Fan ON'):'Fan OFF';
 f.className='big '+(s.fan?'on':'off');
 document.getElementById('link').textContent=s.linked?'Linked to the station':'Searching for the station…';
 s.ch.forEach((c,i)=>document.getElementById('c'+i).textContent=
  c.f?(c.f==2?'working':'standby')+' - '+c.t+' °C (set '+c.s+' °C)':'-');
 if(!roSet){document.getElementById('ro').value=s.runon_ms/1000;roSet=true}
})}
tick();setInterval(tick,1000);
</script></body></html>)HTML";

static void webSetup() {
  web.on("/", HTTP_GET, [] { web.send_P(200, "text/html", PAGE); });
  web.on("/status", HTTP_GET, [] {
    uint32_t left = (relayOn && offAt) ? (uint32_t)max(0L, (long)(offAt - millis())) : 0;
    String j = String("{\"linked\":") + (linked ? "true" : "false") + ",\"fan\":" + (relayOn ? "true" : "false") +
               ",\"runon_ms\":" + runOnMs + ",\"runon_left\":" + left + ",\"ch\":[";
    for (int i = 0; i < 2; i++)
      j += String(i ? "," : "") + "{\"f\":" + chFlag[i] + ",\"t\":" + chTemp[i] + ",\"s\":" + chSet[i] + "}";
    web.send(200, "application/json", j + "]}");
  });
  web.on("/fan", HTTP_POST, [] {
    webFanCmd = web.arg("on") == "1" ? 1 : 0;  // applied by the main loop
    web.send(200, "text/plain", "ok");
  });
  web.on("/set", HTTP_POST, [] {
    saveRunOn((uint32_t)web.arg("runon").toInt() * 1000);
    web.sendHeader("Location", "/");
    web.send(303);
  });
  web.on("/appass", HTTP_POST, [] {
    if (!saveApPass(web.arg("pw"))) {
      web.send(400, "text/plain", "Password must be 8-63 characters.");
      return;
    }
    web.send(200, "text/plain", "Saved. Reconnect to FumeLink with the new password.");
    delay(500);
    startAp();
  });
  web.on(
    "/update", HTTP_POST,
    [] {
      bool ok = !Update.hasError();
      web.send(200, "text/plain", ok ? "Update OK - rebooting." : String("Update failed: ") + Update.errorString());
      if (ok) {
        delay(800);
        ESP.restart();
      }
    },
    [] {
      HTTPUpload &up = web.upload();
      if (up.status == UPLOAD_FILE_START) {
        logLine("OTA start: " + up.filename);
        Update.begin(UPDATE_SIZE_UNKNOWN);
      } else if (up.status == UPLOAD_FILE_WRITE) {
        Update.write(up.buf, up.currentSize);
      } else if (up.status == UPLOAD_FILE_END) {
        if (Update.end(true)) logLine("OTA done, " + String(up.totalSize) + " bytes");
        else logLine(String("OTA failed: ") + Update.errorString());
      }
    });
  web.begin();
}

static void webTask(void *) {
  for (;;) {
    web.handleClient();
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);
  if (RELAY_PIN >= 0) pinMode(RELAY_PIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  setRelay(false, "boot");
  updateLed();
  BLEDevice::init("FumeLink");
  BLEScan *scan = BLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(new ScanCallbacks());
  scan->setActiveScan(true);
  scan->setInterval(100);
  scan->setWindow(90);
  logLine("FumeLink client up, hello name " + helloName());

  prefs.begin("fumelink", false);
  runOnMs = prefs.getUInt("runon", runOnMs);
  apPass = prefs.getString("appass", AP_DEFAULT_PASS);
  logLine("run-on " + String(runOnMs) + " ms");
  WiFi.mode(WIFI_AP);
  startAp();
  webSetup();
  xTaskCreatePinnedToCore(webTask, "web", 8192, nullptr, 1, nullptr, 1);
}

void loop() {
  static String line;
  static uint32_t lastPoll = 0;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') { handleCommand(line); line = ""; }
    else line += c;
  }

  // BOOT button: toggle by hand; the iron's next state change takes over again.
  static bool lastBtn = true;
  static uint32_t btnAt = 0;
  bool btn = digitalRead(BUTTON_PIN);
  if (!btn && lastBtn && millis() - btnAt > 250) {
    btnAt = millis();
    setRelay(!relayOn, "button");
  }
  lastBtn = btn;

  if (webFanCmd >= 0) {
    setRelay(webFanCmd == 1, "web");
    webFanCmd = -1;
  }

  if (discEvent) {
    discEvent = false;
    logLine("DISCONNECTED reason " + String(discReason));
    linked = false;
    lastMask = 0xFF;
    haveStation = false;
    startRunOn("link lost");
  }

  // Scan in the background so the button and LED keep working while the iron is off.
  static uint32_t retryAt = 0;
  if (connHandle == BLE_HS_CONN_HANDLE_NONE) {
    if (!haveStation) {
      if (!BLEDevice::getScan()->isScanning() && (int32_t)(millis() - retryAt) >= 0)
        BLEDevice::getScan()->start(5, scanDone, false);
    } else if (!connectStation()) {
      haveStation = false;
      retryAt = millis() + 2000;
    }
  } else if (millis() - lastPoll >= POLL_MS) {
    lastPoll = millis();
    if (gattRead()) handleState(readBuf, readLen);
  }

  if (offAt && (int32_t)(millis() - offAt) >= 0) setRelay(false, "run-on elapsed");
  updateLed();
  delay(5);
}
