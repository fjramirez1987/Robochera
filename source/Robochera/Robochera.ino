/*
 * 
 * Programa: Robochera
 * Versión: 0.3.2
 * Autor: FjRamírez
 * Fecha: 05/09/2020
 * Web: https://tuelectronica.es/
 *
 *
 * Acceso web
 * User: admin
 * Password: AP password
 */

// ***** CONFIGURACIÓN DE PINES *****
#define DI_PIN 5 // Pin DI de la tira led WS2812b (GPIO5)
#define SERVOL_PIN 4 // Pin data servo izquierdo (GPIO4)
#define SERVOR_PIN 18 // Pin data servo derecho (GPIO2)
#define CONFIG_PIN 15 // Pin para resetear la contraseña del AP (GPIO15)
#define STATUS_PIN 2 // pin led de estado (GPIO18)

// ***** CONFIGURACIÓN DE PARÁMETROS *****
#define NUMPIXELS 16 // Número de leds
#define FIRMWARE_VERSION "0.3.2" // Versión del Firmware


#include <MQTT.h>
#include <IotWebConf.h>
#include <ESP32Servo.h>
#include <FastLED.h>
#include <ArduinoJson.h>

unsigned long timer1 = 0; // Temporizador para luces
unsigned long timer2 = 0; // Temporizador para puerta
unsigned long timer3 = 0; // Temporizador para enviar el estado
unsigned long timer4 = 0; // Temporizador para reescribir el valor del servo

unsigned int frequency = 1000;
unsigned int beeps = 10;

String ledEffect = "none"; // Efecto de la tira led
boolean ledState = false; // Estado de la tira led
boolean deviceAdded = false; // Dispositivo añadido a HA

int i=0;
int offset = -1; // Compensacion de servo izquierdo.
CRGB color = CRGB( 90, 210, 0); // GRB

CRGB strip[NUMPIXELS];

// Nombre inicial del dispositivos. Se usa por ejemplo para el SSID del punto de acceso
const char thingName[] = "Robochera";

// Contraseña inicial para el punto de acceso (AP) y conexión al dispositivo mediante su IP (User: admin)
const char wifiInitialApPassword[] = "robochera32";

#define STRING_LEN 128 // Tamaño máximo para los parametros strings
#define NUMBER_LEN 32 // Tamaño máximo para los parametros numéricos


// -- Callback method declarations.
void wifiConnected();
void configSaved();
boolean formValidator();
void mqttMessageReceived(String &topic, String &payload);

DNSServer dnsServer;
WebServer server(80);
HTTPUpdateServer httpUpdater;
WiFiClient net;
MQTTClient mqttClient(1024); // 1024 bytes tamaño de mensaje

char mqttServerValue[STRING_LEN];
char mqttUserNameValue[STRING_LEN];
char mqttUserPasswordValue[STRING_LEN];
char openParamValue[NUMBER_LEN] = "180";
char openSpeedParamValue[NUMBER_LEN] = "15";
char closedParamValue[NUMBER_LEN] = "0";
char closedSpeedParamValue[NUMBER_LEN] = "15";

IotWebConf iotWebConf(thingName, &dnsServer, &server, wifiInitialApPassword, FIRMWARE_VERSION);
IotWebConfParameter mqttServerParam = IotWebConfParameter("Servidor MQTT", "mqttServer", mqttServerValue, STRING_LEN);
IotWebConfParameter mqttUserNameParam = IotWebConfParameter("Usuario MQTT", "mqttUser", mqttUserNameValue, STRING_LEN);
IotWebConfParameter mqttUserPasswordParam = IotWebConfParameter("Contraseña MQTT", "mqttPass", mqttUserPasswordValue, STRING_LEN);
// -- Separador
IotWebConfSeparator separator1 = IotWebConfSeparator("Configuración");
IotWebConfParameter openParam = IotWebConfParameter("Ángulo abierto", "openParam", openParamValue, NUMBER_LEN, "number", "", NULL, "min='0' max='180' step='1'");
IotWebConfParameter openSpeedParam = IotWebConfParameter("Velocidad para abrir (Max. 0 - Min. 100)", "openSeedParam", openSpeedParamValue, NUMBER_LEN, "number", "Max. 0 - Min. 100", NULL, "min='0' max='100' step='1'");
IotWebConfParameter closedParam = IotWebConfParameter("Ángulo cerrado", "closedParam", closedParamValue, NUMBER_LEN, "number", "", NULL, "min='0' max='180' step='1'");
IotWebConfParameter closedSpeedParam = IotWebConfParameter("Velocidad para cerrar (Max. 0 - Min. 100)", "closedSeedParam", closedSpeedParamValue, NUMBER_LEN, "number", "Max. 0 - Min. 100", NULL, "min='0' max='100' step='1'");

