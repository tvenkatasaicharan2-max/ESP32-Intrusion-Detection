/*  ESP32 smart alarm
    – PIR + reed sensors + keypad + Wi-Fi dashboard
    – Three users / passwords
    – 3 wrong attempts → lockout
    – Continuous alarm on intrusion during lockout
    – Simple statistical anomaly detection on PIR
*/

#include <WiFi.h>
#include <Keypad.h>
#include <WebServer.h>

// ---------- Wi-Fi ----------
const char* ssid     = "realme 10 Pro 5G";
const char* password = "h462js5t";

// ---------- Pins ----------
const uint8_t PIR_PIN    = 27;
const uint8_t REED_PIN   = 12;
const uint8_t BUZZER_PIN = 14;

// ---------- Keypad ----------
const byte ROWS = 4, COLS = 3;
char keys[ROWS][COLS] = {
  {'1','2','3'},
  {'4','5','6'},
  {'7','8','9'},
  {'*','0','#'}
};
byte rowPins[ROWS] = {21,19,18,5};
byte colPins[COLS] = {32,33,26};
Keypad keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

// ---------- Users ----------
struct User { String name; String pwd; };
User users[3] = { {"Alice","1111"},{"Bob","2222"},{"Carol","3333"} };

// ---------- Config ----------
const unsigned long DISARM_DURATION = 10000UL;
const unsigned long PIR_GRACE_MS    = 10000UL;
const int MAX_WRONG = 3;

// ---------- States ----------
bool disarmed=false, lockedOut=false;
unsigned long disarmUntil=0;
String disarmedBy="", lastUser="", lastEvent="System ready";
int wrongAttempts=0;

bool reedOpen=false, reedAlarm=false;
bool pirAlarm=false, pirDetected=false;
unsigned long pirDetectedAt=0;

// ---------- Buzzer control ----------
unsigned long lastBuzzerToggle=0;
bool buzzerState=false;
const unsigned long PIR_BEEP_ON=120, PIR_BEEP_OFF=120;
void beepOn(){ digitalWrite(BUZZER_PIN,HIGH); }
void beepOff(){ digitalWrite(BUZZER_PIN,LOW); }

// ---------- Simple anomaly detector ----------
float avgPirInterval=5000, avgPirDuration=1000;
unsigned long lastPirOn=0,lastPirOff=0;
bool pirPrev=false, aiAnomaly=false;
void updateAnomaly(bool pirNow){
  unsigned long now=millis();
  if(pirNow && !pirPrev){
    unsigned long interval=now-lastPirOff;
    if(lastPirOff) avgPirInterval=0.9*avgPirInterval+0.1*interval;
    lastPirOn=now;
  }else if(!pirNow && pirPrev){
    unsigned long dur=now-lastPirOn;
    if(lastPirOn) avgPirDuration=0.9*avgPirDuration+0.1*dur;
    lastPirOff=now;
  }
  if((now-lastPirOff)<2000 && (avgPirInterval<2000 || avgPirDuration>5000))
    aiAnomaly=true;
  pirPrev=pirNow;
}

// ---------- Web ----------
WebServer server(80);
void handleRoot(){
  String h="<!doctype html><html><head><meta charset='utf-8'>";
  h+="<script>function r(){fetch('/s').then(x=>x.text()).then(t=>s.innerHTML=t);}setInterval(r,1000);</script>";
  h+="<style>body{background:#111;color:#eee;font-family:Arial;text-align:center}div{background:#222;border-radius:8px;padding:12px;display:inline-block}</style>";
  h+="</head><body><h2>ESP32 Alarm</h2><div id='s'>Loading...</div></body></html>";
  server.send(200,"text/html",h);
}
void handleStatus(){
  String s="<p><b>System:</b> ";
  if(lockedOut) s+="<span style='color:red'>LOCKED OUT</span>";
  else if(disarmed) s+="<span style='color:orange'>DISARMED</span>";
  else s+="<span style='color:lightgreen'>ARMED</span>";
  s+="</p><p><b>Last event:</b> "+lastEvent+"</p>";
  s+="<p><b>Last user:</b> "+(lastUser==""?"None":lastUser)+"</p>";
  s+="<p>PIR: "+String(pirDetected?"Motion":"No motion")+"</p>";
  s+="<p>Reed: "+String(reedOpen?"OPEN":"CLOSED")+"</p>";
  s+="<p>Wrong attempts: "+String(wrongAttempts)+"/"+String(MAX_WRONG)+"</p>";
  if(aiAnomaly) s+="<h3 style='color:yellow'>AI anomaly detected</h3>";
  if(reedAlarm) s+="<h3 style='color:red'>Reed alarm</h3>";
  if(pirAlarm)  s+="<h3 style='color:orange'>Motion alarm</h3>";
  server.send(200,"text/html",s);
}

