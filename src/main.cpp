/*
Program grabs frames of data from Chargery BMS RS-232 port via a converter RS232<->TTL
connected to rx serial port at pin 16 and to tx chargery bms out (port 3) 
The frames are parsed, transformed into mqtt messages and published to three topics
 corresponding to the 3 frames (Cell Voltages, Measure Values, Cells impedance): 
*/

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <HardwareSerial.h>
#include <ArduinoJson.h>

#include "credentials.h"

// debug log, set to 1 to enable
#define ENABLE_DEBUG_LOG 1

// to do ota update via wifi
#define OTA_HANDLER

/*for debug, simulation  frames of bms 16 to send via bluetooth serial
24 24 56 2D 0C FD 0D 04 0D 04 0D 02 0D 03 0D 04 0D 06 0D 01 0D 08 0D 02 0D 05 0C FE 0D 06 0C FB 0D 0F 0C FC 76 FE D5 02 63 14 0E 00 95
24 24 58 28 01 E4 00 01 00 03 00 03 00 03 00 02 00 03 00 00 00 00 00 01 00 01 00 01 00 00 00 05 00 02 00 03 00 03 00 CC
24 24 57 0F 0E 24 01 00 E4 00 83 00 84 5B 27
*/
//#define BLUETOOTH

#ifdef OTA_HANDLER
#include <ArduinoOTA.h>
#endif // OTA_HANDLER

#ifdef BLUETOOTH
#include <BluetoothSerial.h>
BluetoothSerial bmsSerial;
#else
// BMS connected to rx port 16 of esp32 at 115200bps 8N1 with rs232->ttl converter
HardwareSerial bmsSerial(2);
#endif

// define the bms model with total of cells: 8, 16 or 24
const int bmsCellsTotal = 16;
// define number of cells wired : used for calculating diff voltage, min index and max index
const int bmsCellsWired = 14;

// bms protocol hexadecimal start byte(s), based on v1.25 protocol
const byte rcvhex = 0x56;
const byte rmvhex = 0x57;
const byte rcihex = 0x58;
const byte startbytehex = 0x24;

const bool verifyChecksum = true;

// mqtt topics
#define ROOT_TOPIC "bms"
#define NODE_ID "1_16T"
char const *rcvtopic = ROOT_TOPIC "/" NODE_ID "/cellvoltages";
char const *rmvtopic = ROOT_TOPIC "/" NODE_ID "/measures";
char const *rcitopic = ROOT_TOPIC "/" NODE_ID "/cellimpedances";

// Replace the next variables with your SSID/Password combination
const char *ssid = WIFI_SSID;
const char *password = WIFI_PASSWD;

// Add your MQTT Broker IP address, example:
//const char* mqtt_server = "YOUR_MQTT_BROKER_IP_ADDRESS";
const char *mqtt_server = "192.168.10.2";

// rs serial port config
const int baudrate = 115200;
const int rs_config = SERIAL_8N1;

unsigned long lastReconnectAttempt = 0;

byte cmd[64];

bool newInfo = false;
byte newCmd;

struct MinMaxDiff
{
  int minidx;
  int maxidx;
  int diffmv;
};

const size_t rmvjsoncapacity = JSON_OBJECT_SIZE(6);
const size_t rcvjsoncapacity = JSON_ARRAY_SIZE(16) + 2 * JSON_OBJECT_SIZE(5) + 70;
const size_t rcijsoncapacity = JSON_ARRAY_SIZE(16) + JSON_OBJECT_SIZE(3);

WiFiClient espClient;
PubSubClient client(espClient);

void debug_log(char const *str)
{
#if ENABLE_DEBUG_LOG == 1
  Serial.println(str);
#endif
}
void debug_printCmd()
{
#if ENABLE_DEBUG_LOG == 1
  for (int i = 0; i < 64; i++)
  {
    if (cmd[i] < 0x10)
    {
      Serial.print("0");
    }
    Serial.print(cmd[i], HEX);
    Serial.print(" ");
  }
  Serial.println("");
#endif
}

void setup_wifi()
{
  delay(10);

  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED)
  {
    debug_log("Connecting to WiFi network");
    delay(500);
    debug_log(".");
  }
