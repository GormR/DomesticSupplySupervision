/*---------------------------------------------------
HTTP 1.1 Temperature & Humidity Webserver for ESP8266 
for ESP8266 adapted Arduino IDE

(C) Gorm Rose 10/2020
GPL V3.0

Connect DHT21 / AMS2301 at GPIO2
---------------------------------------------------*/

#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
// #include "time_ntp.h"
#include "DHT.h"
#include <PubSubClient.h>  // PubSubClient by Nick ’O Leary
#include <ArduinoJson.h>
#include <WiFiClientSecure.h>
//#include <UniversalTelegramBot.h>
#include <ArduinoOTA.h>

// WiFi connection
const char* ssid = "xxxxxxxx";
const char* password = "xxxxxxxxxxxxxxxxxxxxxxxx";

// TelegramBot initialisieren
#define botToken "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"  // den Bot-Token bekommst du vom Botfather) TrinkwasserBot
//Deine User ID
#define userID "xxxxxxxxx"  // /getid @ IDBot

//#define _divWaterCnt 391.324    //  396 pulses per l   (given by Chinaman)
#define shutoffWaterFlow 39132  // switch water off when this volume is drawn continously
#define shutoffWaterTime 300    // switch water off when water is drawn continously this time in 4s steps
#define divWaterTemp 7          // 1/K
#define offsetWaterTemp 80
#define divWaterPress 1.01      // 1/kPa
#define offsetWaterPress 168

#define MQTTclient "Trinkwasseranschluss_1"
#define mqtt_server "192.168.0.100"
#define mqtt_user "user"
#define mqtt_password "xxxxxxxxx"
#define humidity_topic "tele/tasmota/Humidity"
#define temperature_topic "tele/tasmota/Temperature"
#define pressure_topic "tele/tasmota/Pressure"
#define stroke0_topic "tele/tasmota/Fade"
#define stroke1_topic "tele/tasmota/Dimmer"
#define consumption_topic "tele/tasmota/Saturation"
#define reconnect_topic "tele/tasmota/Red"
//"tele/tasmota/Humidity"

// ntp timestamp
// unsigned long ulSecs2000_timer=0;
// const char* ntpServer = "pool.ntp.org";
// const long  gmtOffset_sec = 0;
// const int   daylightOffset_sec = 3600;
 

// storage for Measurements; keep some mem free; allocate remainder
#define KEEP_MEM_FREE 15240
#define MEAS_SPAN_H 168

unsigned long ulMeasDelta_ms;         // distance to next meas time
unsigned long ulLastVolMeas_ms = 500;   // last volume check time
unsigned long ulMyMillis = 0;
uint32_t      uiVolCnt = 0;           // for water
unsigned short ucTimeOfStrokeTest = 0;
uint8_t       uiStrokeState = 0;
uint32_t      uiLastVolCnt = 0;
float         fWatPressStroke = 0;
uint32_t      ulStrokePulses = 0;
uint16_t      ui4Seconds = 14 * 60 * 15 + 220; // counter for 4 seconds during the day

float         fAirTemp=0,fHum=0;          // temperature and humidity measurements
float         fWatTemp=0,fWatPress=0;     // water temperature and pressure
float         fWatPressRaw=0, fWatPressRawSum=0;
float         fWatPressMin=1000, fWatPressMax=0; 
float         fAirTempSum=0;          // temperature measurements low pass filter
float         fAirTempLast=16;        // to suppress errors only
float         fHumSum=0;              // humidity measurements low pass filter
float         fWatTempSum=0;          // water temperature low pass filter
float         fWatPressSum=0;         // water pressure low pass filter
unsigned int  uiNoST=0;               // Number of samples
unsigned int  uiNoSH=0;               // Number of samples
unsigned int  uiNoSTP=0;              // Number of samples
unsigned int  uiNoSRaw=0;             // Number of samples
unsigned int  uiWaterFlowTime=0;      // Counting seconds of water flow
uint32_t      uiWaterFlowVol=0;       // Counting volume of water flow
uint32_t      uiWaterFlow15=0;        // Counting volume of water flow within time interval

unsigned long ulReconncount;          // how often did we connect to WiFi
unsigned long ulLastSec_ms = 0;
uint8_t       forcedClose = 0;        // force closing via MQTT
uint8_t       forcedOpen = 0;         // force opening via MQTT

