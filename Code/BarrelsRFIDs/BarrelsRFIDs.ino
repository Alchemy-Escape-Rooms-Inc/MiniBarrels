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

#define MQTT_TOPIC          "MermaidsTale/MiniBarrels/"
#define MQTT_TOPIC_COMMAND  "MermaidsTale/MiniBarrels/command"
#define MQTT_TOPIC_STATUS   "MermaidsTale/MiniBarrels/status"
#define MQTT_TOPIC_LOG      "MermaidsTale/MiniBarrels/log"
#define MQTT_TOPIC_MESSAGE  "MermaidsTale/MiniBarrels/message"
#define MQTT_TOPIC_SYSTEM   "MermaidsTale/MiniBarrels/system"

#define MAX_CHAR 20
#define ID_LEN 12
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
  char uid[ID_LEN];

  SPICE():spice(""),uid(0,0,0,0,0,0,0,0,0,0,0,0){}
  SPICE(const String& vSpice, char vUid[]): spice(vSpice){
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
//const SPICE spices[5] = {
//  SPICE("Vanilla",(char[]){'5','1','0','0','0','A','F','0','9','A','3','1'}),
//  SPICE("Cloves",(char[]){'0','1','1','2','D','7','B','8','7','1','0','D'}),
//  SPICE("Molasses",(char[]){'5','1','0','0','0','C','7','4','F','A','D','3'}),
//  SPICE("Sugar Cane",(char[]){'5','1','0','0','0','D','0','1','5','F','0','2'}),
//  SPICE("Yeast",(char[]){'0','1','1','2','D','7','B','8','6','A','1','6'})
//};

const SPICE vanillaSpice = SPICE("Vanilla",(char[]){'5','1','0','0','0','D','9','0','A','F','6','3'});
const SPICE clovesSpice = SPICE("Cloves",(char[]){'0','1','1','2','D','7','B','8','7','1','0','D'});
const SPICE molassesSpice = SPICE("Molasses",(char[]){'5','1','0','0','0','C','7','4','F','A','D','3'});
const SPICE sugarCaneSpice = SPICE("SugarCane",(char[]){'5','1','0','0','0','D','0','1','5','F','0','2'});
const SPICE yeastSpice = SPICE("Yeast",(char[]){'0','1','1','2','D','7','B','8','6','A','1','6'});

// WiFi credentials
const char* WIFI_SSID = "AlchemyGuest";
const char* WIFI_PASS = "VoodooVacation5601";

// MQTT broker
const char* MQTT_SERVER = "10.1.10.115";
const int MQTT_PORT = 1883;


const unsigned long heartBeatPulse = 5 * 1000UL; //5 sec

unsigned long lastTime = 0;

char vanillaTag[ID_LEN];
char clovesTag[ID_LEN];
char molassesTag[ID_LEN];
char sugarCane[ID_LEN];
char yeastTag[ID_LEN];


bool puzzleSolved = false;
bool validPlacements[5] = {false, false, false,false, false};
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

void mqttUIDLog(char * newTag,const SPICE & spice, bool isValid){
  String out = "UID: ";
  String topic = String(MQTT_TOPIC_SYSTEM) + "/";

  topic += spice.spice;


  for(byte i = 0; i < ID_LEN; i++)
    out += String(spice.uid[i]);

  out += " Scanned: ";
  for(byte i = 0; i < ID_LEN; i++ )
    out += String(newTag[i]);
  
  out += " Valid: ";
  out += (isValid) ? "True":"False";
  mqttClient.publish(topic.c_str(),out.c_str());
  
  String sTopic = String(MQTT_TOPIC) + spice.spice;
  mqttClient.publish(sTopic.c_str(),(isValid) ? "True":"False");
}

bool isAMatchingID(const char id1[], const char id2[]) {
  for (byte i = 0; i < ID_LEN; i++)
    if (id1[i] != id2[i])
      return false;
  return true;
}

bool idValidation(char id[], const SPICE & spice) {
  return isAMatchingID(id, spice.uid);
}

void listen(Stream & rSerial, char * newTag, const SPICE & spice, byte index){
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

  //verify that the UID is a match and update placement status
  bool isValid  = idValidation(newTag,spice);
  validPlacements[index] = isValid;
  delay(10);

  mqttUIDLog(newTag,spice,isValid);
}

void clearTag(char * newTag){
  for(byte i = 0; i < ID_LEN; i++)
    newTag[i] = 0;
}

void checkSuccess(){
  for(byte i = 0; i < 5; i++)
    if(!validPlacements[i])
      return;

  puzzleSolved = true;
  mqttClient.publish(MQTT_TOPIC_STATUS,"SOLVED");
  delay(5000); //10sec delay after winning
}

void vanillaRFIDRead(){
  listen(rfid1,vanillaTag,vanillaSpice,0);
  clearTag(vanillaTag);
}
void clovesRFIDRead(){
  listen(rfid2,clovesTag,clovesSpice,1);
  clearTag(clovesTag);
}
void molassesRFIDRead(){
  listen(rfid3,molassesTag,molassesSpice,2);
  clearTag(molassesTag);
}
void sugarCaneRFIDRead(){
  listen(rfid4,sugarCane,sugarCaneSpice,3);
  clearTag(sugarCane);
}
void yeastRFIDRead(){
  listen(rfid5,yeastTag,yeastSpice,4);
  clearTag(yeastTag);
}


void setupRFID(){
  // Serial.begin(115200);

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
  clovesRFIDRead();
  molassesRFIDRead();
  sugarCaneRFIDRead();
  yeastRFIDRead();
  checkSuccess();
}

//================================================
//               SETUP
//================================================
void setup() {
  _init();
}

//================================================
//             MAIN LOOP
//================================================
void loop() {
  program();
}

