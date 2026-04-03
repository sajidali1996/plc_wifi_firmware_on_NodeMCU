#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <DNSServer.h>
#include <EEPROM.h>

// ---------------------------
// PLC I/O configuration
// ---------------------------
const uint8_t RELAY_PINS[] = {5, 4, 14, 12}; // D1, D2, D5, D6
const uint16_t COIL_COUNT = 32;
const uint16_t REG_COUNT = 32;

bool coils[COIL_COUNT];
bool discreteInputs[COIL_COUNT];
uint16_t holdingRegisters[REG_COUNT];
uint16_t inputRegisters[REG_COUNT];

// ---------------------------
// Wi-Fi credential storage
// ---------------------------
const uint16_t EEPROM_SIZE = 256;
const uint16_t EEPROM_MAGIC = 0xBEEF;

struct WifiConfig {
  uint16_t magic;
  char ssid[32];
  char pass[64];
};

WifiConfig wifiConfig;

// ---------------------------
// Provisioning portal
// ---------------------------
DNSServer dnsServer;
ESP8266WebServer portalServer(80);
bool provisioningMode = false;

const byte DNS_PORT = 53;
String apName;

// ---------------------------
// Modbus TCP server
// ---------------------------
WiFiServer modbusServer(502);
WiFiClient modbusClient;

const char* HTML_FORM =
  "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
  "<title>PLC WiFi Setup</title></head><body style='font-family:Arial;padding:20px;'>"
  "<h2>PLC WiFi Provisioning</h2>"
  "<form action='/save' method='post'>"
  "<label>SSID</label><br><input name='ssid' maxlength='31' required><br><br>"
  "<label>Password</label><br><input name='pass' type='password' maxlength='63'><br><br>"
  "<button type='submit'>Save & Reboot</button>"
  "</form></body></html>";

void setRelayOutputsFromCoils() {
  for (uint8_t i = 0; i < sizeof(RELAY_PINS); i++) {
    digitalWrite(RELAY_PINS[i], coils[i] ? HIGH : LOW);
  }
}

void syncInputsFromState() {
  for (uint16_t i = 0; i < COIL_COUNT; i++) {
    discreteInputs[i] = coils[i];
  }

  // Example runtime values for SCADA/HMI reads.
  inputRegisters[0] = (uint16_t)(ESP.getFreeHeap() & 0xFFFF);
  inputRegisters[1] = (uint16_t)(WiFi.RSSI() < -32768 ? 0 : (WiFi.RSSI() + 32768));
}

void saveWifiConfig(const char* ssid, const char* pass) {
  memset(&wifiConfig, 0, sizeof(wifiConfig));
  wifiConfig.magic = EEPROM_MAGIC;
  strncpy(wifiConfig.ssid, ssid, sizeof(wifiConfig.ssid) - 1);
  strncpy(wifiConfig.pass, pass, sizeof(wifiConfig.pass) - 1);

  EEPROM.put(0, wifiConfig);
  EEPROM.commit();
}

bool loadWifiConfig() {
  EEPROM.get(0, wifiConfig);
  if (wifiConfig.magic != EEPROM_MAGIC) {
    return false;
  }

  if (strlen(wifiConfig.ssid) == 0) {
    return false;
  }

  return true;
}

void handlePortalRoot() {
  portalServer.send(200, "text/html", HTML_FORM);
}

void handlePortalSave() {
  if (!portalServer.hasArg("ssid") || !portalServer.hasArg("pass")) {
    portalServer.send(400, "text/plain", "Missing ssid/pass");
    return;
  }

  String ssid = portalServer.arg("ssid");
  String pass = portalServer.arg("pass");

  saveWifiConfig(ssid.c_str(), pass.c_str());

  portalServer.send(200, "text/plain", "Saved. Rebooting...");
  delay(1000);
  ESP.restart();
}

void startProvisioningPortal() {
  provisioningMode = true;
  WiFi.mode(WIFI_AP);

  uint32_t chip = ESP.getChipId();
  apName = "PLC-Setup-" + String(chip, HEX);

  WiFi.softAP(apName.c_str(), "plcsetup123");

  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());

  portalServer.on("/", handlePortalRoot);
  portalServer.on("/save", HTTP_POST, handlePortalSave);
  portalServer.onNotFound(handlePortalRoot);
  portalServer.begin();

  Serial.println("Provisioning AP started");
  Serial.print("AP Name: ");
  Serial.println(apName);
  Serial.println("AP Password: plcsetup123");
  Serial.print("Open: http://");
  Serial.println(WiFi.softAPIP());
}

bool connectWifiFromStoredConfig() {
  if (!loadWifiConfig()) {
    return false;
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiConfig.ssid, wifiConfig.pass);

  Serial.print("Connecting to WiFi");
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(300);
    Serial.print('.');
  }
  Serial.println();

  return WiFi.status() == WL_CONNECTED;
}