// Assign Arduino Friendly Names to GPIO pins
#define analogInPin  A0     // ESP8266 Analog Pin ADC0 = A0
#define D0 16
#define D1 5
#define D2 4
#define D3 0
#define D4 2
#define D5 14
#define D6 12
#define D7 13
#define D8 15

#define oSel      D0
#define iFlow     D5              // interrupt pin
#define i_Running D6
#define oClose     D7
#define oRelActive    D8

#define ValveRunDuration 8       // in 4s steps
#define StrokeDuration  60

// Create an instance of the server on Port 80
WiFiServer server(80);

//////////////////////////////
// DHT21 / AMS2301 is at GPIO2
//////////////////////////////
#define DHTPIN 2

// Uncomment whatever type you're using!
//#define DHTTYPE DHT11   // DHT 11 
//#define DHTTYPE DHT22   // DHT 22  (AM2302)
#define DHTTYPE DHT21   // DHT 21 (AM2301)

// init DHT; 3rd parameter = 16 works for ESP8266@80MHz
DHT dht(DHTPIN, DHTTYPE, 16); 

// MQTT
WiFiClient espClient;
PubSubClient client(espClient);

//WiFiClientSecure Tclient;
//UniversalTelegramBot bot(botToken, Tclient);

// needed to avoid link error on ram check
extern "C" 
{
#include "user_interface.h"
}

/////////////////////
// this one is handling GPIO interrupt for volume pulse counting
/////////////////////
ICACHE_RAM_ATTR void handleInterrupt() 
{
  if (((digitalRead(iFlow) == HIGH) && (uiVolCnt & 1)) || ((digitalRead(iFlow) == LOW) && !(uiVolCnt & 1)))
  {
    uiVolCnt++;
  }
  // Serial.print("x"); // never print here.. hahaha
}


/////////////////////
// MQTT callback
/////////////////////
void callback(char* topic, byte* payload, unsigned int length) 
{
  Serial.print("Message arrived in topic: ");
  Serial.println(topic);
  Serial.print("Message:");
  for (int i = 0; i < length; i++) 
    Serial.print((char)payload[i]);
  Serial.println();
  Serial.println("-----------------------");
  if (strcmp(topic, "cmnd/tasmota/POWER") == 0)
  {
    if (payload[1] == 70)  // 70 = 1st "F" of "OFF"
    {
      forcedClose = 1; 
      forcedOpen = 0;
      Serial.print("Closing valve");
    }
    else
    {
      forcedOpen = 1; 
      forcedClose = 0;
      Serial.print("Opening valve");
    }
  }
  if (strstr(topic, "cmnd/tasmota/StrokeTest"))
    ucTimeOfStrokeTest = 1; // start stroke test
}

/////////////////////
// the setup routine
/////////////////////
void setup() 
{
  // pin setup
  pinMode(oSel, OUTPUT);        // Selects water temp (==0) or pressure (==1) for analog input
  digitalWrite(oSel, HIGH);
  pinMode(oClose, OUTPUT);       // motor opens valve
  digitalWrite(oClose, LOW);
  pinMode(oRelActive, OUTPUT);      // motor closes valve
  digitalWrite(oRelActive, LOW);
  pinMode(i_Running, INPUT);    // feedback (motor current)
  pinMode(iFlow, INPUT_PULLUP); // set interrupt handler for volume pulse counting
  attachInterrupt(digitalPinToInterrupt(iFlow), handleInterrupt, CHANGE);
  
  // setup globals
  ulReconncount=0;
    
  // start serial
  Serial.begin(115200);
  Serial.println(); 
  Serial.println(); 
  Serial.println("WLAN Temperatur und Feuchtigkeitslogger - Gorm Rose 10/2020");
  
  // inital connect
  WiFi.mode(WIFI_STA);
  WiFi.hostname("temphum");
  WiFiStart();
  WiFi.setAutoReconnect(true);
  WiFi.persistent(true);
  
  Serial.println("");
  Serial.print("Connected to ");
  Serial.println(ssid);
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  // Port defaults to 8266
  // ArduinoOTA.setPort(8266);

  // Hostname defaults to esp8266-[ChipID]
  ArduinoOTA.setHostname("esp8266-Trinkwasser");

  // No authentication by default
  // ArduinoOTA.setPassword("admin");

  // Password can be set with it's md5 value as well
  // MD5(admin) = 21232f297a57a5a743894a0e4a801fc3
  // ArduinoOTA.setPasswordHash("21232f297a57a5a743894a0e4a801fc3");

  ArduinoOTA.onStart([]() {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH) {
      type = "sketch";
    } else { // U_FS
      type = "filesystem";
    }

    // NOTE: if updating FS this would be the place to unmount FS using FS.end()
    Serial.println("Start updating " + type);
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\nEnd");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) {
      Serial.println("Auth Failed");
    } else if (error == OTA_BEGIN_ERROR) {
      Serial.println("Begin Failed");
    } else if (error == OTA_CONNECT_ERROR) {
      Serial.println("Connect Failed");
    } else if (error == OTA_RECEIVE_ERROR) {
      Serial.println("Receive Failed");
    } else if (error == OTA_END_ERROR) {
      Serial.println("End Failed");
    }
  });
  ArduinoOTA.begin();

  //////////////////////////////////////////////////////////////

  client.setServer(mqtt_server, 1883);
  client.setCallback(callback);

