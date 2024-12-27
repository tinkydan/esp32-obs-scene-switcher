
#include "main.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <WebSocketsClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include "base64.h"
#include "mbedtls/md.h"
#include "Ticker.h"
#include <ESPmDNS.h>

#include <WiFiManager.h> //https://github.com/tzapu/WiFiManager
#include <Preferences.h>
Preferences preferences;
#include <EEPROM.h>

#ifdef ESP32
#include <SPIFFS.h>
#include <FS.h>
#endif

// needed for library
#include <DNSServer.h>
#include <WebServer.h>

Ticker blinker;
Ticker LED_B;

#define DEBUG

#ifdef DEBUG
#define TRACE(x) Serial.print(x);
#else
#define TRACE(x)
#endif

int RESET_COUNT=0;
int TouchPin[10] = {4, 0, 2, 15, 13, 12, 14, 27, 33, 32};
int LedPin[10] = {25, 23, 22, 21, 19, 18, 5, 17, 16, 26};
int WAVE[8] = {1, 4, 7, 8, 9, 6, 3, 2};
int WAVErev[8] = {2, 3, 6, 9, 8, 7, 4, 1};
unsigned long last_trigger[10];
int Touch_T[10]; // number of touch trigger
int threshold;
int touchValue;
int debounce = 400;
int touch_check = 10;
int Touch_State[10];
int Hotkey_State[10];
bool get_Scene = 0;
int Request_Count = 1;
char Scene_Name[][32] = {"Scene1", "Scene2", "Scene3", "Scene4", "Scene5", "Scene6", "Scene7"}; //,

// byte Brightness[65536];
unsigned int LED_Iter = 0;
int iterator = 0;

/* Put your SSID & Password */
// Default Can be set in web Browser

char OBS_IP_Address[20] = "192.168.1.107";  // OSB computer Default Can be set in web Browser
char OBS_port[6] = "4455";                  // OSB port
char OBS_password[34] = "axzg28JChH6Ox6c7"; // OSB password
char OBS_input[40] = "Mic/Aux";             // OSB Input
char threshold_ch[6] = "57";             // Touch Sensitivity

WebSocketsClient webSocket;

WiFiManager wifiManager;

WiFiManagerParameter custom_OBS_IP_Address("server", "OBS_IP_Address", OBS_IP_Address, 20);
WiFiManagerParameter custom_OBS_port("port", "OBS_port", OBS_port, 6);
WiFiManagerParameter custom_OBS_password("apikey", "OBS_password", OBS_password, 32);
WiFiManagerParameter custom_OBS_input("Input", "OBS_Audio_Input", OBS_input, 40);
WiFiManagerParameter custom_touch_sensitivity("Thres", "Touch Sensitivity (0-100) lower is more sensitivie", threshold_ch, 6);

StaticJsonDocument<2000> doc;

uint8_t InputPin = 2;
uint8_t LEDPin = 12;
bool PreviousState = LOW;

void factory_reset()
{
    TRACE("Factory Reset\n");
    wifiManager.resetSettings();
    WiFi.disconnect();
    //delay(3000);
    //hardwareReset();
    delay(2000);
ESP.restart();
}

byte *sha256(const char *payload)
{
    // http://www.esp32learning.com/code/using-sha-256-with-an-esp32.php
    byte *shaResult = new byte[32];

    mbedtls_md_context_t ctx;
    mbedtls_md_type_t md_type = MBEDTLS_MD_SHA256;

    const size_t payloadLength = strlen(payload);
    TRACE("\npayload:");
    TRACE(payload);
    TRACE("\npayload_length:");
    TRACE(payloadLength);

    mbedtls_md_init(&ctx);
    mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(md_type), 0);
    mbedtls_md_starts(&ctx);
    mbedtls_md_update(&ctx, (const unsigned char *)payload, payloadLength);
    mbedtls_md_finish(&ctx, shaResult);
    mbedtls_md_free(&ctx);
    return shaResult;
}

std::string EncodePassword(std::string challenge, std::string salt)
{
    macaron::Base64 b64encode;

    std::string message = OBS_password + salt;

    // Create a SHA256 hash for password + server salt
    static const byte *hash = sha256(message.c_str());

    static const std::string secret = b64encode.Encode(hash, 32);

    // Create a SHA256 hash for base64 secret + server challenge
    const std::string message2 = secret + challenge;

    static const byte *auth_response_hash = sha256(message2.c_str());

    static const std::string auth_response = b64encode.Encode(auth_response_hash, 32);

    TRACE("\npassword:");
    TRACE(OBS_password);
    TRACE("\nsalt:");
    TRACE(salt.c_str());
    TRACE("\nchallenge:");
    TRACE(challenge.c_str());
    TRACE("\nb64_encoded_hash:");
    TRACE(secret.c_str());
    TRACE("\nmsg2h:");
    TRACE(message2.c_str());
    TRACE("\nb64_encoded_auth:");
    TRACE(auth_response.c_str());

    return auth_response;
}

