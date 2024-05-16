#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiAP.h>
#include "esp_camera.h"
#include "Freenove_4WD_Car_WiFi.h"
#include "Freenove_4WD_Car_Emotion.h"
#include "Freenove_4WD_Car_WS2812.h"
#include "Freenove_4WD_Car_For_ESP32.h"
#include <PubSubClient.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <limits.h>

// Ressources
// https://randomnerdtutorials.com/esp32-websocket-server-arduino/#1
// https://randomnerdtutorials.com/esp32-mqtt-publish-subscribe-arduino-ide/
// https://randomnerdtutorials.com/esp32-static-fixed-ip-address-arduino-ide/
// https://github.com/Freenove/Freenove_4WD_Car_Kit_for_ESP32/tree/master

// Replace the next variables with your SSID/Password combination
const char *ssid_wifi = "OnePlus 7 Pro";
const char *password_wifi = "33500a9695df";

// Add your MQTT Broker IP address, example:
// const char* mqtt_server = "192.168.1.144";
const char *mqtt_server = "192.168.145.40";
const int mqtt_interval_ms = 5000;

// Setup the desired IP address of your car
IPAddress localIP(192, 168, 145, 49);

IPAddress localGateway(192, 168, 145, 55);
IPAddress localSubnet(255, 255, 255, 0);
IPAddress primaryDNS(8, 8, 8, 8);
IPAddress secondaryDNS(8, 8, 4, 4);

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

WiFiClient espClient;
PubSubClient client(espClient);

// WiFiServer server_Cmd(4000);
WiFiServer server_Camera(7000);
bool videoFlag = 0;

long last_message = 0;

int distance[4];          // Storage of ultrasonic data
int sensor_v;             // Int cast of track sensor data
char buff[6];             // Buffer to store the battery voltage data
char ultrasonic_buff[10]; // Buffer to store the Ultrasonic data

// put function declarations here:
void WiFi_Init();
void loopTask_Camera(void *pvParameters);
void notifyClients();
void handleWebSocketMessage(void *arg, uint8_t *data, size_t len);
void onEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len);
void initWebSocket();
void reconnect();

void WiFi_Init()
{
    ssid_Router = "OnePlus 7 Pro";    // Modify according to your router name
    password_Router = "33500a9695df"; // Modify according to your router password
    ssid_AP = "Sunshine";             // ESP32 turns on an AP and calls it Sunshine
    password_AP = "Sunshine";         // Set your AP password for ESP32 to Sunshine
    frame_size = FRAMESIZE_CIF;       // 400*296
}

void setup()
{
    delay(5000);

    Serial.begin(115200);
    Serial.setDebugOutput(true);

    if (!WiFi.config(localIP, localGateway, localSubnet, primaryDNS, secondaryDNS))
    {
        Serial.println("STA Failed to configure");
    }

    Buzzer_Setup(); // Buzzer initialization
    WiFi_Init();    // WiFi paramters initialization
    WiFi_Setup(0);  // Start AP Mode. If you want to connect to a router, change 1 to 0.
    // server_Cmd.begin(4000);    // Start the command server
    server_Camera.begin(7000); // Turn on the camera server

    cameraSetup();      // Camera initialization
    Emotion_Setup();    // Emotion initialization
    WS2812_Setup();     // WS2812 initialization
    PCA9685_Setup();    // PCA9685 initialization
    Light_Setup();      // Light initialization
    Track_Setup();      // Track initialization
    Ultrasonic_Setup(); // Initialize the ultrasonic module

    // Cette section serait peut être à virer...
    disableCore0WDT(); // Turn off the watchdog function in kernel 0
    xTaskCreateUniversal(loopTask_Camera, "loopTask_Camera", 8192, NULL, 0, NULL, 0);
    xTaskCreateUniversal(loopTask_WTD, "loopTask_WTD", 8192, NULL, 0, NULL, 0);

    client.setServer(mqtt_server, 1883);

    initWebSocket();

    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request)
              { 
                camera_fb_t *fb = NULL;
                fb = esp_camera_fb_get();

                Serial.println(fb != NULL);

                        if (fb != NULL)
                        {
                            uint8_t slen[4];
                            slen[0] = fb->len >> 0;
                            slen[1] = fb->len >> 8;
                            slen[2] = fb->len >> 16;
                            slen[3] = fb->len >> 24;
                            // client.write(slen, 4);
                            client.write(fb->buf, fb->len);
                            request->send_P(200, "image", fb->buf, fb->len);
                            Serial.println("Camera send");
                            esp_camera_fb_return(fb);
                        } });

    server.begin();
    Servo_1_Angle(60);
    Emotion_SetMode(1);
}