boolean needMqttConnect = false;
boolean needReset = false; // Variable para determinar si es necesario reiniciar
int pinState = HIGH;
unsigned long lastMqttConnectionAttempt = 0;
Servo servoLeft;  // create servo object to control a servo
Servo servoRight;  // create servo object to control a servo

int value=0; // Difine el ángulo de abertura de la puerta
int valuePre=0; // Valor anterior

void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println("Iniciando...");

  FastLED.addLeds<WS2812B, DI_PIN, RGB>(strip, NUMPIXELS);  // GRB ordering is typical
  FastLED.clear(); // Set all pixel colors to 'off'
  FastLED.show();

  servoLeft.attach(SERVOL_PIN); // Objeto servo izquierdo
  servoRight.attach(SERVOR_PIN); // Objeto servo derecho
  servoLeft.write(0); // NO SE SI FUNCIONA
  servoRight.write(180-0); // NO SE SI FUNCIONA

  iotWebConf.setStatusPin(STATUS_PIN);
  iotWebConf.setConfigPin(CONFIG_PIN);
  iotWebConf.addParameter(&mqttServerParam);
  iotWebConf.addParameter(&mqttUserNameParam);
  iotWebConf.addParameter(&mqttUserPasswordParam);
  iotWebConf.addParameter(&separator1); // Separador
  iotWebConf.addParameter(&openParam);
  iotWebConf.addParameter(&openSpeedParam);
  iotWebConf.addParameter(&closedParam);
  iotWebConf.addParameter(&closedSpeedParam);
  
  iotWebConf.setConfigSavedCallback(&configSaved);
  iotWebConf.setFormValidator(&formValidator);
  iotWebConf.setWifiConnectionCallback(&wifiConnected);
  iotWebConf.setupUpdateServer(&httpUpdater);

  // Incicializa la configuración:
  boolean validConfig = iotWebConf.init();
    if (!validConfig){
      mqttServerValue[0] = '\0';
      mqttUserNameValue[0] = '\0';
      mqttUserPasswordValue[0] = '\0';
    }

  // -- Set up required URL handlers on the web server.
  server.on("/", handleRoot);
  server.on("/add", register_homeassistantHTML);
  server.on("/config", []{ iotWebConf.handleConfig(); });
  server.onNotFound([](){ iotWebConf.handleNotFound(); });

  mqttClient.begin(mqttServerValue, net);
  mqttClient.onMessage(mqttMessageReceived);
  
  Serial.println("Ready!");

  
}

void loop(){
  // -- doLoop should be called as frequently as possible.
  iotWebConf.doLoop();
  mqttClient.loop();
  
    if (needMqttConnect){ // Si necesitamos conectarnos al servidor MQTT:
      if (connectMqtt()){ // Si nos hemos conectado al servidor MQTT:
        needMqttConnect = false; // Ya no es necesario volver a conectarse al servidor MQTT
      }
    } else if ((iotWebConf.getState() == IOTWEBCONF_STATE_ONLINE) && (!mqttClient.connected())){ // Si necesitamos volver a conectarnos al servidor MQTT:
      Serial.println("Reconectando MQTT.");
      connectMqtt(); // Nos conectamos al servidor MQTT.
    }

  if (needReset){ // Si es necesario reiniciar:
    Serial.println("Reiniciando...");
    iotWebConf.delay(1000); // Esperamos un segundo
    ESP.restart(); // Reiniciamos el dispositivo
  }

// *****

      // Abrir/Cerrar puerta:
      if(valuePre < value){ // Si tenemos que abrir la puerta:
        openDoor(atoi(openSpeedParamValue));
      } else if ( valuePre > value) { // Si tenemos que cerrar la puerta
        closeDoor(atoi(closedSpeedParamValue));
      } /*else if (( valuePre == value) && (millis()>= (timer4 + 10))){
        servoLeft.write(valuePre + offset);
        servoRight.write(180-valuePre);
        timer4 = millis();
      } */

      // Tira led:
      if (ledState){
        if (ledEffect == "opendoor"){
          color = CRGB( 90, 210, 0);
          colorGoOut(100); // 80
        } else if (ledEffect == "closeddoor"){
          color = CRGB( 90, 210, 0);
          colorGoIn(100); // 80
        } else if (ledEffect == "opendoor2"){
          color = CRGB( 190, 0, 0); // Verde
          colorGoOut(100); // 80
        } else if (ledEffect == "colorblink"){
          color = CRGB( 90, 210, 0); // Naranja
          colorBlink(1000);
        }
      }

    // Enviar estado cada 10s:
    if (millis()>= (timer3 + 10000)){
      timer3 = millis();
      sendStatus();
    }

// *****
     
}

