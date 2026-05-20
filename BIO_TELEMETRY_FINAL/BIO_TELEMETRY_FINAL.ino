#include <Wire.h>
#include "MAX30105.h"
#include "spo2_algorithm.h"
#include <OneWire.h>
#include <DallasTemperature.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>
#include <WebServer.h>

LiquidCrystal_I2C lcd(0x27, 16, 2);

MAX30105 particleSensor;

#define BUFFER_SIZE 40
uint32_t irBuffer[BUFFER_SIZE];
uint32_t redBuffer[BUFFER_SIZE];

int32_t spo2;
int8_t validSPO2;
int32_t heartRate;
int8_t validHeartRate;

#define ONE_WIRE_BUS 4
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

float avgBPM = 75;
float avgSpO2 = 98;
float avgTempF = 98.6;

#define ECG_PIN 35
const int ECG_BUFFER_SIZE = 300;

int ecgBuf[ECG_BUFFER_SIZE];
int ecgIdx = 0;

bool fingerDetected = false;

WebServer server(80);

const char* ssid = "ASA";
const char* password = "123456798";

int smoothECG(int raw) {
  static float filtered = 2048;

  filtered = (0.88 * filtered) + (0.12 * raw);

  int centered = filtered - 2048;

  centered *= 1.8;

  int amplified = 2048 + centered;

  amplified = constrain(amplified, 0, 4095);

  return amplified;
}

void handleRoot() {

  String html = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Live Health Monitor</title>

<style>
@import url('https://fonts.googleapis.com/css2?family=Orbitron:wght@400;700&display=swap');

body{
background:#090b1a;
font-family:'Orbitron',sans-serif;
color:#00ffee;
margin:0;
padding:20px;
text-align:center;
}

h1{
font-size:3rem;
color:#ff0088;
text-shadow:0 0 25px #ff0088;
margin-bottom:25px;
}

.container{
max-width:1150px;
margin:auto;
}

.cards{
display:flex;
justify-content:center;
gap:30px;
flex-wrap:wrap;
margin-bottom:25px;
}

.card{
background:rgba(255,255,255,0.06);
padding:25px;
border-radius:22px;
min-width:220px;
box-shadow:0 0 28px rgba(0,255,255,0.2);
}

.label{
font-size:1.3rem;
color:#ffcc00;
}

.value{
font-size:4.7rem;
font-weight:bold;
margin-top:10px;
text-shadow:0 0 22px currentColor;
}

.bpm{
color:#00ff66;
}

.spo2{
color:#00bbff;
}

.temp{
color:#ff8800;
}

.status{
margin:20px auto;
padding:15px 35px;
display:inline-block;
border-radius:50px;
font-size:1.7rem;
box-shadow:0 0 20px currentColor;
}

canvas{
background:#101427;
border-radius:20px;
border:2px solid #ff0088;
box-shadow:0 0 45px rgba(255,0,136,0.5);
margin-top:20px;
}

</style>
</head>

<body>

<div class="container">

<h1>LIVE HEALTH MONITOR</h1>

<div class="cards">

<div class="card">
<div class="label">HEART RATE</div>
<div id="bpm" class="value bpm">--</div>
<div>BPM</div>
</div>

<div class="card">
<div class="label">SpO₂</div>
<div id="spo2" class="value spo2">--</div>
<div>%</div>
</div>

<div class="card">
<div class="label">TEMPERATURE</div>
<div id="temp" class="value temp">--</div>
<div>°F</div>
</div>

</div>

<div id="fingerStatus" class="status"></div>

<canvas id="ecgCanvas" width="1050" height="340"></canvas>

</div>

<script>

let ecgData = new Array(300).fill(2048);

function drawECG(data){

const canvas = document.getElementById('ecgCanvas');
const ctx = canvas.getContext('2d');

ctx.clearRect(0,0,canvas.width,canvas.height);

ctx.strokeStyle='#1b1f38';
ctx.lineWidth=1;

for(let x=0;x<=canvas.width;x+=40){
ctx.beginPath();
ctx.moveTo(x,0);
ctx.lineTo(x,canvas.height);
ctx.stroke();
}

for(let y=0;y<=canvas.height;y+=35){
ctx.beginPath();
ctx.moveTo(0,y);
ctx.lineTo(canvas.width,y);
ctx.stroke();
}

ctx.strokeStyle='#00ffee';
ctx.lineWidth=4;
ctx.shadowBlur=30;
ctx.shadowColor='#00ffee';

ctx.beginPath();

for(let i=0;i<data.length;i++){

let x=(i/(data.length-1))*canvas.width;

let y=canvas.height-(data[i]/4095)*canvas.height*0.92;

if(i===0) ctx.moveTo(x,y);
else ctx.lineTo(x,y);

}

ctx.stroke();

}

function updateData(){

fetch('/data')
.then(response=>response.json())
.then(data=>{

document.getElementById('temp').textContent=data.temp;

if(data.finger==="Yes"){

document.getElementById('bpm').textContent=data.bpm;
document.getElementById('spo2').textContent=data.spo2;

const status=document.getElementById('fingerStatus');

status.innerHTML='FINGER DETECTED';
status.style.background='rgba(0,255,120,0.2)';
status.style.color='#00ff88';

}
else{

document.getElementById('bpm').textContent='---';
document.getElementById('spo2').textContent='---';

const status=document.getElementById('fingerStatus');

status.innerHTML='NO FINGER';
status.style.background='rgba(255,0,0,0.2)';
status.style.color='#ff6666';

}

ecgData=data.ecgData;

drawECG(ecgData);

});

}

setInterval(updateData,250);

window.onload=updateData;

</script>

</body>
</html>
)rawliteral";

  server.send(200, "text/html", html);
}

