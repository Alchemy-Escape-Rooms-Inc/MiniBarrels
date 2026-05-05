//================================================
//                 MACROS
//================================================

#include <WiFi.h>
#include <PubSubClient.h>
#include <HardwareSerial.h>
#include <SoftwareSerial.h>

#define VERSION "1.0.0"

#define GAME_NAME "MermaidsTale"
#define PROP_NAME "MiniBarrels"

#define MQTT_TOPIC_COMMAND  "MermaidsTale/MiniBarrels/command"
#define MQTT_TOPIC_STATUS   "MermaidsTale/MiniBarrels/status"
#define MQTT_TOPIC_LOG      "MermaidsTale/MiniBarrels/log"
#define MQTT_TOPIC_MESSAGE  "MermaidsTale/MiniBarrels/message"
#define MQTT_TOPIC_SYSTEM   "MermaidsTale/MiniBarrels/system"

#define MAX_CHAR 20
#define ID_LEN 13
#define TAG_LEN 16

#define S1_RFID_RX_PIN  4
#define S2_RFID_RX_PIN  5
#define S3_RFID_RX_PIN  6
#define S4_RFID_RX_PIN  7
#define S5_RFID_RX_PIN  15

//================================================
//            DATA STRUCTURE
//================================================
struct SPICE {
  String spice;
  byte uid[ID_LEN];

  SPICE():spice(""),uid(0,0,0,0,0,0,0,0,0,0,0,0,0){}
  SPICE(const String& vSpice, byte vUid[]): spice(vSpice){  
    for(byte i = 0; i < ID_LEN; i++)
      uid[i] = vUid[i];
  }
};

//================================================
//            GLOBAL VARIABLES
//================================================

WiFiClient espClient;
PubSubClient mqttClient(espClient);

//Hardware UART setup
HardwareSerial rfid1(0);
HardwareSerial rfid2(1);
HardwareSerial rfid3(2);

//Software UART setup
EspSoftwareSerial::UART rfid4;
EspSoftwareSerial::UART rfid5;



//Known spices and their IDs
const SPICE spices[5] = {
  SPICE("Cinnamon",(byte[]){0,0,0,0,0,0,0,0,0,0,0,0,0}),
  SPICE("Vanilla",(byte[]){0,0,0,0,0,0,0,0,0,0,0,0,0})
 // SPICE("",{}),
 // SPICE("",{}),
 // SPICE("",{})
};

// WiFi credentials
const char* WIFI_SSID = "AlchemyGuest";
const char* WIFI_PASS = "VoodooVacation5601";

// MQTT broker
const char* MQTT_SERVER = "10.1.10.115";
const int MQTT_PORT = 1883;


const unsigned long heartBeatPulse = 5 * 1000UL; //5 sec

unsigned long lastTime = 0;

byte newS1Tag[ID_LEN];
byte newS2Tag[ID_LEN];
byte newS3Tag[ID_LEN];
byte newS4Tag[ID_LEN];
byte newS5Tag[ID_LEN];

byte correctPlacementCount = 0;

bool puzzleSolved = false;

//================================================
//            NETWORK & MQTT 
//================================================
//WIFI NETWORK
void setupWiFi() {
  delay(1000);
  Serial.println("*********** WIFI ***********");
  Serial.print("Connecting to SSID: ");
  Serial.println(WIFI_SSID);

  WiFi.begin(WIFI_SSID,WIFI_PASS);

  while(WiFi.status() != WL_CONNECTED){
    delay(100);
    Serial.print("-");
  }
  Serial.println("\nConnected.");
}


//MQTT SERVER
void connectMQTT() {
  while (!mqttClient.connected()) {
    Serial.print("Connecting to MQTT...");

    String clientId = PROP_NAME;
    clientId += "_";
    clientId += String(random(0xffff), HEX);

    if (mqttClient.connect(clientId.c_str())) {
      Serial.println("connected!");

      // Subscribe to command topic
      mqttClient.subscribe(MQTT_TOPIC_COMMAND);

      // Announce we're online
      mqttClient.publish(MQTT_TOPIC_STATUS, "ONLINE");
      mqttLogf("%s v%s online", PROP_NAME, VERSION);

    } else {
      Serial.printf("failed (rc=%d), retrying in 5s\n", mqttClient.state());
      delay(5000);
    }
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  char topicBuf[128];
  strncpy(topicBuf,topic,sizeof(topicBuf)-1);
  topicBuf[sizeof(topicBuf)-1] = '\0';

  char message[128];
  if(length >= sizeof(message)){
    length = sizeof(message) - 1;
  }

  memcpy(message,payload,length);
  message[length] = '\0';

  char * msg = message;
  while(*msg == ' ' || *msg == '\r' || *msg == '\n')
    msg++;
  char * end = msg + strlen(msg) -1;
  while(end > msg && (*end == ' ' || *end == 't' || *end == '\r' || *end == '\n')){
    *end = '\0';
    end--;
  }

  Serial.printf("[MQTT] Received on %s: %s\n", topicBuf,msg);

  if(strcmp(topicBuf,MQTT_TOPIC_COMMAND) != 0){
    return;
  }

  if(strcmp(msg,"PING") == 0){
    mqttClient.publish(MQTT_TOPIC_COMMAND,"PONG");
    Serial.println("[MQTT] PING -> PONG");
    return;
  }
  if(strcmp(msg,"STATUS") == 0){
    const char* state = "READY";
    mqttClient.publish(MQTT_TOPIC_COMMAND,state);
    Serial.printf("[MQTT] STATUS -> %s\n",state);
    return;
  }
  if(strcmp(msg,"RESET") == 0){
    mqttClient.publish(MQTT_TOPIC_COMMAND,"OK");
    delay(100);
    ESP.restart();
    return;
  }
  Serial.printf("[MQTT] Unknown command: %s\n", msg);
}

void setupMQTT() {
  mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setBufferSize(512);  // Increase if needed
}

void mqttLogf(const char* format, ...) {
  char buffer[256];
  va_list args;
  va_start(args, format);
  vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);

  mqttClient.publish(MQTT_TOPIC_LOG, buffer);
  Serial.println(buffer);
}