/**
 * Handle web requests to "/" path.
 */
void handleRoot(){
  // -- Let IotWebConf test and handle captive portal requests.
  if (iotWebConf.handleCaptivePortal())
  {
    // -- Captive portal request were already served.
    return;
  }
  String s = "<!DOCTYPE html><html lang=\"es\"><head><meta charset=\"UTF-8\"><meta name=\"viewport\" content=\"width=device-width, initial-scale=1, user-scalable=no\"/>";
  s += "<title>Robochera</title></head><body>";
  s += "Configuración WIFI";
  s += "<ul>";
  s += "<li>SSID: ";
  s += iotWebConf.getWifiSsidParameter()->valueBuffer;
  s += "</ul>";
  s += "Configuración MQTT";
  s += "<ul>";
  s += "<li>MQTT server: ";
  s += mqttServerValue;
  s += "<li>MQTT usuario: ";
  s += mqttUserNameValue;
  s += "<li>MQTT contraseña: ";
  s += mqttUserPasswordValue;
  s += "</ul>";
  s += "Configuración";
  s += "<ul>";
  s += "<li>Ángulo abierto: ";
  s += openParamValue;
  s += "<li>Velocidad para abrir (Max. 0 - Min. 100): ";
  s += openSpeedParamValue;
  s += "<li>Ángulo cerrado: ";
  s += closedParamValue;
  s += "<li>Velocidad para cerrar (Max. 0 - Min. 100): ";
  s += closedSpeedParamValue;
  s += "</ul>";
  s += "<a href='config'>Configuración</a>";
  s += "</body></html>\n";

  server.send(200, "text/html", s);
}

// Callback cuando nos conectamos a la wifi:
void wifiConnected(){
  needMqttConnect = true;
}

// Guardar configuración:
void configSaved(){
  Serial.println("Configuration was updated.");
  needReset = true; // Necesario un reinicio para aplicar los cambios
}

// Validar formulario de configuración:
boolean formValidator(){
  Serial.println("Validating form.");
  boolean valid = true;

  int lengthParam = server.arg(mqttServerParam.getId()).length(); // Longitud del parámetro mqttServerParam
  
    if (lengthParam < 3){ // Si es menor de 3 carácteres:
      mqttServerParam.errorMessage = "Formato incorrecto.";
      valid = false;
    }
    
  return valid;
}

// Conexion con el servidor MQTT:
boolean connectMqtt(){
  unsigned long now = millis();

    // No repetir hasta pasados 1 segundo:
    if (1000 > now - lastMqttConnectionAttempt){
      return false;
    }
  
  Serial.println("Connecting to MQTT server...");
  
    if (!connectMqttOptions()) { // Si no conseguimos conectar con el servidor MQTT:
      lastMqttConnectionAttempt = now;
      return false;
    }
    
  Serial.println("Connected!");

  // Sibscripción a temas:
  mqttClient.subscribe("robochera/value");
  Serial.println("Subscribed to topic: robochera/value");
  mqttClient.subscribe("homeassistant/switch/robochera/set");
  Serial.println("Subscribed to topic: homeassistant/switch/robochera/set");
  mqttClient.subscribe("homeassistant/light/robochera/set");
  Serial.println("Subscribed to topic: homeassistant/light/robochera/set");
  mqttClient.subscribe("homeassistant/light/robochera/fx");
  Serial.println("Subscribed to topic: homeassistant/light/robochera/fx");
  mqttClient.subscribe("homeassistant/light/robochera/cmnd/dimmer");
  Serial.println("Subscribed to topic: homeassistant/light/robochera/cmnd/dimmer");

    if (!deviceAdded){ // Si no se ha registrado el dispositivo en HA:
      Serial.print("Registering device in Home Assistant...");
      register_homeassistant(); // Registramos dispositivo en Home Assistant
    }
    
  return true;
}

