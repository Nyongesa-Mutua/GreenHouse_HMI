#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <DHTesp.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <time.h>

extern "C" 
{
	#include "freertos/FREERTOS.h"
	#include "freertos/task.h"
	#include "freertos/queue.h"
}

// pins
#define WATER_SENSOR_PIN 34
#define RELAY_PIN 27
#define DHT_PIN 4
#define FAN_PIN 26
#define LDR_PIN 35			//analog input pin
#define LED_PIN 32
//#define LIGHT_BUTTON_PIN 18
#define FAN_BUTTON_PIN 33
#define PUMP_BUTTON_PIN 13

DHTesp dht;
//LiquidCrystal_I2C lcd(0x3F, 16, 2);
LiquidCrystal_I2C lcd(0x27, 20, 4);

// RTOS handles
TaskHandle_t water_sensor_task_handle = NULL;
QueueHandle_t water_sensor_queue = NULL;	//integer
QueueHandle_t control_queue = NULL;	//boolean
QueueHandle_t command_queue = NULL;	//boolean
QueueHandle_t button_queue = NULL;	//boolean

// control params - tune after calibration
static const int LOWER_WATER_THRESHOLD = 1800;
static const int UPPER_WATER_THRESHOLD = 2300;
static const float TEMP_LOW = 26.0;
static const float TEMP_HIGH = 30.0;
static const int LIGHT_THRESHOLD = 1500;

// Timer ISR
hw_timer_t *timer = NULL;

const char* ssid = "";
const char* password = "";

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

volatile int latest_water_sensor_value = 0;
volatile int ldr_value = 0;
volatile bool pump_state = false;
volatile bool fan_state = false;
volatile bool light_state = false;
volatile bool manual_override = false;
volatile bool manual_pump = false;
volatile bool manual_fan = false;
volatile bool manual_lights = false;

volatile float latest_temp = 0.0;
volatile float latest_humidity = 0.0;

typedef struct {
	bool pump;
	bool fan;
	bool light;
} control_msg_t;

typedef struct {
	int button_id;
	bool pressed;
} button_event;

typedef enum {
	CMD_NONE,
	CMD_TOGGLE_PUMP,
	CMD_TOGGLE_FAN,
	CMD_TOGGLE_LIGHTS,
	CMD_AUTO,
} command_type_t;

typedef struct {
	command_type_t type;
} command_msg_t;


void init_time()
{
	configTime(3 * 3600, 0, "pool.ntp.org", "time.nist.gov");
	struct tm timeinfo;
	
	int retries = 0;
	while (!getLocalTime(&timeinfo) && retries < 10) {
		Serial.println("Waiting for NTP time sync ....");
		vTaskDelay(pdMS_TO_TICKS(1000));
		retries++;
	}
	Serial.println("Time synchronized.");
}


void initWiFi()
{
	Serial.println("pp");
	WiFi.begin(ssid, password);

	while (WiFi.status() != WL_CONNECTED) {
		vTaskDelay(pdMS_TO_TICKS(500));
	}

	Serial.print("IP address: ");
	Serial.println(WiFi.localIP());
}


void initFS()
{
	if (!LittleFS.begin()) {
		Serial.println("LittleFS mount failed.");
		return;
	}
}

//hardware timer
void IRAM_ATTR onTimer() 
{
	BaseType_t hp_task_woken = pdFALSE;
	vTaskNotifyGiveFromISR(water_sensor_task_handle, &hp_task_woken);
	portYIELD_FROM_ISR(hp_task_woken);
}

// Utilities
// takes 8 readings, finds average to filter out noise
static int read_averaged_ADC(int pin)
{
	const int N = 8;		//small, fast filter
	int sum = 0;
	for (int i = 0; i < N; i++) {
		sum += analogRead(pin);
	}
	return sum / N;
}

bool button_pressed(int pin)
{
	static bool initialized = false;
	static bool last_state[40];

	if (!initialized) {
		for (int i = 0; i < 40; i++) {
			last_state[40] = HIGH;
		}
		initialized = true;
	}

	bool current_state = digitalRead(pin);
	bool pressed = false;

	if (last_state[pin] == HIGH && current_state == LOW) {
		pressed = true;
	}
	last_state[pin] = current_state;
	return pressed;
}

void send_log(String message);

// Tasks
// water sensor task -> waits for timer tick, samples ADC, pushes value

void water_sensor_task(void *pvParameters) 
{
	int value = 0;

	for (;;) {		// infinite loop
		//block till ISR notifies
		ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

		value = read_averaged_ADC(WATER_SENSOR_PIN);
		latest_water_sensor_value = value;
		// non-blocking send preferred for real-time systems
		xQueueOverwrite(water_sensor_queue, &value);
	}
}