// flag for saving data
bool shouldSaveConfig = false;

// callback notifying us of the need to save config
void saveConfigCallback()
{
     // read updated parameters
    strcpy(OBS_IP_Address, custom_OBS_IP_Address.getValue());
    strcpy(OBS_port, custom_OBS_port.getValue());
    strcpy(OBS_password, custom_OBS_password.getValue());
    strcpy(OBS_input, custom_OBS_input.getValue());
    strcpy(threshold_ch,  custom_touch_sensitivity.getValue());
    Serial.println("Should save config");
    shouldSaveConfig = true;
    Serial.println("Setting EEPROM");
    EEPROM.put( 0, OBS_IP_Address);//[20]
    EEPROM.put( 20, OBS_port);     //[6]
    EEPROM.put( 26, OBS_password);  //[34] 
    EEPROM.put( 60, OBS_input);    //[40] 
    EEPROM.put( 100, threshold_ch);    //[6] 
    TRACE("The values stored: \n");
    TRACE("\tOBS_IP_Address : " + String(OBS_IP_Address) + "\n");
    TRACE("\tOBS_port : " + String(OBS_port) + "\n");
    TRACE("\tOBS_password : " + String(OBS_password) + "\n");
    TRACE("\tOBS_Input : " + String(OBS_input) + "\n");
    TRACE("\tTouch Sensitivity: " + String(threshold_ch) + "\n");
    EEPROM.commit();
    threshold=atoi(threshold_ch);
  
}

void Request_Data(String Request)
{
    Request_Count++;
    StaticJsonDocument<200> reqestdoc;
    reqestdoc["op"] = 6;
    reqestdoc["d"]["requestType"] = Request;
    reqestdoc["d"]["requestId"] = String(Request_Count);
    String output2;
    serializeJsonPretty(reqestdoc, output2);
    webSocket.sendTXT(output2);
}

void Mute_Input(String Channel, bool stauts)
{
    Request_Count++;
    StaticJsonDocument<200> reqestdoc;
    reqestdoc["op"] = 6;
    reqestdoc["d"]["requestType"] = "SetInputMute";
    reqestdoc["d"]["requestId"] = String(Request_Count);
    reqestdoc["d"]["requestData"]["inputName"] = Channel;
    reqestdoc["d"]["requestData"]["inputMuted"] = stauts;

    String output2;

    serializeJsonPretty(reqestdoc, output2);
    webSocket.sendTXT(output2);
}

void Get_Mute(String Channel)
{
    Request_Count++;
    StaticJsonDocument<200> reqestdoc;
    reqestdoc["op"] = 6;
    reqestdoc["d"]["requestType"] = "GetInputMute";
    reqestdoc["d"]["requestId"] = String(Request_Count);
    reqestdoc["d"]["requestData"]["inputName"] = Channel;
    String output2;
    serializeJsonPretty(reqestdoc, output2);
    webSocket.sendTXT(output2);
}

void set_led_brightness(unsigned int POS, int LED_CH)
{
    if (POS > 65535)
    {
        POS = POS - 65535;
    }
    if (POS < 6000)
    {
        ledcWrite(LED_CH, map(POS, 0, 6000, 0, 255));
    }
    else if (POS < 13000)
    {
        ledcWrite(LED_CH, 255);
    }
    else if (POS < 19000)
    {
        ledcWrite(LED_CH, 255 - map(POS, 13000, 19000, 0, 255));
    }
    else
    {
        ledcWrite(LED_CH, 0);
    }
}

void LED_FADE()
{
    for (int i = 0; i <= 7; i++)
    {
        LED_Iter = LED_Iter + 100;
        if (LED_Iter > 65535)
        {
            LED_Iter = LED_Iter - 65535;
        }
        set_led_brightness(LED_Iter + i * 65535 / 8, WAVE[i]);
    }
}