// Conectar a servidor MQTT:
boolean connectMqttOptions(){
  boolean result;
  
    if (mqttUserPasswordValue[0] != '\0'){
      result = mqttClient.connect(iotWebConf.getThingName(), mqttUserNameValue, mqttUserPasswordValue);
    } else if (mqttUserNameValue[0] != '\0'){
      result = mqttClient.connect(iotWebConf.getThingName(), mqttUserNameValue);
    } else {
      result = mqttClient.connect(iotWebConf.getThingName());
    }
  
  return result;
}

// Cuando recibimos un mensaje MQTT:
void mqttMessageReceived(String &topic, String &payload){
  Serial.println("Incoming message of topic " + topic + ": " + payload);

    if (topic.endsWith("robochera/value")) { // Valor del ángulo
      value = payload.toInt();
    } else if (topic.endsWith("switch/robochera/set")){ // Interruptor ON/OFF
        if (payload == "ON"){
          value = atoi(openParamValue);
          mqttClient.publish("homeassistant/switch/robochera/state", "ON");
        } else {
          value = atoi(closedParamValue);
          mqttClient.publish("homeassistant/switch/robochera/state", "OFF");
        }
    } else if (topic.endsWith("light/robochera/set")){ // Luz ON/OFF
        if (payload == "ON"){
          ledState = true;
          mqttClient.publish("homeassistant/light/robochera/state", "ON");
        } else {
          ledState = false;
          FastLED.clear();
          FastLED.show();
          mqttClient.publish("homeassistant/light/robochera/state", "OFF");
        }
    } else if (topic.endsWith("light/robochera/fx")){ // Efectos LED:
        ledEffect = payload;
        FastLED.clear();
        FastLED.show();
        mqttClient.publish("homeassistant/light/robochera/fx_stat", ledEffect);
    } else if (topic.endsWith("dimmer")){ // Brillo:
        FastLED.setBrightness(payload.toInt());
    }  
}

// Efecto de salida en leds:
void colorGoOut(uint16_t wait) {
  unsigned long now = millis();
    if ( (wait < now - timer1) ){
      timer1 = now;
        if (i<0){
          FastLED.clear(); // Set all pixel colors to 'off'
          FastLED.show();
          i=NUMPIXELS/2-1;
        } else {
          strip[i] = color;
          strip[NUMPIXELS-i-1] = color;
          FastLED.show();   // Send the updated pixel colors to the hardware.
          i=i-1;
        } 
    }
}

// Efecto de entrada en leds:
void colorGoIn(uint16_t wait) {
  unsigned long now = millis();
    if ( (wait < now - timer1) ){
      timer1 = now;
        if (i<NUMPIXELS/2){
          strip[i] = color;
          strip[NUMPIXELS-i-1] = color;
          FastLED.show();   // Send the updated pixel colors to the hardware.
          i=i+1;
        } else {
          FastLED.clear(); // Set all pixel colors to 'off'
          FastLED.show();
          i=0;
        } 
    }
}

// Efecto leds "intermitentes naranja":
void colorBlink(uint16_t wait) {
  unsigned long now = millis();
    if ( (wait < now - timer1) ){
      timer1 = now;
        if (i==0){
          //strip = color;
          fill_solid( strip, NUMPIXELS, color);
          //strip[NUMPIXELS-i-1] = color;
          FastLED.show();   // Send the updated pixel colors to the hardware.
          i=i+1;
        } else {
          FastLED.clear(); // Set all pixel colors to 'off'
          FastLED.show();
          i=0;
        }
    }
}

// Abrir puerta:
void openDoor(uint16_t wait) {
  unsigned long now = millis();
    if ( (wait < now - timer2) ){ // Repetimos cada x tiempo:
      timer2 = now;
        if (valuePre < value){ // Si el valor previo es menor que el valor actual:
          valuePre = valuePre + 1;
          servoLeft.write(valuePre + offset);
          servoRight.write(180-valuePre);
          Serial.println(valuePre);
        }
    }
}