// dht task

void dht_task(void *pvParameters) 
{
	TempAndHumidity data;

	for (;;) {
		data = dht.getTempAndHumidity();

		if (!isnan(data.temperature) && !isnan(data.humidity)) {
			latest_temp = data.temperature;
			latest_humidity = data.humidity;
		}
		// dht22 max sampling rate = 0.5Hz (once every two seconds)
		vTaskDelay(pdMS_TO_TICKS(2000));
	}
}

// ldr task
void ldr_task(void *pvParameters) 
{
	for (;;) {
		// read analog value from LDR
		ldr_value = analogRead(LDR_PIN);

		vTaskDelay(pdMS_TO_TICKS(2000));
	}
}

// control task -> applies hysteresis, outputs pump state

void control_task(void *pvParameters)
{
	//int water_sensor_value = latest_water_sensor_value;
	//control_msg_t ctrl = {false, false, false};
	control_msg_t ctrl;
	command_msg_t cmd;

	for (;;) {
		
		int water_sensor_value = latest_water_sensor_value;

		if (xQueueReceive(command_queue, &cmd, 0) == pdTRUE) {
			switch (cmd.type) 
			{
				case CMD_TOGGLE_PUMP:
					manual_pump = true;
					manual_override = true;
					ctrl.pump = !ctrl.pump;
					send_log(String("Pump manually ") + (ctrl.pump ? "activated" : "deactivated"));
					break;
				case CMD_TOGGLE_FAN:
					manual_fan = true;
					manual_override = true;
					ctrl.fan = !ctrl.fan;
					send_log(String("Fan manually ") + (ctrl.fan ? "activated" : "deactivated"));
					break;
				case CMD_TOGGLE_LIGHTS:
					manual_lights = true;
					manual_override = true;
					ctrl.light = !ctrl.light;
					send_log(String("Lights manually ") + (ctrl.light ? "activated" : "deactivated"));
					break;
				case CMD_AUTO:
					manual_pump = false;
					manual_fan = false;
					manual_lights = false;
					manual_override = false;
					send_log("Automatic mode enabled");
					break;

				default:
					break;
			}
		}

		if (!manual_pump) {
			if (water_sensor_value < LOWER_WATER_THRESHOLD) {
				ctrl.pump = true;	//low, pump ON
			} else if (water_sensor_value > UPPER_WATER_THRESHOLD) {
				ctrl.pump = false;	//high, pump OFF
			}	
		}
		if (!manual_fan) {
			if (latest_temp > TEMP_HIGH) {
				ctrl.fan = true;
			} else if (latest_temp < TEMP_LOW) {
				ctrl.fan = false;
			}
		}
		if (!manual_lights) {
			if (ldr_value < LIGHT_THRESHOLD) {
				ctrl.light = true;	//low, lights ON
			} else {
				ctrl.light = false;	//high, lights OFF
			}
		}

		xQueueOverwrite(control_queue, &ctrl);
	}
}

// actuator task
void actuator_task(void *pvParameters)
{
	control_msg_t ctrl;
	
	for (;;) {	
		if (xQueueReceive(control_queue, &ctrl, portMAX_DELAY) == pdTRUE) {
			// relay logic (assuming active low)
			digitalWrite(RELAY_PIN, ctrl.pump ? LOW: HIGH);
			digitalWrite(FAN_PIN, ctrl.fan ? LOW: HIGH);
			//digitalWrite(LED_PIN, ctrl.light ? LOW: HIGH);
			digitalWrite(LED_PIN, ctrl.light ? HIGH: LOW);

			//update globals for UI 
			pump_state = ctrl.pump;
			fan_state = ctrl.fan;
			light_state = ctrl.light;
		}
	}
}

void button_task(void *pvParameters)
{
	command_msg_t cmd;

	for (;;) {
		if (button_pressed(PUMP_BUTTON_PIN)) {
			cmd.type = CMD_TOGGLE_PUMP;
			Serial.println("Pump button pressed.");
			xQueueSend(command_queue, &cmd, 0);
		}
		if (button_pressed(FAN_BUTTON_PIN)) {
			cmd.type = CMD_TOGGLE_FAN;
			Serial.println("Fan button pressed.");
			xQueueSend(command_queue, &cmd, 0);
		}
		/*
		if (button_pressed(LIGHT_BUTTON_PIN)) {
			cmd.type = CMD_TOGGLE_LIGHTS;
			Serial.println("Light button pressed.");
			xQueueSend(command_queue, &cmd, 0);
		}
		*/
		vTaskDelay(pdMS_TO_TICKS(20));
	}
}