// ---------- Password submit ----------
void submitPassword(String &pwd){
  if(lockedOut){ lastEvent="Locked out – password refused"; Serial.println(lastEvent); return; }
  for(int i=0;i<3;i++){
    if(pwd==users[i].pwd){
      disarmed=true; disarmUntil=millis()+DISARM_DURATION;
      disarmedBy=users[i].name; lastUser=disarmedBy;
      wrongAttempts=0; reedAlarm=pirAlarm=false; lastEvent="Disarmed by "+disarmedBy;
      beepOff(); Serial.println(lastEvent); return;
    }
  }
  wrongAttempts++; lastEvent="Wrong password ("+String(wrongAttempts)+")";
  Serial.println(lastEvent);
  if(wrongAttempts>=MAX_WRONG){ lockedOut=true; lastEvent="LOCKOUT"; Serial.println("LOCKOUT engaged"); }
}

// ---------- Setup ----------
void setup(){
  Serial.begin(115200);
  pinMode(PIR_PIN,INPUT);
  pinMode(REED_PIN,INPUT_PULLUP);
  pinMode(BUZZER_PIN,OUTPUT);
  beepOff();

  WiFi.begin(ssid,password);
  Serial.print("Connecting Wi-Fi");
  for(int i=0;i<30 && WiFi.status()!=WL_CONNECTED;i++){ delay(500); Serial.print("."); }
  Serial.println();
  if(WiFi.status()==WL_CONNECTED) Serial.println(WiFi.localIP());

  server.on("/",handleRoot);
  server.on("/s",handleStatus);
  server.begin();
  lastEvent="System armed";
}

// ---------- Loop ----------
String keyBuffer="";
void loop(){
  server.handleClient();

  char k=keypad.getKey();
  if(k){
    Serial.print("Key: ");Serial.println(k);
    if(k=='*') keyBuffer="";
    else if(k=='#'){ String p=keyBuffer; keyBuffer=""; submitPassword(p); }
    else keyBuffer+=k;
  }

  int reedRaw=digitalRead(REED_PIN);
  reedOpen=(reedRaw==HIGH);
  bool pirNow=digitalRead(PIR_PIN);

  updateAnomaly(pirNow);

  static bool lockoutLatched=false;
  if(lockedOut && (reedOpen||pirNow)) lockoutLatched=true;

  // PIR detection logic
  if(pirNow && !pirDetected && !disarmed && !lockedOut){
    pirDetected=true; pirDetectedAt=millis(); lastEvent="Motion detected – 10 s to enter code";
    Serial.println(lastEvent);
  }
  if(pirDetected && !disarmed && !lockedOut && millis()-pirDetectedAt>=PIR_GRACE_MS){
    pirAlarm=true; lastEvent="PIR alarm triggered"; Serial.println(lastEvent);
  }
  if(disarmed){ pirDetected=false; pirAlarm=false; }

  // Reed alarm
  if(reedOpen && !disarmed && !lockedOut){ reedAlarm=true; lastEvent="Reed alarm"; }
  else if(!reedOpen && !lockedOut) reedAlarm=false;

  // ---- Buzzer rules ----
  if(lockoutLatched){ beepOn(); lastEvent="LOCKOUT alarm"; }
  else if(reedAlarm){ beepOn(); }
  else if(pirAlarm){
    unsigned long now=millis();
    unsigned long phase=buzzerState?PIR_BEEP_ON:PIR_BEEP_OFF;
    if(now-lastBuzzerToggle>=phase){
      buzzerState=!buzzerState; lastBuzzerToggle=now;
      digitalWrite(BUZZER_PIN,buzzerState);
    }
  }else beepOff();

  // ---- Disarm re-arm logic ----
  if(disarmed && millis()>=disarmUntil && !reedOpen){
    disarmed=false; disarmedBy=""; lastEvent="System re-armed"; Serial.println(lastEvent);
  }

  // ---- Simple AI alert sound ----
  if(aiAnomaly && !lockedOut){
    Serial.println("⚠ AI anomaly detected");
    for(int i=0;i<3;i++){ beepOn(); delay(100); beepOff(); delay(100); }
    aiAnomaly=false;
  }

  delay(30);
}