#if ENABLE_DEBUG_LOG == 1
  Serial.println("");
  Serial.println("WiFi connected");
  Serial.println("IP address: ");
  Serial.println(WiFi.localIP());
  Serial.println("SSID: ");
  Serial.println(ssid);
#endif
}

boolean reconnect()
{
  if (client.connect("BMS16TClient"))
  {
    // Once connected, publish an announcement...
    client.publish("outTopic", "Power is coming");
    // ... and resubscribe
    //client.subscribe("esp32/output");
  }
  return client.connected();
}

void listenToBMS()
{
  static byte incomingByte;
  static byte lastByte;
  static bool recordData = false;
  static int nbToRecord = 0;

  while (bmsSerial.available() > 0 && newInfo == false)
  {
    incomingByte = bmsSerial.read();

    // start pattern 0x24 0x24
    if (incomingByte == startbytehex && lastByte == startbytehex)
    {
      debug_log("start pattern detected");
      recordData = true;
    }
    else if (recordData == true)
    {
      debug_log("->start recording cmd");
      // reset to 0 cmd
      memset(cmd, 0, sizeof(cmd));

      // commade 0x57 AscII:W RMV
      if (incomingByte == rmvhex)
      {
        debug_log("-->start recording cmd 0x57");
        newCmd = rmvhex;
        nbToRecord = 12; // 0F=2+1+1+2+1+2+2+2+1+1=15, 15-3 (242457)=12
        bmsSerial.readBytes(cmd, nbToRecord);
        newInfo = true;
        debug_printCmd();
        debug_log("-->end recording cmd 0x57");
      }
      // commade 0x56 AscII:V RCV
      else if (incomingByte == rcvhex)
      {
        debug_log("-->start recording cmd 0x56");
        newCmd = rcvhex;
        nbToRecord = 42; // 2D=2+1+1+16*2+4+4+1=45,45 - 3 (242456) = 42
        bmsSerial.readBytes(cmd, nbToRecord);
        newInfo = true;
        debug_printCmd();
        debug_log("-->end recording cmd 0x56");
      }
      else if (incomingByte == rcihex)
      {
        debug_log("-->start recording cmd 0x58");
        newCmd = rcihex;
        nbToRecord = 37; //  28=2+1+1+1+2+16*2+1=40, 40- 3 (242458) = 37
        bmsSerial.readBytes(cmd, nbToRecord);
        newInfo = true;
        debug_printCmd();
        debug_log("-->end recording cmd 0x58");
      }
      else
      {
        debug_log("--> !! cmd not in protocol or bad transmit !!");
        // commande inconnue
        newCmd = 0x0;
      }
      //end recording cmd received
      debug_log("->end recording cmd");
      recordData = false;
    }
    lastByte = incomingByte;
  }
}