void lcd_task(void *pvParameters) 
{
	char line1[17];	
	char line2[17];	

	control_msg_t last_ctrl;
	struct tm timeinfo;

	for (;;) {
		//getLocalTime(&timeinfo);

		xQueuePeek(control_queue, &last_ctrl, 0);

		if (xQueuePeek(control_queue, &last_ctrl, 0) != pdTRUE) {
			last_ctrl.pump = false;
			last_ctrl.fan = false;
			last_ctrl.light = false;
		}

		snprintf(line1, sizeof(line1), "P:%S F:%s L:%s", last_ctrl.pump ? "ON": "OFF", 
				last_ctrl.fan ? "ON": "OFF", last_ctrl.light ? "ON": "OFF");
		//snprintf(line2, sizeof(line2), "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);

		if (!getLocalTime(&timeinfo)) {
			snprintf(line2, sizeof(line2), "Time Error");
		} else {
			snprintf(line2, sizeof(line2), "%02d:%02d T:%.2fC", timeinfo.tm_hour, timeinfo.tm_min, latest_temp);
		}

		//lcd.clear();
		
		//lcd.setCursor(...);
		//lcd.print("           ");	//overwrite old text

		lcd.setCursor(0, 0);
		lcd.print("           ");	//overwrite old text
		lcd.setCursor(0, 0);
		lcd.print(line1);

		lcd.setCursor(0, 1);
		lcd.print("           ");	
		lcd.setCursor(0, 1);
		lcd.print(line2);

		vTaskDelay(pdMS_TO_TICKS(5000));	
	}
}

/*
void lcd_task(void *pvParameters) 
{
	char line1[17];	
	char line2[17];	

	//control_msg_t last_ctrl = {false, false, false};
	control_msg_t last_ctrl;
	struct tm timeinfo;

	for (;;) {
		getLocalTime(&timeinfo);
		xQueuePeek(control_queue, &last_ctrl, 0);

		snprintf(line1, sizeof(line1), "L:%s F:%s", ldr_value, last_ctrl.light ? "ON": "OFF");
		snprintf(line2, sizeof(line2), "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);

		lcd.clear();
		
		lcd.setCursor(...);
		lcd.print("           ");	//overwrite old text
		

		lcd.setCursor(0, 0);
		lcd.print(line1);

		lcd.setCursor(0, 1);
		lcd.print(line2);

		vTaskDelay(pdMS_TO_TICKS(5000));	
	}
}
*/

//logger task
void logger_task(void *pvParameters)
{
	//control_msg_t last_ctrl = {false, false, false};
	control_msg_t last_ctrl;

	for (;;) {
		//peek without consuming (diagnostics only)
		//xQueuePeek(water_sensor_queue, &last_water_value, 0);
		xQueuePeek(control_queue, &last_ctrl, 0);

		Serial.printf("[%lu ms]", millis());
		Serial.printf(" Water Sensor value: %d | Pump Status: %s | Temperature: %.2fC | Humidity: %.2f%% | Fan Status: %s | Light intensity: %d | Light Status: %s| Heap Size: %u\n", 
				latest_water_sensor_value, last_ctrl.pump ? "ON" : "OFF", latest_temp, latest_humidity, 
				last_ctrl.fan ? "ON": "OFF", ldr_value, last_ctrl.light ? "ON" : "OFF", (unsigned)esp_get_free_heap_size());
		Serial.printf("\n");

		vTaskDelay(pdMS_TO_TICKS(5000));	//puts task to sleep for 1s
	}
}

void notify_clients()
{
	JsonDocument doc;
	control_msg_t ctrl = {false, false, false};
	xQueuePeek(control_queue, &ctrl, 0);

	int lat_hum = 40;
	int lat_temp = 22;

	doc["type"] = "state";
	doc["temp"] = lat_temp;
	doc["humidity"] = lat_hum;
	doc["water"] = latest_water_sensor_value;
	doc["light"] = ldr_value;

	doc["pump"] = pump_state;
	doc["fan"] = fan_state;
	doc["lights"] = light_state;

	doc["auto"] = !manual_override;

	String json;
	serializeJson(doc, json);
	//Serial.println(json);
	//Serial.println("\n");
	ws.textAll(json);
}