void sendModbusException(uint16_t txId, uint8_t unitId, uint8_t functionCode, uint8_t exceptionCode) {
  uint8_t resp[9];
  resp[0] = (uint8_t)(txId >> 8);
  resp[1] = (uint8_t)(txId & 0xFF);
  resp[2] = 0;
  resp[3] = 0;
  resp[4] = 0;
  resp[5] = 3; // unit + function + exception
  resp[6] = unitId;
  resp[7] = functionCode | 0x80;
  resp[8] = exceptionCode;
  modbusClient.write(resp, sizeof(resp));
}

void sendModbusReadBitsResponse(uint16_t txId, uint8_t unitId, uint8_t functionCode, bool* src, uint16_t startAddr, uint16_t quantity) {
  uint8_t byteCount = (quantity + 7) / 8;
  uint8_t resp[260] = {0};

  resp[0] = (uint8_t)(txId >> 8);
  resp[1] = (uint8_t)(txId & 0xFF);
  resp[2] = 0;
  resp[3] = 0;
  resp[4] = 0;
  resp[5] = (uint8_t)(3 + byteCount);
  resp[6] = unitId;
  resp[7] = functionCode;
  resp[8] = byteCount;

  for (uint16_t i = 0; i < quantity; i++) {
    if (src[startAddr + i]) {
      resp[9 + (i / 8)] |= (1 << (i % 8));
    }
  }

  modbusClient.write(resp, 9 + byteCount);
}

void sendModbusReadRegsResponse(uint16_t txId, uint8_t unitId, uint8_t functionCode, uint16_t* src, uint16_t startAddr, uint16_t quantity) {
  uint8_t byteCount = quantity * 2;
  uint8_t resp[260] = {0};

  resp[0] = (uint8_t)(txId >> 8);
  resp[1] = (uint8_t)(txId & 0xFF);
  resp[2] = 0;
  resp[3] = 0;
  resp[4] = 0;
  resp[5] = (uint8_t)(3 + byteCount);
  resp[6] = unitId;
  resp[7] = functionCode;
  resp[8] = byteCount;

  for (uint16_t i = 0; i < quantity; i++) {
    uint16_t value = src[startAddr + i];
    resp[9 + i * 2] = (uint8_t)(value >> 8);
    resp[10 + i * 2] = (uint8_t)(value & 0xFF);
  }

  modbusClient.write(resp, 9 + byteCount);
}

void sendModbusWriteAck(uint16_t txId, uint8_t unitId, uint8_t functionCode, uint16_t startAddr, uint16_t quantityOrValue) {
  uint8_t resp[12];
  resp[0] = (uint8_t)(txId >> 8);
  resp[1] = (uint8_t)(txId & 0xFF);
  resp[2] = 0;
  resp[3] = 0;
  resp[4] = 0;
  resp[5] = 6;
  resp[6] = unitId;
  resp[7] = functionCode;
  resp[8] = (uint8_t)(startAddr >> 8);
  resp[9] = (uint8_t)(startAddr & 0xFF);
  resp[10] = (uint8_t)(quantityOrValue >> 8);
  resp[11] = (uint8_t)(quantityOrValue & 0xFF);
  modbusClient.write(resp, sizeof(resp));
}