MinMaxDiff maxAbsDiff(float arr[], int n)
{
  MinMaxDiff mmD;

  float minEle = arr[0];
  float maxEle = arr[0];
  mmD.minidx = 0;
  mmD.maxidx = 0;

  for (int i = 1; i < n; i++)
  {
    minEle = _min(minEle, arr[i]);
    maxEle = _max(maxEle, arr[i]);
    if (minEle == arr[i])
    {
      mmD.minidx = i;
    }
    if (maxEle == arr[i])
    {
      mmD.maxidx = i;
    }
  }

  mmD.diffmv = (maxEle - minEle) * 1000;
  return mmD;
}
DynamicJsonDocument parseRCVToJson()
{
  debug_log("start parsing rcv cmd 0x56");
  DynamicJsonDocument rcvJson(rcvjsoncapacity);
  float cellsV[bmsCellsTotal];
  float sumCellV = 0.0;
  JsonArray cell = rcvJson.createNestedArray("cell");
  float voltage;
  for (int i = 0; i < bmsCellsTotal; i++)
  {
    voltage = (cmd[i * 2 + 1] * 256 + cmd[i * 2 + 2]) / 1000.0;
    sumCellV += voltage;
#if ENABLE_DEBUG_LOG == 1
    Serial.print(",");
    Serial.print(i);
    Serial.print("=");
    Serial.print(voltage);
#endif
    cell.add(voltage);
    cellsV[i] = voltage;
  }

  int startwhbyte = bmsCellsTotal * 2 + 1;
  //rcvJson["wh"] = *(unsigned long *)&cmd[startwhbyte] / 1000.0;
  rcvJson["wh"] = ((int)cmd[startwhbyte + 3] * 256 * 256 * 256 + (int)cmd[startwhbyte + 2] * 256 * 256 + (int)cmd[startwhbyte + 1] * 256 + (int)cmd[startwhbyte]) / 1000.0;
  startwhbyte = startwhbyte + 4;
  //rcvJson["ah"] = *(unsigned long *)&cmd[startwhbyte] / 1000.0;
  rcvJson["ah"] = ((int)cmd[startwhbyte + 3] * 256 * 256 * 256 + (int)cmd[startwhbyte + 2] * 256 * 256 + (int)cmd[startwhbyte + 1] * 256 + (int)cmd[startwhbyte]) / 1000.0;
  rcvJson["vbatt"] = sumCellV;
  JsonObject diff = rcvJson.createNestedObject("diff");

  MinMaxDiff mmd = maxAbsDiff(cellsV, bmsCellsWired);
  diff["minidx"] = mmd.minidx;
  diff["vcellmin"] = cellsV[mmd.minidx];
  diff["maxidx"] = mmd.maxidx;
  diff["vcellmax"] = cellsV[mmd.maxidx];
  diff["diffmv"] = mmd.diffmv;
  debug_log("end parsing rcv cmd 0x56");
  return rcvJson;
}
DynamicJsonDocument parseRMVToJson()
{
  debug_log("start parsing rmv cmd 0x57");
  DynamicJsonDocument rmvJson(rmvjsoncapacity);
  rmvJson["eoc"] = ((int)cmd[1] * 256 + (int)cmd[2]) / 1000.0;
  rmvJson["mode"] = (int)cmd[3];
  rmvJson["current"] = ((int)cmd[4] * 256 + (int)cmd[5]) / 10.0;
  rmvJson["t1"] = ((int)cmd[6] * 256 + (int)cmd[7]) / 10.0;
  rmvJson["t2"] = ((int)cmd[8] * 256 + (int)cmd[9]) / 10.0;
  rmvJson["soc"] = (int)cmd[10];
  debug_log("end parsing rmv cmd 0x57");
  return rmvJson;
}
DynamicJsonDocument parseRCIToJson()
{
  debug_log("start parsing rci cmd 0x58");
  DynamicJsonDocument rciJson(rcijsoncapacity);
  JsonArray rCell = rciJson.createNestedArray("rcell");
  float resistance;
  for (int i = 0; i < bmsCellsTotal; i++)
  {
    resistance = ((int)cmd[i * 2 + 4] + (int)cmd[i * 2 + 5] * 256) / 10.0;
    rCell.add(resistance);
  }
  rciJson["mode"] = (int)cmd[1];
  rciJson["curent"] = ((int)cmd[3] * 256 + (int)cmd[2]) / 10.0;
  ;

  return rciJson;
}

