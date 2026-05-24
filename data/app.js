let ws = new WebSocket(`ws://${location.host}/ws`);

function addLog(message) {
	let logger = document.getElementById("logger");

	let line = document.createElement("div");
	line.textContent = `[${new Date().toLocaleTimeString()}] ${message}`;

	//logger.appendChild(line);
	//logger.scrollTop = logger.scrollHeight()
	logger.prepend(line);
}

ws.onmessage = (event) => {

	let data = JSON.parse(event.data);

	// Monitoring stats
	document.getElementById("temp").innerText = (data.temp).toFixed(2) + " °C";
	document.getElementById("humidity").innerText = (data.humidity).toFixed(2) + " %";
	document.getElementById("light").innerText = data.light;
	document.getElementById("water").innerText = data.water;

	// Plant section
	document.getElementById("tempCurrent").innerText = data.temp.toFixed(1) + " °C";
	document.getElementById("humidityCurrent").innerText = data.humidity.toFixed(1) + " %";
	document.getElementById("lightCurrent").innerText = data.light + " %";
	document.getElementById("waterCurrent").innerText = data.water + " %";

	// Device status
	updateStatus("fanStatus", data.fan);
	updateStatus("pumpStatus", data.pump);
	updateStatus("lightStatus", data.lights);

	// Auto/manual globe
	let globe = document.getElementById("globe");
	let modeText = document.getElementById("modeText");

	if (data.auto) {
		globe.classList.add("spin");
		modeText.innerText = "AUTO MODE";
	} else {
		globe.classList.remove("spin");
		modeText.innerText = "MANUAL MODE";
	}

	addLog("State updated from ESP32");
	console.log(data);
};

function updateStatus(id, active) {

	let el = document.getElementById(id);

	if (active) {
		el.innerText = "Active";
		el.className = "status active";
	} else {
		el.innerText = "Inactive";
		el.className = "status inactive";
	}
}

function togglePump() {
	ws.send("TOGGLE_PUMP");
	addLog("Command sent: TOGGLE_PUMP");
}

function toggleFan() {
	ws.send("TOGGLE_FAN");
	addLog("Command sent: TOGGLE_FAN");
}

function toggleLights() {
	ws.send("TOGGLE_LIGHTS");
	addLog("Command sent: TOGGLE_LIGHTS");
}

function setAuto() {
	ws.send("AUTO");
	addLog("Command sent: AUTO");
}