void heartBeat(){
  unsigned long currentTime = millis();
  if(!(currentTime - lastTime > heartBeatPulse))
    return;
  lastTime = currentTime;
  // Announce we're online
  mqttClient.publish(MQTT_TOPIC_STATUS, "ONLINE");
  mqttLogf("%s v%s online", PROP_NAME, VERSION);
}

//================================================
//          GENERAL FUNCTIONS
//================================================

void mqttUIDLog(byte * newTag,const SPICE & spice, bool isValid){
  String out = "UID: ";
  String topic = String(MQTT_TOPIC_SYSTEM) + "/";
  
  topic += spice.spice;

  for(byte i = 0; i < ID_LEN; i++ )
    out += String(newTag[i],HEX);

  out += " Valid: ";
  out += ((isValid) ? "True":"False");
  mqttClient.publish(topic.c_str(),out.c_str());
}

bool isAMatchingID(const byte id1[], const byte id2[]) {
  for (byte i = 0; i < ID_LEN; i++)
    if (id1[i] != id2[i])
      return false;
  return true;
}

bool idValidation(byte id[], const SPICE & spice) {
  return (isAMatchingID(id, spice.uid)) ? true : false;
}

void listen(Stream & rSerial, byte * newTag, const SPICE & spice, bool & isValid){
  int readByte;
  int i = 0;
  bool tag = (rSerial.available() == TAG_LEN) ? true : false;

  if(!tag)
    return;
  
  while(rSerial.available()){
    readByte = rSerial.read();
    if (readByte != 2 && readByte!= 13 && readByte != 10 && readByte != 3)
      newTag[i++] = readByte;
  }
  
  //verify that the UID is a match
  isValid = idValidation(newTag,spice);
  delay(100);
}
void checkSuccess(){
  if(correctPlacementCount < 5)
    return;
  puzzleSolved = true;
  mqttClient.publish(MQTT_TOPIC_STATUS,"SOLVED");
}
void vanillaRFIDRead(){
  bool isValid; 
  listen(rfid1,newS1Tag,spices[0],isValid);
  mqttUIDLog(newS1Tag,spices[0],isValid);
  correctPlacementCount += (isValid) ? 1 : -1; 
  checkSuccess();
}
void cinnamonRFIDRead(){
  bool isValid; 
  listen(rfid2,newS2Tag,spices[1],isValid);
  mqttUIDLog(newS2Tag,spices[1],isValid);
  correctPlacementCount += (isValid) ? 1 : -1; 
  checkSuccess();
}
void spice3RFIDRead(){
  bool isValid; 
  listen(rfid3,newS3Tag,spices[2],isValid);
  mqttUIDLog(newS3Tag,spices[2],isValid);
  correctPlacementCount += (isValid) ? 1 : -1; 
  checkSuccess();
}
void spice4RFIDReaed(){
  bool isValid; 
  listen(rfid4,newS4Tag,spices[3],isValid);
  mqttUIDLog(newS4Tag,spices[3],isValid);
  correctPlacementCount += (isValid) ? 1 : -1; 
  checkSuccess();
}
void spice5RFIDRead(){
  bool isValid; 
  listen(rfid5,newS5Tag,spices[4],isValid);
  mqttUIDLog(newS5Tag,spices[4],isValid);
  correctPlacementCount += (isValid) ? 1 : -1; 
  checkSuccess();
}


void setupRFID(){
  Serial.begin(115200);

  rfid1.begin(9600,SERIAL_8N1,S1_RFID_RX_PIN,-1);
  rfid2.begin(9600,SERIAL_8N1,S2_RFID_RX_PIN,-1);
  rfid3.begin(9600,SERIAL_8N1,S3_RFID_RX_PIN,-1);
  rfid4.begin(9600,SWSERIAL_8N1,S4_RFID_RX_PIN,-1);
  rfid5.begin(9600,SWSERIAL_8N1,S5_RFID_RX_PIN,-1);
}

void _init(){
  //network setup
  setupWiFi();
  //mqtt setup
  setupMQTT();
  //rfid setup
  setupRFID();
}

void program(){
  if (!mqttClient.connected()) {
    connectMQTT();
  }
  mqttClient.loop();

  heartBeat(); 

  vanillaRFIDRead();
  cinnamonRFIDRead();
  spice3RFIDRead();
  spice4RFIDReaed();
  spice5RFIDRead();
}

//================================================
//               SETUP 
//================================================
void setup() {
  Serial.begin(115200);
  _init();
}

//================================================
//             MAIN LOOP 
//================================================
void loop() {
  program();
}

