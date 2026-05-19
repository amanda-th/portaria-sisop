#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <ArduinoJson.h>
#include <ESP32Servo.h>
#include <FirebaseESP32.h>
#include "secrets.h"

// queue e mutex
SemaphoreHandle_t mutexEstado;
QueueHandle_t filaComandos;
QueueHandle_t filaLogs;

// hardware 
Servo motorTranca;
const int pinoMotor = 12;
const int pinoBotao = 13;
const int pinoSensorPorta = 14;

// rede etc
FirebaseData firebaseData;
FirebaseConfig config;
FirebaseAuth auth;

WiFiClientSecure client;
UniversalTelegramBot bot(BOT_TOKEN, client);

// var globais
int estadoSistema = 0;
unsigned long cronometroEstado1 = 0; 
const long tempoLimite = 15000;
unsigned long ultimaChecagemTelegram = 0;
const long intervaloChecagem = 1000;
String menuBotoes = "[[\"/abrir\", \"/status\"]]";

// interrupção
volatile bool campainhaAcionada = false;
unsigned long ultimoToqueCampainha = 0;
volatile unsigned long ultimoDebounce = 0;

void IRAM_ATTR detectarCampainha();
void registrarNoFirebase(String evento);
void checarMensagens(int numNovasMensagens);
void TarefaRede(void *pvParameters);
void maquinaDeEstados(int estadoIma);
void TarefaHardware(void *pvParameters);

void setup() {
  Serial.begin(115200);

  pinMode(pinoBotao, INPUT_PULLUP);
  pinMode(pinoSensorPorta, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(pinoBotao), detectarCampainha, FALLING);
  
  ESP32PWM::allocateTimer(1);
  motorTranca.setPeriodHertz(50);
  motorTranca.attach(pinoMotor, 500, 2400); 
  motorTranca.write(0); 

  Serial.println("\nConectando ao Wi-Fi...");
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) { 
    delay(500);
    Serial.print(".");
  }

  Serial.println("\nWi-fi conectado!");
  
  configTime(-10800, 0, "pool.ntp.org");
  Serial.println("Relógio sincronizado com a internet!");

  client.setCACert(TELEGRAM_CERTIFICATE_ROOT);

  config.host = FIREBASE_HOST;
  config.signer.tokens.legacy_token = FIREBASE_AUTH;
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  mutexEstado = xSemaphoreCreateMutex(); // 0
  filaComandos = xQueueCreate(5, sizeof(int));
  filaLogs = xQueueCreate(10, sizeof(int));

  // Fixando as tarefas nos núcleos
  xTaskCreatePinnedToCore(TarefaRede, "Rede_Telegram_Firebase", 10000, NULL, 1, NULL, 0); // Núcleo 0
  xTaskCreatePinnedToCore(TarefaHardware, "Hardware_Porta", 10000, NULL, 1, NULL, 1);     // Núcleo 1
}

void loop() {
  vTaskDelete(NULL); 
}

void IRAM_ATTR detectarCampainha() {
  if (millis() - ultimoDebounce > 300) {
    campainhaAcionada = true;
    ultimoDebounce = millis();
  }
}

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

void checarMensagens(int numNovasMensagens) {
  for (int i = 0; i < numNovasMensagens; i++) {
    String chat_id = bot.messages[i].chat_id;
    String texto = bot.messages[i].text;

    if (chat_id != CHAT_ID) continue;

    if (texto == "/abrir") {
      int podeAbrir = 0;
      // tentar implementar queue i guess
      if (xSemaphoreTake(mutexEstado, portMAX_DELAY) == pdTRUE) {
        if (estadoSistema == 0) {
          podeAbrir = 1;
        }
        xSemaphoreGive(mutexEstado); // devolve
      }

      if (podeAbrir){
        Serial.println("Comando remoto recebido. Destrancando...");
        bot.sendMessage(chat_id, "Acesso autorizado! Destrancando a porta...", "");
          
        int comando = 1; // cod pra abrir a porta
        xQueueSend (filaComandos, &comando, 0);

        vTaskDelay (pdMS_TO_TICKS(800)); // delay no nucleo de rede pro motor terminar o movimento e aenergia do usb estabilizar

        registrarNoFirebase("Acesso autorizado (Telegram)");
      } else {
        bot.sendMessage(chat_id, "A porta já está destrancada ou aberta.", "");
      }
    }

    else if (texto == "/status") {
      int estadoTemporario = 0;
      // sera de aqui ser um semaforo tambem?
      if (xSemaphoreTake(mutexEstado, portMAX_DELAY) == pdTRUE){
        estadoTemporario = estadoSistema;
        xSemaphoreGive(mutexEstado);
      }
      if (estadoTemporario == 0) bot.sendMessage(chat_id, "Status: Trancada", "");
      else if (estadoTemporario == 1) bot.sendMessage(chat_id, "Status: Destrancada (Aguardando visitante)", "");
      else bot.sendMessage(chat_id, "Status: Aberta", "");
    }
  }
}

