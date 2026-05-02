#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <ArduinoJson.h>
#include <ESP32Servo.h>
#include <FirebaseESP32.h>
#include "secrets.h"

FirebaseData firebaseData;
FirebaseConfig config;
FirebaseAuth auth;

WiFiClientSecure client;
UniversalTelegramBot bot(BOT_TOKEN, client);

// hardware 
Servo motorTranca;
const int pinoMotor = 12;
const int pinoBotao = 13;
const int pinoSensorPorta = 14;

// var de estado e tempo
int estadoSistema = 0;
unsigned long cronometroEstado1 = 0; 
const long tempoLimite = 15000; 
unsigned long ultimaChecagemTelegram = 0;
const long intervaloChecagem = 1000;

// interrupção
volatile bool campainhaAcionada = false;
unsigned long ultimoToqueCampainha = 0;

// menu dos botoes do telegram
String menuBotoes = "[[\"/abrir\", \"/status\"]]";

// func que "pausa" o processador quando o botao é apertado
void IRAM_ATTR detectarCampainha() {
  campainhaAcionada = true;
}

// func do firebase
void registrarNoFirebase(String evento) {
  String caminho = "/portaria/historico";
  
  FirebaseJson json;
  json.set("evento", evento);
  
  // pega a hora real do relgoio interno
  time_t agora;
  time(&agora);
  
  // firebase e js esperam o tempo em ms, então multiplicamos por 1000
  json.set("timestamp", agora * 1000);

  if (Firebase.pushJSON(firebaseData, caminho, json)) {
    Serial.println("Firebase: Evento registrado -> " + evento);
  } else {
    Serial.println("Firebase: Erro ao registrar -> " + firebaseData.errorReason());
  }
}

void setup() {
  Serial.begin(115200);
  
  pinMode(pinoBotao, INPUT_PULLUP);
  pinMode(pinoSensorPorta, INPUT_PULLUP);
  
  attachInterrupt(digitalPinToInterrupt(pinoBotao), detectarCampainha, FALLING);
  
  ESP32PWM::allocateTimer(1);
  ESP32PWM::allocateTimer(2);
  ESP32PWM::allocateTimer(3);
  motorTranca.setPeriodHertz(50);
  motorTranca.attach(pinoMotor, 500, 2400); 
  motorTranca.write(0); 
  
  Serial.println("\nConectando ao Wi-Fi...");
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWi-Fi Conectado!");

  // configura o relógio interno da esp pro fuso horário de brasilia (UTC-3)
  // -3 horas * 3600 segundos = -10800
  configTime(-10800, 0, "pool.ntp.org");
  Serial.println("Relógio sincronizado com a internet!");
  
  client.setCACert(TELEGRAM_CERTIFICATE_ROOT);
  bot.sendMessageWithReplyKeyboard(CHAT_ID, "Portaria IoT Online!\nOs botões de controle estão no menu abaixo.", "", menuBotoes, true);
  
  // inicialização do firebase
  config.host = FIREBASE_HOST;
  config.signer.tokens.legacy_token = FIREBASE_AUTH;
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);
}

void checarMensagens(int numNovasMensagens) {
  for (int i = 0; i < numNovasMensagens; i++) {
    String chat_id = bot.messages[i].chat_id;
    String texto = bot.messages[i].text;

    if (chat_id != CHAT_ID) continue;

    if (texto == "/abrir") {
      if (estadoSistema == 0) {
        Serial.println("Comando remoto recebido. Destrancando...");
        bot.sendMessage(chat_id, "Acesso autorizado! Destrancando a porta...", "");
        
        motorTranca.write(90); 
        estadoSistema = 1;
        cronometroEstado1 = millis();
        
        // gatilho firebase
        registrarNoFirebase("Acesso Autorizado (Telegram)");
        
      } else {
        bot.sendMessage(chat_id, "A porta já está destrancada ou aberta.", "");
      }
    } 
    else if (texto == "/status") {
      if (estadoSistema == 0) bot.sendMessage(chat_id, "Status: Trancada", "");
      else if (estadoSistema == 1) bot.sendMessage(chat_id, "Status: Destrancada (Aguardando visitante abrir a maçaneta)", "");
      else bot.sendMessage(chat_id, "Status: Aberta", "");
    }
  }
}

void loop() {
  // 1. checa telegram
  if (millis() - ultimaChecagemTelegram > intervaloChecagem) {
    int numNovasMensagens = bot.getUpdates(bot.last_message_received + 1);
    while (numNovasMensagens) {
      checarMensagens(numNovasMensagens);
      numNovasMensagens = bot.getUpdates(bot.last_message_received + 1);
    }
    ultimaChecagemTelegram = millis();
  }

  // 2. le sensor da porta
  int estadoIma = digitalRead(pinoSensorPorta);

  // "maquina de estados"
  if (estadoSistema == 0 && campainhaAcionada) {
    campainhaAcionada = false;
    
    if (millis() - ultimoToqueCampainha > 5000) { 
      Serial.println("Campainha acionada! Notificando proprietária...");
      bot.sendMessageWithReplyKeyboard(CHAT_ID, "Campainha acionada! Tem alguém na porta.\nUse o botão abaixo para liberar a entrada.", "", menuBotoes, true);
      ultimoToqueCampainha = millis();
      
      // gatilho firebase
      registrarNoFirebase("Campainha Tocada");
    }
  }
  
  // estado 1: porta foi destrancada pelo celular
  if (estadoSistema == 1) {
    if (estadoIma == HIGH) { 
      Serial.println("Sensor: Porta foi aberta pelo visitante.");
      estadoSistema = 2;
      
      // gatilho firebase
      registrarNoFirebase("Porta Aberta (Sensor)");
      
      delay(500);
    } 
    else if (millis() - cronometroEstado1 > tempoLimite) { 
      Serial.println("TIMEOUT! Trancando por segurança...");
      motorTranca.write(0);
      estadoSistema = 0;
      bot.sendMessage(CHAT_ID, "[!] O visitante não abriu a porta a tempo. Trancada novamente por segurança.", "");
      
      // gatilho firebase
      registrarNoFirebase("Acesso Negado (Timeout)");
    }
  }
  
  // estado 2: porta tava aberta e agora foi encostada
  if (estadoSistema == 2 && estadoIma == LOW) {
    Serial.println("Sensor: Porta encostada. Trancando...");
    motorTranca.write(0); 
    estadoSistema = 0;
    bot.sendMessage(CHAT_ID, "O visitante entrou. Porta fechada e trancada com sucesso.", "");
    
    // gatilho firebase
    registrarNoFirebase("Porta Fechada e Trancada");
    
    delay(500);
  }
}