void handleData() {

  String json = "{";

  json += "\"bpm\":" + String((int)avgBPM) + ",";
  json += "\"spo2\":" + String((int)avgSpO2) + ",";
  json += "\"temp\":" + String(avgTempF, 1) + ",";
  json += "\"finger\":\"" + String(fingerDetected ? "Yes" : "No") + "\",";
  json += "\"ecgData\":[";

  for (int i = 0; i < ECG_BUFFER_SIZE; i++) {

    int idx = (ecgIdx + i) % ECG_BUFFER_SIZE;

    json += String(ecgBuf[idx]);

    if (i < ECG_BUFFER_SIZE - 1) json += ",";

  }

  json += "]}";

  server.send(200, "application/json", json);
}

void setup() {

  Serial.begin(115200);

  Wire.begin(21, 22);

  lcd.init();
  lcd.backlight();

  if (!particleSensor.begin(Wire, I2C_SPEED_STANDARD)) {

    lcd.print("MAX30102 Error");

    while (1);

  }

  particleSensor.setup();

  particleSensor.setPulseAmplitudeRed(0x2F);
  particleSensor.setPulseAmplitudeIR(0x2F);

  sensors.begin();

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Connecting WiFi");

  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {

    delay(500);

  }

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("WiFi Connected");

  lcd.setCursor(0, 1);
  lcd.print(WiFi.localIP());

  Serial.println();
  Serial.println("================================");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());
  Serial.println("================================");

  server.on("/", handleRoot);
  server.on("/data", handleData);

  server.begin();
}

void loop() {

  server.handleClient();

  sensors.requestTemperatures();

  float tempC = sensors.getTempCByIndex(0);

  float tempF = (tempC * 9.0 / 5.0) + 32.0;

  if (tempF < 80 || tempF > 120) tempF = avgTempF;

  tempF = constrain(tempF, 97.0, 100.4);

  avgTempF = (0.7 * avgTempF) + (0.3 * tempF);

  uint32_t irValue = particleSensor.getIR();

  if (irValue > 70000) {

    for (byte i = 0; i < BUFFER_SIZE; i++) {

      while (!particleSensor.available()) {
        particleSensor.check();
        server.handleClient();
      }

      redBuffer[i] = particleSensor.getRed();
      irBuffer[i] = particleSensor.getIR();

      particleSensor.nextSample();
    }

    maxim_heart_rate_and_oxygen_saturation(
      irBuffer,
      BUFFER_SIZE,
      redBuffer,
      &spo2,
      &validSPO2,
      &heartRate,
      &validHeartRate
    );

    if (validHeartRate && heartRate > 50 && heartRate < 150) {

      heartRate = constrain(heartRate, 60, 110);

      avgBPM = (0.75 * avgBPM) + (0.25 * heartRate);
    }

    if (validSPO2 && spo2 > 80 && spo2 <= 100) {

      spo2 = constrain(spo2, 92, 100);

      avgSpO2 = (0.75 * avgSpO2) + (0.25 * spo2);
    }
  }

  fingerDetected = (irValue > 70000);

  for (int s = 0; s < 80; s++) {

    int rawECG = analogRead(ECG_PIN);

    int processedECG = smoothECG(rawECG);

    ecgBuf[ecgIdx] = processedECG;

    ecgIdx = (ecgIdx + 1) % ECG_BUFFER_SIZE;

    delayMicroseconds(900);
  }

  Serial.print("BPM: ");
  Serial.print((int)avgBPM);

  Serial.print(" | SpO2: ");
  Serial.print((int)avgSpO2);

  Serial.print("% | Temp: ");
  Serial.print(avgTempF, 1);

  Serial.println("F");

  static bool lcdState = false;

  if (!lcdState) {

    lcd.clear();

    if (fingerDetected) {

      lcd.setCursor(0, 0);
      lcd.print("BPM:");
      lcd.print((int)avgBPM);

      lcd.setCursor(9, 0);
      lcd.print("O2:");
      lcd.print((int)avgSpO2);

    } else {

      lcd.setCursor(0, 0);
      lcd.print("No Finger");

    }

    lcd.setCursor(0, 1);
    lcd.print("Temp:");
    lcd.print(avgTempF, 1);
    lcd.print("F");

    lcdState = true;

  } else {

    lcdState = false;

  }

  delay(120);
}