void processModbusFrame(uint8_t* req, uint16_t len) {
  if (len < 8) {
    return;
  }

  uint16_t txId = (uint16_t)(req[0] << 8) | req[1];
  uint16_t protocolId = (uint16_t)(req[2] << 8) | req[3];
  uint16_t pduLen = (uint16_t)(req[4] << 8) | req[5];
  uint8_t unitId = req[6];

  if (protocolId != 0 || pduLen + 6 != len) {
    return;
  }

  uint8_t functionCode = req[7];

  if (functionCode == 1 || functionCode == 2 || functionCode == 3 || functionCode == 4 || functionCode == 5 || functionCode == 6 || functionCode == 15 || functionCode == 16) {
    if (len < 12) {
      sendModbusException(txId, unitId, functionCode, 3);
      return;
    }
  }

  uint16_t startAddr = (uint16_t)(req[8] << 8) | req[9];
  uint16_t quantity = (uint16_t)(req[10] << 8) | req[11];

  switch (functionCode) {
    case 1: // Read Coils
      if (quantity < 1 || quantity > 2000 || startAddr + quantity > COIL_COUNT) {
        sendModbusException(txId, unitId, functionCode, 2);
        return;
      }
      sendModbusReadBitsResponse(txId, unitId, functionCode, coils, startAddr, quantity);
      break;

    case 2: // Read Discrete Inputs
      if (quantity < 1 || quantity > 2000 || startAddr + quantity > COIL_COUNT) {
        sendModbusException(txId, unitId, functionCode, 2);
        return;
      }
      sendModbusReadBitsResponse(txId, unitId, functionCode, discreteInputs, startAddr, quantity);
      break;

    case 3: // Read Holding Registers
      if (quantity < 1 || quantity > 125 || startAddr + quantity > REG_COUNT) {
        sendModbusException(txId, unitId, functionCode, 2);
        return;
      }
      sendModbusReadRegsResponse(txId, unitId, functionCode, holdingRegisters, startAddr, quantity);
      break;

    case 4: // Read Input Registers
      if (quantity < 1 || quantity > 125 || startAddr + quantity > REG_COUNT) {
        sendModbusException(txId, unitId, functionCode, 2);
        return;
      }
      sendModbusReadRegsResponse(txId, unitId, functionCode, inputRegisters, startAddr, quantity);
      break;

    case 5: { // Write Single Coil
      uint16_t value = quantity;
      if (startAddr >= COIL_COUNT || (value != 0xFF00 && value != 0x0000)) {
        sendModbusException(txId, unitId, functionCode, 3);
        return;
      }
      coils[startAddr] = (value == 0xFF00);
      setRelayOutputsFromCoils();
      sendModbusWriteAck(txId, unitId, functionCode, startAddr, value);
      break;
    }

    case 6: // Write Single Holding Register
      if (startAddr >= REG_COUNT) {
        sendModbusException(txId, unitId, functionCode, 2);
        return;
      }
      holdingRegisters[startAddr] = quantity;
      sendModbusWriteAck(txId, unitId, functionCode, startAddr, quantity);
      break;

    case 15: { // Write Multiple Coils
      if (len < 13) {
        sendModbusException(txId, unitId, functionCode, 3);
        return;
      }
      uint8_t byteCount = req[12];
      if (quantity < 1 || quantity > 1968 || startAddr + quantity > COIL_COUNT || (13 + byteCount) > len) {
        sendModbusException(txId, unitId, functionCode, 3);
        return;
      }

      for (uint16_t i = 0; i < quantity; i++) {
        bool bitVal = (req[13 + (i / 8)] >> (i % 8)) & 0x01;
        coils[startAddr + i] = bitVal;
      }

      setRelayOutputsFromCoils();
      sendModbusWriteAck(txId, unitId, functionCode, startAddr, quantity);
      break;
    }

    case 16: { // Write Multiple Holding Registers
      if (len < 13) {
        sendModbusException(txId, unitId, functionCode, 3);
        return;
      }
      uint8_t byteCount = req[12];
      if (quantity < 1 || quantity > 123 || startAddr + quantity > REG_COUNT || byteCount != quantity * 2 || (13 + byteCount) > len) {
        sendModbusException(txId, unitId, functionCode, 3);
        return;
      }

      for (uint16_t i = 0; i < quantity; i++) {
        holdingRegisters[startAddr + i] = ((uint16_t)req[13 + i * 2] << 8) | req[14 + i * 2];
      }

      sendModbusWriteAck(txId, unitId, functionCode, startAddr, quantity);
      break;
    }

    default:
      sendModbusException(txId, unitId, functionCode, 1);
      break;
  }
}

void handleModbusTcp() {
  if (!modbusClient || !modbusClient.connected()) {
    if (modbusClient) {
      modbusClient.stop();
    }
    modbusClient = modbusServer.available();
    return;
  }

  if (modbusClient.available() < 7) {
    return;
  }

  uint8_t header[7];
  if (modbusClient.read(header, 7) != 7) {
    return;
  }

  uint16_t lenField = ((uint16_t)header[4] << 8) | header[5];
  if (lenField == 0 || lenField > 253) {
    modbusClient.stop();
    return;
  }

  uint16_t remaining = lenField - 1; // unit id already in header
  uint8_t req[260] = {0};

  memcpy(req, header, 7);

  unsigned long startWait = millis();
  while (modbusClient.available() < remaining) {
    if (millis() - startWait > 1000) {
      return;
    }
    delay(1);
  }

  int readLen = modbusClient.read(req + 7, remaining);
  if (readLen != (int)remaining) {
    return;
  }

  processModbusFrame(req, 7 + remaining);
}

void setup() {
  Serial.begin(115200);
  delay(200);

  for (uint8_t i = 0; i < sizeof(RELAY_PINS); i++) {
    pinMode(RELAY_PINS[i], OUTPUT);
    coils[i] = (i == 0);
  }

  setRelayOutputsFromCoils();

  EEPROM.begin(EEPROM_SIZE);

  if (connectWifiFromStoredConfig()) {
    Serial.println("WiFi connected");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
    modbusServer.begin();
    Serial.println("Modbus TCP server started on port 502");
  } else {
    startProvisioningPortal();
  }
}

void loop() {
  syncInputsFromState();

  if (provisioningMode) {
    dnsServer.processNextRequest();
    portalServer.handleClient();
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    modbusClient.stop();
    startProvisioningPortal();
    return;
  }

  handleModbusTcp();
}