// --- A NOVA TAREFA DE REDE (NÚCLEO 0) ---
void TarefaRede(void *pvParameters) {
  // mensagem de inicializacao, so roda uma vez no nucleo 0
  Serial.println("Rede: Tarefa iniciada. Mandando mensagem de boot...");
  bot.sendMessageWithReplyKeyboard(CHAT_ID, "Portaria IoT Online!\nOs botões de controle estão no menu abaixo.", "", menuBotoes, true);

  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("Wi-Fi caiu! Tentando reconectar...");
      WiFi.reconnect();
      vTaskDelay(pdMS_TO_TICKS(5000)); // espera 5s antes de tentar de novo
      continue; // pula o resto da tarefa e volta pro começo do loop
    }

    // rede rodando livre
    if (millis() - ultimaChecagemTelegram > intervaloChecagem) {
      int numNovasMensagens = bot.getUpdates(bot.last_message_received + 1);
      
      while (numNovasMensagens) {
        checarMensagens(numNovasMensagens);
        numNovasMensagens = bot.getUpdates(bot.last_message_received + 1);
      }
      ultimaChecagemTelegram = millis();
    }

    int avisoRecebido;
    if (xQueueReceive(filaLogs, &avisoRecebido, 0) == pdTRUE) {
      switch (avisoRecebido) {
        case 1: // Campainha
          Serial.println("Rede: Enviando alerta de campainha tocada...");
          bot.sendMessageWithReplyKeyboard(CHAT_ID, "Campainha acionada!...", "", menuBotoes, true);
          registrarNoFirebase("Campainha Tocada");
          break;
        case 2: // Abertura Física
          Serial.println("Rede: Registrando abertura física...");
          registrarNoFirebase("Porta Aberta (Sensor)");
          break;
        case 3: // Timeout
          Serial.println("Rede: Enviando alertas de Timeout...");
          bot.sendMessage(CHAT_ID, "O visitante não abriu a porta a tempo. Trancada novamente.", "");
          registrarNoFirebase("Acesso Negado (Timeout)");
          break;
        case 4: // Porta Fechada
          Serial.println("Rede: Enviando alerta de porta fechada...");
          bot.sendMessage(CHAT_ID, "O visitante entrou. Porta fechada e trancada com sucesso.", "");
          registrarNoFirebase("Porta Fechada e Trancada");
          break;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(100)); 
  }
}

// boom boom chk chk boom e tals
void maquinaDeEstados(int estadoIma) {
  switch (estadoSistema) {
    
    case 0: // TRANCADA
      if (campainhaAcionada) {
        if (xSemaphoreTake(mutexEstado, portMAX_DELAY) == pdTRUE) {
          campainhaAcionada = false;
          if (millis() - ultimoToqueCampainha > 5000) { 
            Serial.println("Campainha acionada! Notificando proprietária...");
            int aviso = 1;
            xQueueSend(filaLogs, &aviso, 0);
            ultimoToqueCampainha = millis();
          }
          xSemaphoreGive(mutexEstado);
        }
      }
      break;

    case 1: // DESTRANCADA
      if (estadoIma == HIGH) { 
        if (xSemaphoreTake(mutexEstado, portMAX_DELAY) == pdTRUE) {
          Serial.println("Sensor: Porta foi aberta pelo visitante.");
          estadoSistema = 2;
          int aviso = 2;
          xQueueSend(filaLogs, &aviso, 0);
          xSemaphoreGive(mutexEstado);
          vTaskDelay(pdMS_TO_TICKS(500));
        }
      } else if (millis() - cronometroEstado1 > tempoLimite) {
        if (xSemaphoreTake(mutexEstado, portMAX_DELAY) == pdTRUE) { 
          Serial.println("TIMEOUT! Trancando por segurança...");
          motorTranca.write(0);
          estadoSistema = 0;
          int aviso = 3;
          xQueueSend(filaLogs, &aviso, 0);
          xSemaphoreGive(mutexEstado);
        }
      }
      break;

    case 2: // ABERTA
      if (estadoIma == LOW) {
        if (xSemaphoreTake(mutexEstado, portMAX_DELAY) == pdTRUE) { 
          Serial.println("Sensor: Porta encostada. Trancando...");
          motorTranca.write(0); 
          estadoSistema = 0;
          int aviso = 4;
          xQueueSend(filaLogs, &aviso, 0);
          xSemaphoreGive(mutexEstado);
          vTaskDelay(pdMS_TO_TICKS(500));
        }
      }
      break;
  }
}

void TarefaHardware(void *pvParameters) {
  for (;;) {
    int comandoRecebido;

    if (xQueueReceive(filaComandos, &comandoRecebido, 0) == pdTRUE ){
      // se recebeu 1 (abrir), e a porta ta trancada, executa
      if (comandoRecebido == 1 && estadoSistema == 0){
        if (xSemaphoreTake(mutexEstado, portMAX_DELAY) == pdTRUE) {
          Serial.println("Hardware: Destrancando...");
          motorTranca.write(90);
          estadoSistema = 1;
          cronometroEstado1 = millis();
          xSemaphoreGive(mutexEstado);
        }
      }
    }  

    int estadoIma = digitalRead(pinoSensorPorta);
    maquinaDeEstados (estadoIma);
    vTaskDelay(pdMS_TO_TICKS(50)); 
  }
}