bool checkSum(byte frameChecksum, const byte *prefixFrame, const int sizeOfPrefix)
{
  bool isValid = false;
  int checksum = 0;

  for (int i = 0; i < sizeOfPrefix; i++)
  {
    checksum += (int)prefixFrame[i];
    //Serial.print(checksum);
    //Serial.print("+");
  }
  //Serial.println("");
  // data lenght=cmd[Ø] -3 pour le prefixe -1 pour ne pas compter le checksum
  for (int i = 0; i < (int)cmd[0] - 3 - 1; i++)
  {
    checksum += (int)cmd[i];
    //Serial.print(checksum);
    //Serial.print("+");
  }
  Serial.println("");
  int calculatedChecksum = checksum % 256;
  /*Serial.print("calcCheck=");
  Serial.println(calculatedChecksum);
  Serial.println("frameCheck=");
  Serial.println(frameChecksum);*/
  if (calculatedChecksum == (int)frameChecksum)
  {
    isValid = true;
  }
  return isValid;
}
void setup()
{
  Serial.begin(baudrate, rs_config);
  Serial.println(rcvtopic);
  debug_log("serial esp32 begin");

#ifdef BLUETOOTH
  bmsSerial.begin("BMSTestClient"); //Bluetooth device name
  debug_log("Open Bluetooth Server");
#else
  bmsSerial.begin(baudrate, rs_config, 16, 17);
#endif

  debug_log("serial to bms begin, tx of bms on pin 16 of esp32");
  setup_wifi();

#ifdef OTA_HANDLER
  ArduinoOTA
      .onStart([]() {
        String type;
        if (ArduinoOTA.getCommand() == U_FLASH)
          type = "sketch";
        else // U_SPIFFS
          type = "filesystem";

        // NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
        Serial.println("Start updating " + type);
      })
      .onEnd([]() {
        Serial.println("\nEnd");
      })
      .onProgress([](unsigned int progress, unsigned int total) {
        Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
      })
      .onError([](ota_error_t error) {
        Serial.printf("Error[%u]: ", error);
        if (error == OTA_AUTH_ERROR)
          Serial.println("Auth Failed");
        else if (error == OTA_BEGIN_ERROR)
          Serial.println("Begin Failed");
        else if (error == OTA_CONNECT_ERROR)
          Serial.println("Connect Failed");
        else if (error == OTA_RECEIVE_ERROR)
          Serial.println("Receive Failed");
        else if (error == OTA_END_ERROR)
          Serial.println("End Failed");
      });
  // if DNSServer is started with "*" for domain name, it will reply with
  // provided IP to all DNS request

  ArduinoOTA.begin();
#endif // OTA_HANDLER

  //set client mqtt
  client.setServer(mqtt_server, 1883);
  //client.setCallback(callback);
  debug_log("mqtt client initialized");
  delay(1000);
}
void loop()
{
#ifdef OTA_HANDLER
  ArduinoOTA.handle();
#endif // OTA_HANDLER

  if (!client.connected())
  {
    long now = millis();
    if (now - lastReconnectAttempt > 5000)
    {
      lastReconnectAttempt = now;
      // Attempt to reconnect
      if (reconnect())
      {
        lastReconnectAttempt = 0;
      }
    }
  }
  else
  {
    // Client connected
    client.loop();
  }

  //read serial bms
  listenToBMS();

  if (newInfo == true)
  {
    switch (newCmd)
    {
    case rmvhex:
    {
      if (verifyChecksum)
      {
        byte prefixrmvframe[3] = {startbytehex, startbytehex, rmvhex};
        byte frameChecksum = cmd[(int)cmd[0] - 3 - 1]; //normalement 0F=15 et on enleve les 3 premiers non contenus dans cmd.
        if (!checkSum(frameChecksum, prefixrmvframe, 3))
        {
          debug_log("bad checksum rmv frame");
          break;
        }
      }
      char rmvOutput[rmvjsoncapacity];
      serializeJson(parseRMVToJson(), rmvOutput);
      debug_log("publishing rmv msg on topic:");
      debug_log(rmvtopic);
      client.publish(rmvtopic, rmvOutput);
      break;
    }
    case rcvhex:
    {
      if (verifyChecksum)
      {
        byte prefixrcvframe[3] = {startbytehex, startbytehex, rcvhex};
        byte frameChecksum = cmd[(int)cmd[0] - 3 - 1]; //normalement 0F=15 et on enleve les 3 premiers non contenus dans cmd.
        if (!checkSum(frameChecksum, prefixrcvframe, 3))
        {
          debug_log("bad checksum rcv frame");
          break;
        }
      }
      char rcvOutput[rcvjsoncapacity];
      serializeJson(parseRCVToJson(), rcvOutput);
      debug_log("publishing rcv msg on topic:");
      debug_log(rcvtopic);
      client.publish(rcvtopic, rcvOutput);
      break;
    }
    case rcihex:
    {
      if (verifyChecksum)
      {
        byte prefixrciframe[3] = {startbytehex, startbytehex, rcihex};
        byte frameChecksum = cmd[(int)cmd[0] - 3 - 1]; //normalement 0F=15 et on enleve les 3 premiers non contenus dans cmd.
        if (!checkSum(frameChecksum, prefixrciframe, 3))
        {
          debug_log("bad checksum rci frame");
          break;
        }
      }
      char rciOutput[rcijsoncapacity];
      serializeJson(parseRCIToJson(), rciOutput);
      debug_log("publishing rci msg on topic:");
      debug_log(rcitopic);
      client.publish(rcitopic, rciOutput);
      break;
    }
    default:
    {
      Serial.println("unknwn!");
    }
    }
    newCmd = 0x0;
    newInfo = false;
  }
}