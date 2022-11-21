//Librerias para trabajar con el servidor esp32
#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <StreamUtils.h>

//Librerias para trabajar con NFC
#include <Wire.h>
#include <SPI.h>
#include <UNIT_PN532.h>

//Libreria de tiempo
#include "RTClib.h"


#define PN532_SCK  (18)
#define PN532_MOSI (23)
#define PN532_SS   (5)
#define PN532_MISO (19)
#define PN532_IRQ   (2)
#define PN532_RESET (3) 

UNIT_PN532 nfc(PN532_SS);

uint8_t uid[] = { 0, 0, 0, 0, 0, 0, 0 };
boolean success;
uint8_t uidLength = 0;

const char* ssid     = "ESP32";
const char* password = "";

IPAddress apIP(192, 168, 1, 10);
IPAddress netMsk(255, 255, 255, 0);

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

//Variables para control de datos JSON
StaticJsonDocument<250> document;
StaticJsonDocument<250> doc;
StaticJsonDocument<250> fecha;

bool responseUser = true;
bool configTimeVariable = true;

char buffer[250];
String message = "";

//Variable de tiempo
RTC_DS3231 rtc;

//Variable deñ buzzer
#define BUZZER_PIN         4  // ESP32 pin GIOP04 connected to Buzzer's pin

 
void read_sensor_data(void * parameter) {

  //Configuracion de la fecha 
  if (!rtc.begin()) {
    Serial.println("Couldn't find RTC");
    while (1);
  }
   
  uint8_t keya[6] = { 0xD3, 0xF7, 0xD3, 0xF7, 0xD3, 0xF7 }, getData[16];
  String uidcard = "", data = "";
  
  for (;;) 
  {
    success = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength);  
    if (success) 
    {
      //Serial.println("Intentando autentificar bloque 4 con clave KEYA");
      success = nfc.mifareclassic_AuthenticateBlock(uid, uidLength, 4, 0, keya);  

      if (success)
      {
        //Serial.println("Sector 1 (Bloques 4 a 7) autentificados"); 
        success = nfc.mifareclassic_ReadDataBlock(4, getData);  

        if (success)
        {          
          //Serial.println("Datos leidos de sector 4:");
          //nfc.PrintHexChar(getData, 16);
          
          for (byte i = 0; i <= uidLength - 1; i++) {
            uidcard += (uid[i] < 0x10 ? "0" : "") + String(uid[i], HEX);
          }

          if(!doc.containsKey(uidcard)){

            
            DateTime now = rtc.now();

            String fecha = (String) now.day() + "-"+ (String) now.month() + "-"+ (String) now.year();
            String hora = (String) now.hour() + ":" + (String) now.minute() + ":" + (String) now.second();
            
            doc[uidcard] = fecha + " " + hora;

            EepromStream esp32(0,EEPROM.length());
            //Escribir o guardar en la eeprom la informacion 
            serializeJson(doc, esp32);
            esp32.flush();
            //Leer o actualizar la informacion del document principal
            deserializeJson(doc, esp32);

            //Carga el buffer con la nueva informacion 
            set_buffer();
            //Y es notificada a los socket conectados
            notifyUser();

            digitalWrite(BUZZER_PIN, HIGH); // turn on
            vTaskDelay(500 / portTICK_PERIOD_MS);      
            digitalWrite(BUZZER_PIN, LOW);  // turn off

          }
        
          uidcard = "";
          //Serial.println("");
                
        }
        else
        {
          //Serial.println("Fallo al leer tarjeta");
        }
      }
      else
      {
        //Serial.println("Fallo autentificar tarjeta");
      }
    }
  }
}

void setup_task() {    
  xTaskCreate(     
  read_sensor_data,      
  "Read sensor data",      
  3072,      
  NULL,      
  1,     
  NULL     
  );     
}

void onWsEvent(AsyncWebSocket * server, AsyncWebSocketClient * client, AwsEventType type, void * arg, uint8_t *data, size_t len){
  
  if(type == WS_EVT_CONNECT){
    Serial.println("Websocket client connection received");
    notifyUser();
  } else if(type == WS_EVT_DISCONNECT){
    Serial.println("Client disconnected");
  } else if(type == WS_EVT_DATA){
    handleWebSocketMessage(arg, data, len);
  }
}


void handleWebSocketMessage(void *arg, uint8_t *data, size_t len) {

  AwsFrameInfo *info = (AwsFrameInfo*)arg;
  
  if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
    
    data[len] = 0;
    message = (char*)data;
    
    if(responseUser){
    
      responseUser = false;
      
      if(doc.containsKey(message))
      {
        
        //Eliminar item del json doc
        doc.remove(message);

        EepromStream eeprom1(0,EEPROM.length());
        //Escribir
        serializeJson(doc, eeprom1);
        eeprom1.flush();
        
        //Leer
        deserializeJson(doc, eeprom1);
        //Carga el buffer con la nueva informacion 
        set_buffer();
        //Y es notificada a los socket conectados
        notifyUser();

      } 

      responseUser = true;  

    }
  }
}


void set_buffer(){

  JsonObject documentRoot = doc.as<JsonObject>();
	JsonArray array = document.to<JsonArray>();
  
  for(JsonPair keyValue : documentRoot){
    //Creating the nested object in the JsonArray.
    JsonObject nested = array.createNestedObject();
    
    //Writing the key:value pairs in the nested object.
    nested["uid"] = keyValue.key().c_str();
    nested["day"] = keyValue.value();	
  }

  serializeJson(array, buffer);

}

void notifyUser(){
  ws.textAll(buffer);
}

void setup() {     
  
  Serial.begin(115200);
  EEPROM.begin(512);
  
  WiFi.softAPConfig(apIP, apIP, netMsk);
  WiFi.softAP(ssid, password);

  IPAddress IP = WiFi.softAPIP();
  Serial.print("AP IP address: ");
  Serial.println(IP);
  Serial.println(WiFi.localIP());

  // Configurar para leer etiquetas RFID
  nfc.begin();
  nfc.setPassiveActivationRetries(0xFF);
  nfc.SAMConfig();

  //Inicia la tarea es decir un segundo ciclo loop que se encargara de la lectura de la tarjeta
  setup_task();    
 
  //Iniciar WebSocket   
  ws.onEvent(onWsEvent);
  server.addHandler(&ws);        
  server.begin();  

  EepromStream eepromStream(0,EEPROM.length());
  //Leer, extre la informacion de la eeprom y la carga en el doc principal json
  deserializeJson(doc, eepromStream); 
  set_buffer();

  //Configuracion del buzzer
  pinMode(BUZZER_PIN, OUTPUT); 

}    
       
void loop() {
 
}