void process_button(int button_num, bool set)
{
    if (set && (button_num > 2))
    {
        for (int i = 3; i <= iterator + 2; i++)
        {
            Hotkey_State[i] = 0;
            ledcWrite(i, 0);
        }
    }
    if (set && (button_num < iterator + 3) && (button_num != 2))
    {
        ledcWrite(button_num, 255);
    }

    for (int i = 0; i <= 6; i++)
    {
        TRACE(Scene_Name[i]);
        TRACE("\n");
    }

    TRACE("Setting button for ");
    TRACE(button_num);
    TRACE(" LED ");
    TRACE(Hotkey_State[button_num] == 1);

    switch (button_num)
    {

    case 0:
        Request_Data("GetSceneList");

    case 1: // Transition
        Request_Data("TriggerStudioModeTransition");
        // Request_Data("GetInputList");
        // Request_Data("GetSceneList");
        break;

    case 2: // MUTE

        if (((Hotkey_State[2] == 0) && set) || ((Hotkey_State[2] == 1) && !set))
        {
            ledcWrite(2, 255);
            if (set)
            {
                Hotkey_State[2] = 1;
                Mute_Input(OBS_input, Hotkey_State[2]);
            }
        }
        else if (((Hotkey_State[2] == 1) && set) || ((Hotkey_State[2] == 0) && !set))
        {
            ledcWrite(2, 0);
            if (set)
            {
                Hotkey_State[2] = 0;
                Mute_Input(OBS_input, Hotkey_State[2]);
            }
        }

        TRACE(" Case 2 ");
        break;

    default:

        TRACE("Button num ");
        TRACE(button_num);
        TRACE(" Default ");
        if (((Hotkey_State[button_num] == 0) && set) || ((Hotkey_State[button_num] == 1) && !set))
        {

            TRACE(" UpState ");
            TRACE(Hotkey_State[button_num]);
            if (set)
            {
                Hotkey_State[button_num] = 1;

                Request_Count++;
                StaticJsonDocument<200> reqestdoc;
                reqestdoc["op"] = 6;
                reqestdoc["d"]["requestType"] = "SetCurrentProgramScene";
                reqestdoc["d"]["requestId"] = String(Request_Count);
                reqestdoc["d"]["requestData"]["sceneName"] = Scene_Name[button_num - 3];

                String output2;

                serializeJsonPretty(reqestdoc, output2);
                webSocket.sendTXT(output2);
            }
        }
        else if (((Hotkey_State[button_num] == 1) && set) || ((Hotkey_State[button_num] == 0) && !set))
        {
            TRACE(" DownState ");
            if (set)
            {
                Hotkey_State[button_num] = 0;
            }
        }
        break;
    }
    TRACE(" after ");
    TRACE(Hotkey_State[button_num] == 1);
    TRACE("\n");
}

void ReceivedResponse(char *payload)
{
    TRACE("Payload ->/\n");
    TRACE(payload);
    TRACE("\n");
}