//  Tclient.setInsecure();
//  Serial.print("Bot "); 
//  bot.sendMessage(userID, "Neustart Trinkwasserüberwachung", "");
//  Serial.println("done."); 

  ulLastSec_ms = millis();
}


///////////////////
// (re-)start WiFi
///////////////////
void WiFiStart()
{
  ulReconncount++;
  
  // Connect to WiFi network
  Serial.println();
  Serial.println();
  Serial.print("Connecting to ");
  Serial.println(ssid);
  
  WiFi.begin(ssid, password);
  
  while (WiFi.status() != WL_CONNECTED) 
  {
    delay(500);
    Serial.print(".");
  }
  Serial.println("");
  Serial.println("WiFi connected");
  
  // Start the server
  server.begin();
  Serial.println("Server started");

  // Print the IP address
  Serial.println(WiFi.localIP());
  
  ///////////////////////////////
  // connect to NTP and get time
  ///////////////////////////////
//  ulSecs2000_timer=getNTPTimestamp();
//  Serial.print("Current Time UTC from NTP server: " );
//  Serial.println(epoch_to_string(ulSecs2000_timer).c_str());

//  ulSecs2000_timer -= millis()/1000;  // keep distance to millis() counter
//  ulLastSec_ms = millis();
}

///////////////////
// (re-)start MQTT
///////////////////
void reconnect() 
{
  // Loop until we're reconnected
  while (!client.connected()) {
    Serial.print("Attempting MQTT connection...");
    // Attempt to connect
    // If you do not want to use a username and password, change next line to
    // if (client.connect("ESP8266Client")) {
    if (client.connect(MQTTclient, mqtt_user, mqtt_password)) 
    {
      Serial.println("connected");
//      client.publish("stat/tasmota/POWER", "ON");
//      client.publish("tele/tasmota/Humidity", "99");
//      client.publish("tele/tasmota/SENSOR", "{\"Time\":\"2017-02-16T10:13:52\", \"DS18B20\":{\"Temperature\":\"20.6\"}}");
      // ... and resubscribe
      client.subscribe("inTopic");
    } 
    else 
    {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" try again in 5 seconds");
      // Wait 5 seconds before retrying
      delay(5000);
    }
  }
}