void loop()
{
    // put your main code here, to run repeatedly:
    ws.cleanupClients();

    // The MQTT part
    if (!client.connected())
    {
        reconnect();
    }
    client.loop();

    long now = millis();
    if (now - last_message > mqtt_interval_ms)
    {
        last_message = now;

        // Battery level
        dtostrf(Get_Battery_Voltage(), 5, 2, buff);
        client.publish("esp32/battery", buff);

        // Track Read
        Track_Read();
        sensor_v = static_cast<int>(sensorValue[3]);
        char const *n_char = std::to_string(sensor_v).c_str();
        client.publish("esp32/track", n_char);

        // Ultrasonic Data
        dtostrf(Get_Sonar(), 5, 2, ultrasonic_buff);
        client.publish("esp32/sonar", ultrasonic_buff);
    }

    Emotion_Show(emotion_task_mode); // Led matrix display function
    WS2812_Show(ws2812_task_mode);   // Car color lights display function
}

// put function definitions here:
void notifyClients()
{
    ws.textAll(String("hellow"));
}

void handleWebSocketMessage(void *arg, uint8_t *data, size_t len)
{
    AwsFrameInfo *info = (AwsFrameInfo *)arg;

    // Serial.println((char *)data);

    if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT)
    {
        data[len] = 0;

        StaticJsonDocument<200> doc;

        DeserializationError error = deserializeJson(doc, (char *)data);

        if (error)
        {
            Serial.print("deserializeJson() failed: ");
            Serial.println(error.c_str());
            return;
        }

        int cmd = doc["cmd"];

        if (1 == cmd)
        {
            JsonArray data = doc["data"];
            int data_0 = data[0];
            int data_1 = data[1];
            int data_2 = data[2];
            int data_3 = data[3];

            Motor_Move(data_0, data_1, data_2, data_3);
        }
        else if (2 == cmd)
        {
            int data = doc["data"];
            Emotion_SetMode(data);
        }
        else if (3 == cmd)
        {
            JsonArray angles = doc["data"];
            int angle_0 = angles[0];
            int angle_1 = angles[1];
            Servo_1_Angle(angle_0); // Set the Angle value of servo 1 to 0 to 180°
            Servo_2_Angle(angle_1);
        }
        else if (4 == cmd)
        {
            int led_mode = doc["data"];
            WS2812_SetMode(led_mode);
        }
        else if (5 == cmd)
        {
            JsonArray led_color = doc["data"];
            int led_color_0 = led_color[0];
            int led_color_1 = led_color[1];
            int led_color_2 = led_color[2];
            int led_color_3 = led_color[3];

            WS2812_Set_Color_1(led_color_0, led_color_1, led_color_2, led_color_3);
        }
        else if (6 == cmd)
        {
            JsonArray led_color_2 = doc["data"];
            int led_color_2_0 = led_color_2[0];
            int led_color_2_1 = led_color_2[1];
            int led_color_2_2 = led_color_2[2];
            int led_color_2_3 = led_color_2[3];

            WS2812_Set_Color_2(led_color_2_0, led_color_2_1, led_color_2_2, led_color_2_3);
        }
        else if (7 == cmd)
        {
            bool alarm = doc["data"] == 1;
            Buzzer_Alarm(alarm);
        }
        else if (8 == cmd)
        {
            JsonArray buzzer_data = doc["data"];
            int alarm_on = buzzer_data[0] == 1;
            int frequency_hz = buzzer_data[1];
            Buzzer_Variable(alarm_on, frequency_hz);
        }

        notifyClients();
    }
}

void onEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len)
{
    switch (type)
    {
    case WS_EVT_CONNECT:
        Serial.printf("WebSocket client #%u connected from %s\n", client->id(), client->remoteIP().toString().c_str());
        break;
    case WS_EVT_DISCONNECT:
        Serial.printf("WebSocket client #%u disconnected\n", client->id());
        break;
    case WS_EVT_DATA:
        handleWebSocketMessage(arg, data, len);
        break;
    case WS_EVT_PONG:
    case WS_EVT_ERROR:
        break;
    }
}

void initWebSocket()
{
    ws.onEvent(onEvent);
    server.addHandler(&ws);
}

void reconnect()
{
    // Loop until we're reconnected
    while (!client.connected())
    {
        Serial.print("Attempting MQTT connection...");
        // Attempt to connect
        if (client.connect("ESP8266Client"))
        {
            Serial.println("connected");
            // Subscribe
            client.subscribe("esp32/output");
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

void loopTask_Camera(void *pvParameters)
{
    while (1)
    {
        WiFiClient client = server_Camera.available(); // listen for incoming clients
        if (client)
        { // if you get a client
            Serial.println("Camera_Server connected to a client.");
            if (client.connected())
            {
                camera_fb_t *fb = NULL;
                while (client.connected())
                { // loop while the client's connected
                    if (videoFlag == 1)
                    {
                        fb = esp_camera_fb_get();
                        if (fb != NULL)
                        {
                            uint8_t slen[4];
                            slen[0] = fb->len >> 0;
                            slen[1] = fb->len >> 8;
                            slen[2] = fb->len >> 16;
                            slen[3] = fb->len >> 24;
                            client.write(slen, 4);
                            client.write(fb->buf, fb->len);
                            Serial.println("Camera send");
                            esp_camera_fb_return(fb);
                        }
                    }
                }
                // close the connection:
                client.stop();
                Serial.println("Camera Client Disconnected.");
                ESP.restart();
            }
        }
    }
}