void ParseOBSResponse(char *payload)
{
    // TRACE("Payload-------------------\n");
    //  TRACE(payload);
    //  TRACE("--------------------------\n");
    //  TRACE("Parsing Response\n");
    // webSocket.sendTXT("Hello");
    // Deserialize the JSON document
    DeserializationError error = deserializeJson(doc, payload);

    // Test if parsing succeeds.
    if (error)
    {
        TRACE(F("deserializeJson() failed: "));
        TRACE(error.f_str());
        return;
    }
    else
    {
        JsonObject root = doc.as<JsonObject>();
        // DeserializationError error = deserializeJson(doc, roota["d"]);
        // JsonObject root = doc.as<JsonObject>();

        if (get_Scene)
        {
            get_Scene = 0;
            Request_Data("GetSceneList");
            Get_Mute(OBS_input);
        }

        // Retreives the responce from the mute status request may want a unique id to ensure correct processing
        if (root["d"]["responseData"])
        {
            if (strcmp(root["d"]["requestType"], "GetInputMute") == 0)
            {
                Hotkey_State[2] = root["d"]["responseData"]["inputMuted"];
                process_button(2, 0);
            }
        }

        char RqT[][30] = {"responseData", "eventData"};
        for (int k = 0; k <= 1; k++)
        {

            TRACE(k);
            if (root["d"][RqT[k]]["scenes"])
            {

                iterator = 0;
                while (root["d"][RqT[k]]["scenes"][iterator]["sceneName"])
                {
                    iterator++;
                }
                int iter = 0;
                for (int i = iterator - 1; i >= iterator - 7; i--)
                {
                    if (i >= 0)
                    {
                        strcpy(Scene_Name[iter], root["d"][RqT[k]]["scenes"][i]["sceneName"]);
                        TRACE(Scene_Name[iter]);
                        TRACE("\n");
                        iter++;
                    }
                }
                for (int i = iterator; i < 7; i++)
                {
                    ledcWrite(i + 3, 1);
                }
                for (int i = 0; i <= iterator - 1; i++)
                {
                    Hotkey_State[i + 3] = 0;
                    ledcWrite(i + 3, 0);

                    if (strcmp(root["d"]["responseData"]["currentProgramSceneName"], Scene_Name[i]) == 0)
                    {
                        Hotkey_State[i + 3] = 1;
                        ledcWrite(i + 3, 255);
                    }
                }
            }
        }
        if (root["d"]["eventType"])
        {

            if (strcmp(root["d"]["eventType"], "InputMuteStateChanged") == 0)
            {
                if (strcmp(root["d"]["eventData"]["inputName"], OBS_input) == 0)
                {
                    Hotkey_State[2] = root["d"]["eventData"]["inputMuted"];
                    process_button(2, 0);
                }
            }
            if (strcmp(root["d"]["eventType"], "CurrentProgramSceneChanged") == 0)
            {
                TRACE("\nScene Change \n");
                for (int i = 0; i <= iterator - 1; i++)
                {
                    Hotkey_State[i + 3] = 0;
                    ledcWrite(i + 3, 0);

                    if (strcmp(root["d"]["eventData"]["sceneName"], Scene_Name[i]) == 0)
                    {
                        Hotkey_State[i + 3] = 1;
                        ledcWrite(i + 3, 255);
                    }
                }
            }
            if (strcmp(root["d"]["eventType"], "CurrentSceneCollectionChanged") == 0)
            {
                Request_Data("GetSceneList");
            }
            else if (strcmp(root["d"]["eventType"], "SceneCollectionListChanged") == 0)
            {
                Request_Data("GetSceneList");
            }
            else if (strcmp(root["d"]["eventType"], "CurrentProfileChanged") == 0)
            {
                Request_Data("GetSceneList");
            }
            else if (strcmp(root["d"]["eventType"], "SceneCreated") == 0)
            {
                Request_Data("GetSceneList");
            }
            else if (strcmp(root["d"]["eventType"], "SceneRemoved") == 0)
            {
                Request_Data("GetSceneList");
            }
            else if (strcmp(root["d"]["eventType"], "SceneNameChanged") == 0)
            {
                Request_Data("GetSceneList");
            }
            else if (strcmp(root["d"]["eventType"], "SceneListChanged") == 0)
            {
                Request_Data("GetSceneList");
            }
            else if (strcmp(root["d"]["eventType"], "SceneItemCreated") == 0)
            {
                Request_Data("GetSceneList");
            }
            else if (strcmp(root["d"]["eventType"], "SceneItemRemoved") == 0)
            {
                Request_Data("GetSceneList");
            }
            else if (strcmp(root["d"]["eventType"], "SceneItemListReindexed") == 0)
            {
                Request_Data("GetSceneList");
            }
        }

        if ((root["op"] == 0))
        {
            std::string challenge = root["d"]["authentication"]["challenge"];
            std::string salt = root["d"]["authentication"]["salt"];
            std::string hashedpassword = EncodePassword(challenge, salt);

            TRACE("\nResponse Hash: ");
            TRACE(hashedpassword.c_str());

            // Send JSON Packet

            StaticJsonDocument<200> responsedoc;
            responsedoc["op"] = 1;
            responsedoc["d"]["rpcVersion"] = 1;
            if (root["d"]["authentication"])
            {
                responsedoc["d"]["authentication"] = hashedpassword;
            }

            responsedoc["d"]["eventSubscriptions"] = 1 << 0 | 1 << 1 | 1 << 2 | 1 << 3 | 1 << 7;
            String output;

            serializeJsonPretty(responsedoc, output);
            webSocket.sendTXT(output);
            get_Scene = 1;
            // webSocket.sendTXT("Hello");
            // webSocket.sendTXT(output);
            webSocket.loop();

            TRACE("\nAuth Response\n");
            TRACE(output.c_str());
            LED_B.detach();

            for (int i = 0; i <= 9; i++)
            {
                ledcAttachPin(LedPin[i], i);
            }
        }
        TRACE("\n");
    }
}