/////////////
// main loop
/////////////
void loop() 
{
  ArduinoOTA.handle();

  //////////////////////////////////////////////////////////////

  //////////////////////////////
  // check if WLAN is connected
  //////////////////////////////
  if (WiFi.status() != WL_CONNECTED)
  {
  //  WiFiStart();
  }

  //////////////////////////////
  // check if MQTT is connected
  //////////////////////////////
  if (!client.connected()) reconnect();
  client.loop();

  ///////////////////
  // do data logging
  ///////////////////
  
  ///// 4s timer /////
  ulMyMillis = millis();
  if ((ulMyMillis - ulLastSec_ms) < 4000)   // get pressure while waiting one second
  {
    // sample pressure on idle    
    digitalWrite(oSel, HIGH);
    delay(1);
    fWatPressRaw = (analogRead(analogInPin) - offsetWaterPress) / divWaterPress;
    if (fWatPressRaw < fWatPressMin) fWatPressMin = fWatPressRaw;
    if (fWatPressRaw > fWatPressMax) fWatPressMax = fWatPressRaw;
    fWatPressRawSum += fWatPressRaw;
    uiNoSRaw += 1;
  }
  else // 4s seconds over
  {
    ulLastSec_ms += 4000;
    ui4Seconds += 1;

    digitalWrite(oSel, LOW);
    delay(1);
    fWatTemp = (analogRead(analogInPin) - offsetWaterTemp) / divWaterTemp;
    digitalWrite(oSel, HIGH);
    delay(1);
    fWatTempSum += fWatTemp;
    fWatPress = 555;
    if (uiNoSRaw) fWatPress = fWatPressRawSum / uiNoSRaw;  // mean value of last second
    fWatPressSum += fWatPress;
    uiNoSTP += 1;
    uiNoSRaw = 0;
    fWatPressRawSum = 0;
    
    if (forcedOpen)
    {
      if (forcedOpen < ValveRunDuration)
      {
        forcedOpen++;
        digitalWrite(oClose, LOW);
        digitalWrite(oRelActive, HIGH);
        Serial.print("."); 
      }
      else
      {
        digitalWrite(oClose, LOW);
        digitalWrite(oRelActive, LOW);
        Serial.println("opened");
        client.publish("stat/tasmota/POWER", "ON");
        forcedOpen = 0; 
      }
    }
    if (forcedClose)
    {
      if (forcedClose < ValveRunDuration)
      {
        digitalWrite(oClose, HIGH);
        digitalWrite(oRelActive, HIGH);
        Serial.print("."); 
        forcedClose++;
//        if ((forcedClose == 1) && (uiWaterFlowTime > shutoffWaterTime)) bot.sendMessage(userID, "Wassersperre (> Maximalzeit)!", "");
//        if ((forcedClose == 1) && (uiWaterFlowVol  > shutoffWaterFlow)) bot.sendMessage(userID, "Wassersperre (> Maximalmenge)!", "");
      }
      else
      {
        if (forcedClose == ValveRunDuration)
        {
          digitalWrite(oClose, LOW);
          digitalWrite(oRelActive, LOW);
          Serial.println("closed due to MQTT or ongoing water consumption"); 
          client.publish("stat/tasmota/POWER", "OFF");
        }
      }
    }
    else switch (uiStrokeState)
    {
      case 0:  // no test, normal operation
        
        if (uiLastVolCnt == uiVolCnt)    // no water drawn
        {
          if (uiWaterFlowVol > 2)            // send every consumption as a single packet
          {
            StaticJsonDocument<200> doc, subdoc, subdoc1;
            subdoc["Data"]       = String(uiWaterFlowVol);
            subdoc1["Data"]      = String(uiWaterFlowTime);
            
            doc["Water"]    = subdoc;
            doc["WaterMax"] = subdoc1;
            char output[200];
            serializeJson(doc, output);
            client.publish("tele/tasmota/SENSOR", output);
            Serial.println(output);
          }
          uiWaterFlowVol = 0;
          uiWaterFlowTime = 0; 
        }
        else // water is drawn
        {
          uiWaterFlowTime += 1;
          uiWaterFlowVol += uiVolCnt - uiLastVolCnt;
          uiLastVolCnt = uiVolCnt;
        }
        if ((uiWaterFlowTime > shutoffWaterTime)  || (uiWaterFlowVol > shutoffWaterFlow))  // shutoff if water is drawn continously
        {
          forcedClose = 1;  // close after 20min continous water flow
        }
        
        // measure room temp/hum every 4s
        fHum = dht.readHumidity();
        fAirTemp = dht.readTemperature();
        if ((fHum > 10) && (fHum < 100))
        {
          fHumSum += fHum;
          uiNoSH += 1;
        }
        if ((fAirTemp > 4) && (fAirTemp < 40) && (fAirTemp > (fAirTempLast - 2))) 
        {
          fAirTempSum += fAirTemp;
          uiNoST += 1;
        }
        Serial.print("Temperature: "); 
        Serial.print(fAirTemp);
        Serial.print(" deg Celsius - Humidity: "); 
        Serial.print(fHum);
        Serial.print("% - Pressure: "); 
        Serial.print(fWatPress);
        Serial.print("kPa - WaterTemp: "); 
        Serial.print(fWatTemp);
        Serial.print("deg Celsius - Consumption: "); 
        Serial.println((uint16_t)uiVolCnt);
            
        if ((ui4Seconds % 225) == 224)      // 15min = 900s = 4s x 225 over?
        {
          uiWaterFlow15 = uiVolCnt - uiWaterFlow15;
          StaticJsonDocument<400> doc, subdoc, subdoc1, subdoc2, subdoc3;
          if (uiNoST) 
          {
            fAirTempLast = fAirTempSum/uiNoST;                    
            subdoc["Temperature"] = String(fAirTempLast);
          }
          if (uiNoSH)  subdoc["Humidity"]     = String(fHumSum/uiNoSH);      
          if (uiNoSTP) subdoc1["Temperature"] = String(fWatTempSum/uiNoSTP);  
          if (uiNoSTP) subdoc1["Pressure"]    = String(fWatPressSum/uiNoSTP);
    //      subdoc1["Data"]       = String(uiWaterFlow15);
          subdoc2["Pressure"]   = String(fWatPressMax);
          subdoc3["Pressure"]   = String(fWatPressMin);
    
          doc["AM2301"]   = subdoc;
          doc["Water"]    = subdoc1;
          doc["WaterMax"] = subdoc2;
          doc["WaterMin"] = subdoc3;
          char output[400];
          serializeJson(doc, output);
          client.publish("tele/tasmota/SENSOR", output);
          Serial.println(output);
              
          // Re-init low pass filter
          fAirTempSum  = 0;         // temperature measurements low pass filter
          uiNoST  = 0;
          fHumSum      = 0;         // humidity measurements low pass filter
          uiNoSH  = 0;
          fWatTempSum  = 0;         // water temperature low pass filter
          fWatPressSum = 0;         // water pressure low pass filter
          uiNoSTP = 0;
          fWatPressMin  = fWatPress;
          fWatPressMax  = fWatPress;
          uiWaterFlow15 = uiVolCnt;
        }   
        
        if ((ui4Seconds >= 21600) && not uiWaterFlowVol)  // one day over and no comsuption right now?
        {
          Serial.println("Starting stroke test (State = 1).");
          ui4Seconds -= 21600;
          
          digitalWrite(oClose, HIGH);
          delay(10);
          digitalWrite(oRelActive, HIGH);
            
          client.publish("stat/tasmota/POWER", "OFF");
  //        bot.sendMessage(userID, "Tagesverbrauch: " + String((float)uiVolCnt/496) + "l", "");
          ucTimeOfStrokeTest = 0;
          
          uiStrokeState = 1;
          Serial.println("Close valve and start test (State = 1)...");
        } 
        break;
      
      case 1:  // wait for closed; stop valve, init measurement
        ucTimeOfStrokeTest++;
        if (ucTimeOfStrokeTest == ValveRunDuration)  // valve closed => release relay, store inital pressure
        {
          digitalWrite(oRelActive, LOW);
          delay(10);
          digitalWrite(oClose, LOW);
          uiVolCnt = 0;
          fWatPressStroke = -fWatPress;  // read ref value
          client.publish("stat/tasmota/POWER", "OFF");

          uiStrokeState = 2;
          Serial.println("Valve closed. Set ref value and wait (State = 2)...");
        }
        break;

      case 2:
        ucTimeOfStrokeTest++;
        if (ucTimeOfStrokeTest == (StrokeDuration - ValveRunDuration))  // off time over?
        {
          fWatPressStroke += fWatPress;  // result
          digitalWrite(oClose, LOW);
          digitalWrite(oRelActive, HIGH);

          uiStrokeState = 3;
          Serial.println("Time elapsed, get measurement value and wait for open (State = 3)...");
        }
        break;

      case 3:
        ucTimeOfStrokeTest++;
        if (ucTimeOfStrokeTest == StrokeDuration)  // test done?
        {
          ulStrokePulses = uiVolCnt;
          digitalWrite(oClose, LOW);
          digitalWrite(oRelActive, LOW);
          
          Serial.println(); 
          Serial.print("Pulses: "); 
          Serial.println(ulStrokePulses);
          Serial.print("Delta pressure: "); 
          Serial.println(fWatPressStroke);
    
          StaticJsonDocument<300> doc, subdoc;
          subdoc["Data"] = String(ulStrokePulses);
          subdoc["Pressure"] = String(fWatPressStroke);
          doc["StrokeTest"] = subdoc;
          char output[300];
          serializeJson(doc, output);
          client.publish("tele/tasmota/SENSOR", output);
          client.publish("stat/tasmota/POWER", "ON");
          Serial.println(output);
  
//          bot.sendMessage(userID, "Stroke Test Pulse: " + String(uiVolCnt), "");
          fWatPressSum = 0;
          fWatTempSum = 0;
          uiNoSTP = 0;
          uiVolCnt = 0;
          uiLastVolCnt = 0;
          uiWaterFlow15 = 0;

          uiStrokeState = 0;
          Serial.println("Stroke test done and values sent (State = 0).");
        }
        break;

      default:
        uiStrokeState = 0;
        Serial.println("case.. default... Something went wrong...");
        break;  
    }
  }
}