void send_log(String message)
{
	JsonDocument doc;

	doc["type"] = "log";
	doc["message"] = message;

	String json;
	serializeJson(doc, json);

	ws.textAll(json);
	Serial.println(message);
	Serial.println("\n");
}

//WebSocket event handler
void on_web_socket_event(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len)
{
	if (type != WS_EVT_DATA) return;

	String msg;
	msg.reserve(len);

	for (size_t i = 0; i < len; i++) {
		msg += (char)data[i];
	}

	msg.trim();
	send_log("Command received: " + msg);

	command_msg_t cmd;

	if (msg == "TOGGLE_PUMP") {
		cmd.type = CMD_TOGGLE_PUMP;
	} else if (msg == "TOGGLE_FAN") {
		cmd.type = CMD_TOGGLE_FAN;
	} else if (msg == "TOGGLE_LIGHTS") {
		cmd.type = CMD_TOGGLE_LIGHTS;
	} else if (msg == "AUTO") {
		cmd.type = CMD_AUTO;
	} else {
		send_log("Unknown command received!");
		return;
	}
	
	xQueueSend(command_queue, &cmd, 0);
	send_log("Command queued: " + msg);
}


void web_update_task(void *pvParameters)
{
	while (true) {
		notify_clients();
		vTaskDelay(pdMS_TO_TICKS(1000));
	}
}


void init_web_server()
{
	ws.onEvent(on_web_socket_event);
	server.addHandler(&ws);
	server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

	server.begin();
}


void setup()
{
	Serial.begin(115200);

	pinMode(RELAY_PIN, OUTPUT);
	pinMode(FAN_PIN, OUTPUT);
	pinMode(LED_PIN, OUTPUT);
	pinMode(PUMP_BUTTON_PIN, INPUT_PULLUP);
	pinMode(FAN_BUTTON_PIN, INPUT_PULLUP);
	//pinMode(LIGHT_BUTTON_PIN, INPUT_PULLUP);

	digitalWrite(RELAY_PIN, HIGH);	//OFF, active low relay
	digitalWrite(FAN_PIN, HIGH);	//OFF, active low FAN
	dht.setup(DHT_PIN, DHTesp::DHT22);
	
	// queues (length 1 + overwrite = latest value only)
	water_sensor_queue = xQueueCreate(1, sizeof(int));
	control_queue = xQueueCreate(1, sizeof(control_msg_t));
	command_queue = xQueueCreate(5, sizeof(command_msg_t));
	button_queue = xQueueCreate(5, sizeof(button_event));

	Serial.println("pp");
	if (!control_queue || !command_queue || !button_queue) {
		Serial.println("Queue creation failed");
		
		while (1) {
			vTaskDelay(pdMS_TO_TICKS(1000));
		}
	} 

	Wire.begin(21, 22);
	Serial.println("Scanning ...");

	for (byte address = 1; address < 127; address++) {
		Wire.beginTransmission(address);

		if (Wire.endTransmission() == 0) {
			Serial.print("Found I2C device at 0x");
			Serial.println(address, HEX);
		}
	}


	Serial.println("pp");
	lcd.init();
	lcd.backlight();
	lcd.clear();

	initWiFi();
	initFS();
	init_web_server();
	init_time();

	// create tasks
	xTaskCreate(web_update_task, "Web Update", 2048, NULL, 1, NULL);
	xTaskCreatePinnedToCore(water_sensor_task, "Water Sensor", 2048, NULL, 2, &water_sensor_task_handle, 1);
	xTaskCreatePinnedToCore(water_sensor_task, "Water Sensor", 2048, NULL, 2, NULL, 1);
	xTaskCreatePinnedToCore(dht_task, "DHT", 2048, NULL, 1, NULL, 1);
	xTaskCreate(button_task, "Button Check", 2048, NULL, 1, NULL);
	xTaskCreate(ldr_task, "LDR", 2048, NULL, 1, NULL);
	xTaskCreatePinnedToCore(control_task, "Control", 2048, NULL, 2, NULL, 1);
	xTaskCreatePinnedToCore(actuator_task, "Actuator", 2048, NULL, 2, NULL, 1);
	xTaskCreatePinnedToCore(logger_task, "Logger", 2048, NULL, 2, NULL, 0);
	xTaskCreatePinnedToCore(lcd_task, "LCD", 4096, NULL, 1, NULL, 0);

	// timer: 1Hz sampling
	timer = timerBegin(0, 80, true);	// 80 prescaler -> 1us tick
	timerAttachInterrupt(timer, &onTimer, true);
	timerAlarmWrite(timer, 1000000, true); 
	timerAlarmEnable(timer);
}

void loop()
{}