void webSocketEvent(WStype_t type, uint8_t *payload, size_t length)
{
    TRACE("Websocket Event:");
    TRACE(type);
    TRACE("\n");
    ReceivedResponse((char *)payload);
    // TRACE(payload);
    switch (type)
    {
    case WStype_DISCONNECTED:
        TRACE("OBS: Disconnected\n");
        LED_B.attach_ms(10, LED_FADE);
        break;
    case WStype_CONNECTED:
        TRACE("OBS: Connected\n");
    case WStype_PING:
        TRACE("OBS: Pinged\n");
    case WStype_PONG:
        TRACE("OBS: Pong\n");
        break;
    case WStype_BIN:
        TRACE("OBS: Bin Request\n");
        break;
    case WStype_TEXT:
        ParseOBSResponse((char *)payload);
        break;
    case WStype_ERROR:
        break;
    case WStype_FRAGMENT_TEXT_START:
    case WStype_FRAGMENT_BIN_START:
    case WStype_FRAGMENT:
    case WStype_FRAGMENT_FIN:
        break;
    }
}

void restart_portal_delayed()
{
    wifiManager.startWebPortal();
    Serial.println("Config portal running");
    Serial.println(WiFi.softAPIP());
    blinker.detach();
}

void restart_portal()
{
    blinker.attach(5, restart_portal_delayed); // Use attach_ms if you need time in ms
}

void setup()
{
    
#ifdef DEBUG
    Serial.begin(115200);
    while (!Serial)
        continue;
#endif



    TRACE("Booting OBS Controller:");

    for (int i = 0; i <= 9; i++)
    {
        ledcSetup(i, 5000, 8);
        // attach the channel to the GPIO to be controlled
        ledcAttachPin(LedPin[i], i);
    }
    // read configuration from FS json


    delay(1000);
    while (!digitalRead(0))
    {
        RESET_COUNT=RESET_COUNT+1;
        if (RESET_COUNT>50){
          factory_reset();
        }
        delay(100);
        for (int i = 0; i <= 9; i++){
        ledcWrite(i, 0);}
        delay(100);
        for (int i = 0; i <= 9; i++){
        ledcWrite(i, 255);}

    }

LED_B.attach_ms(10, LED_FADE);

Serial.println("Starting EEPROM");
  while (!EEPROM.begin(256)) {
    true;
  }
  ////////////////////////////////////////////////////`
  // Pull the values from eeprom
  //EEPROM
  if (1 == 0) {
    Serial.println("Setting EEPROM");
    EEPROM.put( 0, OBS_IP_Address);//[20]
    EEPROM.put( 20, OBS_port);     //[6]
    EEPROM.put( 26, OBS_password);  //[34] 
    EEPROM.put( 60, OBS_input);    //[40] 
    EEPROM.put( 100, threshold_ch);    //[6] 
    EEPROM.commit();
  }
  //| LATT | LONG | TIMZ | ARIS | ASET | UpLm | DnLm
    Serial.println("Reading EEPROM");
    EEPROM.get( 0, OBS_IP_Address);//[20]
    EEPROM.get( 20, OBS_port);     //[6]
    EEPROM.get( 26, OBS_password);  //[34] 
    EEPROM.get( 60, OBS_input);    //[40] 
    EEPROM.get( 100, threshold_ch);    //[6] 
    threshold=atoi(threshold_ch);
    custom_OBS_IP_Address.setValue( OBS_IP_Address, 20);
    custom_OBS_port.setValue(OBS_port, 6);
    custom_OBS_password.setValue( OBS_password, 32);
    custom_OBS_input.setValue( OBS_input, 40);
    custom_touch_sensitivity.setValue( threshold_ch, 6);
    // end read

    // The extra parameters to be configured (can be either global or just in the setup)
    // After connecting, parameter.getValue() will get you the configured value
    // id/name placeholder/prompt default length

    // WiFiManager
    // Local intialization. Once its business is done, there is no need to keep it around
    std::vector<const char *> menu = {"wifi", "wifinoscan", "info", "param", "sep", "erase", "restart"};
    wifiManager.setMenu(menu); // custom menu, pass vector

    // set config save notify callback
    wifiManager.setSaveConfigCallback(saveConfigCallback);
    
    wifiManager.setSaveParamsCallback(saveConfigCallback);


    // set static ip
    // wifiManager.setSTAStaticIPConfig(IPAddress(10, 0, 1, 99), IPAddress(10, 0, 1, 1), IPAddress(255, 255, 255, 0));

    // add all your parameters here
    wifiManager.addParameter(&custom_OBS_IP_Address);
    wifiManager.addParameter(&custom_OBS_port);
    wifiManager.addParameter(&custom_OBS_password);
    wifiManager.addParameter(&custom_OBS_input);
    wifiManager.addParameter(&custom_touch_sensitivity);
    // reset settings - for testing
    // wifiManager.resetSettings();
     //////   WiFi.hostname("OBSBOARD"); // <---- MDNS network hostname dhcp client
    // fetches ssid and pass and tries to connect
    // if it does not connect it starts an access point with the specified name
    // here  "AutoConnectAP"
    // and goes into a blocking loop awaiting configuration

    ////  delay(200); // do not remove, give time for

    ////    if(MDNS.begin("OBSBOARD")){
     ///     MDNS.addService("http", "tcp", 80);
   //   /  }
////
    if (!wifiManager.autoConnect("OBS_Setup_AP"))
    {
        Serial.println("failed to connect and hit timeout");
        delay(3000);
        // reset and try again, or maybe put it to deep sleep
        ESP.restart();
        delay(5000);
    }

    // if you get here you have connected to the WiFi
    for (int i = 0; i < 8; i++)
    {WAVE[i] = WAVErev[i];}
    wifiManager.setConfigPortalBlocking(false);

    // read updated parameters
    //strcpy(OBS_IP_Address, custom_OBS_IP_Address.getValue());
    //strcpy(OBS_port, custom_OBS_port.getValue());
    //strcpy(OBS_password, custom_OBS_password.getValue());
    //strcpy(OBS_input, custom_OBS_input.getValue());
    TRACE("The values in the file are: \n");
    TRACE("\tOBS_IP_Address : " + String(OBS_IP_Address) + "\n");
    TRACE("\tOBS_port : " + String(OBS_port) + "\n");
    TRACE("\tOBS_password : " + String(OBS_password) + "\n");
    TRACE("\tOBS_Input : " + String(OBS_input) + "\n");

  if (shouldSaveConfig) {
//SAVE to EEPROM

    Serial.println("Setting EEPROM");
    EEPROM.put( 0, OBS_IP_Address);//[20]
    EEPROM.put( 20, OBS_port);     //[6]
    EEPROM.put( 26, OBS_password);  //[34] 
    EEPROM.put( 60, OBS_input);    //[40] 
    EEPROM.commit();

  }

    Serial.println("local ip");
    Serial.println(WiFi.localIP());

    wifiManager.setSaveConfigCallback(restart_portal);
    wifiManager.startWebPortal();

    TRACE("\nIP address: " + String(WiFi.localIP()) + "\n");

    // server address, port and URL
    webSocket.begin(OBS_IP_Address, atoi(OBS_port), "/");

    // event handler
    webSocket.onEvent(webSocketEvent);

    // try ever 5000 again if connection has failed
    webSocket.setReconnectInterval(10000);

    if (!MDNS.begin("OBS_BOARD"))
    { // http://esp32.local
        MDNS.addService("http", "tcp", 80);
        Serial.println("Error setting up MDNS responder!");
        while (1)
        {
            delay(1000);
        }
    }
}

