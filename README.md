***** Greenhouse Controller Project *****

The code builds an automated greenhouse control system using FreeRTOS to handle multiplebackground processes concurrently.

* Configuration & Initialization *

The code begins by importing essential libraries for web connectivity (WiFi,
ESPAsyncWebServer), hardware communication (Wire, LiquidCrystal_I2C), data management
(ArduinoJson), and RTOS multitasking. 
It defines the hardware pins for a water sensor,relay-driven pump, DHT temperature/humidity sensor, fan, Light Dependent Resistor (LDR),status LED, and manual buttons.

* Sensor Data Collection Tasks *

Instead of using a standard linear loop, the code splits data gathering into independent
background tasks:
● Water Sensor: Activated precisely once per second by a hardware timer interrupt
(onTimer). It takes eight consecutive analog readings to smooth out signal noise before
passing the data to the system.

● DHT & LDR Tasks: The DHT task polls the temperature and humidity every two seconds
(the optimal physical limit for a DHT22 sensor), while the LDR task concurrently
measures surrounding ambient light levels.


* System Logic & Control * 

The control task acts as the processing brain. It manages two operating modes: Automatic andManual Override.
In automatic mode, it applies hysteresis (on/off cushion thresholds) to prevent hardware jitter:
1. Turns the Pump on/off based on upper and lower water levels.
2. Toggles the Fan depending on critical temperature thresholds.
3. Triggers the Lights based on ambient brightness.
If user interactions arrive from the physical buttons via the button task or over the web
dashboard, this task locks out automation for that specific component and shifts it to manualcontrol.


* Actuators, User Interface, & Networking *

● Actuators: The actuator task listens for changes decided by the control task and
physically switches the external pins (Relays, Fans, LEDs).
● Local UI: The LCD task prints localized real-time telemetry and target system states ontoan attached I2C LCD screen every 5 seconds.
● Remote Web UI: The init_web_server() function spins up a background WebSocket
server. The web_update_task converts current telemetry to a JSON string every second
and pushes it over the network to all active browser connections, while the
on_web_socket_event() translates incoming remote user clicks back into system commands.
● Diagnostics: The logger task continuously prints an organized diagnostic log of all data,statuses, and available internal memory (heap size) to the serial monitor.
It initializes structural definitions (structs) for systemparameters, a hardware timer, and FreeRTOS queues/task handles used to safely move dataacross different concurrent tasks.

* Setup & Execution Loop *
The setup() function configures all I/O pin behaviors, provisions communication queues,
structures FreeRTOS tasks to run concurrently across the ESP32's dual computing cores, and enables the hardware timer alarm. The standard loop() function is left entirely empty becauseFreeRTOS handles all ongoing operations in the background.