// Cerrar puerta:
void closeDoor(uint16_t wait) {
  unsigned long now = millis();
    if ( (wait < now - timer2) ){ // Repetimos cada x tiempo:
      timer2 = now;
        if (valuePre > value){ // Si el valor previo es mayor que el valor actual:
          valuePre = valuePre - 1;
          servoLeft.write(valuePre + offset);
          servoRight.write(180-valuePre);
          Serial.println(valuePre);
        }
    }
}

// Envia el estdo del dispositivo
void sendStatus(){
    // Puerta:
    if ( value >= atoi(openParamValue)){
      mqttClient.publish("homeassistant/switch/robochera/state", "ON");
    } else {
      mqttClient.publish("homeassistant/switch/robochera/state", "OFF");
    }
  
    // LED:
    if (ledState){
      mqttClient.publish("homeassistant/light/robochera/state", "ON");
    } else {
      mqttClient.publish("homeassistant/light/robochera/state", "OFF");
    }

  // Led efecto
  mqttClient.publish("homeassistant/light/robochera/fx_stat", ledEffect);


}


// Registra el dispositivo en Home Assistant:
void register_homeassistant() {
  DynamicJsonDocument doc(1024);
  DynamicJsonDocument device(1024);
  DynamicJsonDocument effect_list(1024);

  device["ids"] = "Robochera"; // identifiers
  device["name"] = "Robochera";
  device["sw"] = FIRMWARE_VERSION; // sw_version
  device["mdl"] = "nodemuc v0.0.1"; // modelo
  device["mf"] = "FjRamirez - Tuelectronica.es"; // manufacturer

  // Interruptor puerta
  doc["~"] = "homeassistant/switch/robochera",
  doc["name"] = "Robochera Door";
  doc["cmd_t"] = "~/set"; // command_topic
  doc["stat_t"] = "~/state";
  doc["uniq_id"] = "robochera_door"; // unique_id
  doc["ic"] = "mdi:garage-variant"; // icon
  doc["device"] = device;

  serializeJson(doc, Serial); // Damos formato JSON
  char playload[measureJson(doc) + 1]; // Creamos un array con el número de carácteres de doc mas fin de linea
  serializeJson(doc, playload, measureJson(doc) + 1); // Pasamos de doc a playload con formato JSON y fin de linea
  
  mqttClient.publish("homeassistant/switch/robochera/config", playload, measureJson(doc) + 1, true); // Publicamos la configuración
  doc.clear(); // Limpiamos el documento dinamico
  
  // Luz led
  doc["~"] = "homeassistant/light/robochera",
  doc["name"] = "Robochera Light";
  doc["cmd_t"] = "~/set"; // command_topic
  doc["stat_t"] = "~/state";
  doc["uniq_id"] = "robochera_light"; // unique_id
  doc["bri_cmd_t"] = "~/cmnd/dimmer"; // brightness_command_topic

  doc["device"] = device;
  doc["fx_cmd_t"] = "~/fx";
  doc["fx_stat_t"] = "~/fx_stat";
  effect_list.add("none");
  effect_list.add("opendoor");
  effect_list.add("closeddoor");
  effect_list.add("opendoor2");
  effect_list.add("colorblink");
  doc["effect_list"] = effect_list;

  serializeJson(doc, Serial); // Damos formato JSON
  char playload2[measureJson(doc) + 1]; // Creamos un array con el número de carácteres de doc mas fin de linea
  serializeJson(doc, playload2, measureJson(doc) + 1); // Pasamos de doc a playload con formato JSON y fin de linea
  
  mqttClient.publish("homeassistant/light/robochera/config", playload2, measureJson(doc) + 1, true); // Publicamos la configuración

  deviceAdded = true;
}

void register_homeassistantHTML() {
  register_homeassistant();
  
  String s = "<!DOCTYPE html><html lang=\"es\"><head><meta charset=\"UTF-8\"><meta name=\"viewport\" content=\"width=device-width, initial-scale=1, user-scalable=no\"/>";
  s += "<title>Robochera</title></head><body>";
  s += "¡Dispositivo añadido a Home Assistant!";
  s += "</body></html>\n";

  server.send(200, "text/html", s);
}