void loop()
{
    webSocket.loop();
    // if (!is_active) {
    wifiManager.process();
    //MDNS.update();
    // Check for Updates and things
    //}
    // put your main code here, to run repeatedly:
    for (int i = 0; i <= 9; i++)
    {
        touchValue = touchRead(TouchPin[i]);

        // check if the touchValue is below the threshold
        // if it is, set ledPin to HIGH
        if ((Touch_T[i] < touch_check) && (touchValue < threshold) && ((millis() - last_trigger[i]) > debounce))
        {
            // turn LED on
            Touch_T[i] = Touch_T[i] + 5;
            Touch_T[i] = min(Touch_T[i], touch_check);
        }
        else if (((millis() - last_trigger[i]) > debounce) && (Touch_T[i] > -touch_check))
        {
            // turn LED off
            Touch_T[i]--;
        }

        if (Touch_T[i] == touch_check)
        { // indicate a touch event

            if (Touch_State[i] < 1)
            {
                last_trigger[i] = millis();
                Touch_State[i] = 1;
                Touch_T[i]++;
                process_button(i, 1);
            }
        }
        if (Touch_T[i] == -touch_check)
        { // indicate a touch event

            if (Touch_State[i] > 0)
            {
                last_trigger[i] = millis();
                Touch_State[i] = 0;
                Touch_T[i]--;
                if (i < 2)
                {
                    ledcWrite(i, 0);
                }
            }
        }
    }



}