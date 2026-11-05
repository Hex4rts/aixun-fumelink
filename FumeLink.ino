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
// Serial (115200) logs every state change. Commands: relay on|off, runon <ms>
// Status LED: blue = searching, green = linked, red = extracting, orange = run-on.
//
// Board: Waveshare ESP32-S3-Zero (WS2812 on GPIO21), arduino-esp32 3.3.x
// FQBN esp32:esp32:esp32s3:CDCOnBoot=cdc,USBMode=hwcdc,FlashSize=4M

#include <BLEDevice.h>
#include <BLEScan.h>
#include <host/ble_hs.h>

// ---- configuration -------------------------------------------------------
// GPIO4 drives an NPN that pulls the extractor's speed input (pot wiper, via 1 k in
// series) to 0 V: pin HIGH = fan stopped, pin LOW = fan at the pot's speed.
// 4k7 GPIO->base, 10k base->GND, emitter to the controller's GND.
static const int RELAY_PIN = 4;
static const bool RELAY_ACTIVE_HIGH = false;  // "extractor on" = pin LOW
static uint32_t runOnMs = 10000;          // keep extracting this long after the iron goes idle
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

static void scanDone(BLEScanResults) {
  BLEDevice::getScan()->clearResults();
}

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

static void handleCommand(String line) {
  line.trim();
  if (line == "relay on") setRelay(true, "serial");
  else if (line == "relay off") setRelay(false, "serial");
  else if (line.startsWith("runon ")) { runOnMs = line.substring(6).toInt(); logLine("run-on " + String(runOnMs) + " ms"); }
  else if (line.length()) logLine("? relay on|off | runon <ms>");
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
