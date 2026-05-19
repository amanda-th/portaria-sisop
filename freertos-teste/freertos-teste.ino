#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <ArduinoJson.h>
#include <ESP32Servo.h>
#include <FirebaseESP32.h>
#include "secrets.h"

// --- VAR DE ESTADO E TEMPO ---
int estadoSistema = 0;
unsigned long cronometroEstado1 = 0; 
const long tempoLimite = 15000;

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
unsigned long ultimaChecagemTelegram = 0;
const long intervaloChecagem = 1000;

// interrupção
volatile bool campainhaAcionada = false;
unsigned long ultimoToqueCampainha = 0;

// O Nosso Cadeado do FreeRTOS
SemaphoreHandle_t mutexEstado;

QueueHandle_t filaComandos;

// menu dos botoes do telegram
String menuBotoes = "[[\"/abrir\", \"/status\"]]";

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
      if (xSemaphoreTake(mutexEstado, portMAX_DELAY) == pdTRUE) {
        
        if (estadoSistema == 0) {
          Serial.println("Comando remoto recebido. Destrancando...");
          bot.sendMessage(chat_id, "Acesso autorizado! Destrancando a porta...", "");
          
          motorTranca.write(90); 
          estadoSistema = 1; 
          cronometroEstado1 = millis();
          
          registrarNoFirebase("Acesso Autorizado (Telegram)");
        } else {
          bot.sendMessage(chat_id, "A porta já está destrancada ou aberta.", "");
        }
        xSemaphoreGive(mutexEstado);
      }
    }

    else if (texto == "/status") {
      // sera de aqui ser um semaforo tambem?
      if (xSemaphoreTake(mutexEstado, portMAX_DELAY) == pdTRUE){
        if (estadoSistema == 0) bot.sendMessage(chat_id, "Status: Trancada", "");
        else if (estadoSistema == 1) bot.sendMessage(chat_id, "Status: Destrancada (Aguardando visitante abrir a maçaneta)", "");
        else bot.sendMessage(chat_id, "Status: Aberta", "");

        xSemaphoreGive(mutexEstado);
      }
    }
  }
}

// --- A NOVA TAREFA DE REDE (NÚCLEO 0) ---
void TarefaRede(void *pvParameters) {
  for (;;) {
    // rede rodando livre
    if (millis() - ultimaChecagemTelegram > intervaloChecagem) {
      int numNovasMensagens = bot.getUpdates(bot.last_message_received + 1);
      
      while (numNovasMensagens) {
        checarMensagens(numNovasMensagens);
        numNovasMensagens = bot.getUpdates(bot.last_message_received + 1);
      }
      ultimaChecagemTelegram = millis();
    }
    
    vTaskDelay(pdMS_TO_TICKS(100)); 
  }
}

void maquinaDeEstados (int estadoIma){
  if (estadoSistema == 0 && campainhaAcionada) {
    if (xSemaphoreTake(mutexEstado, portMAX_DELAY) == pdTRUE){
      campainhaAcionada = false;
      
      if (millis() - ultimoToqueCampainha > 5000) { 
        Serial.println("Campainha acionada! Notificando proprietária...");
        bot.sendMessageWithReplyKeyboard(CHAT_ID, "🔔 Campainha acionada!...", "", menuBotoes, true);
        registrarNoFirebase("Campainha Tocada");
        ultimoToqueCampainha = millis();
      }
      
      xSemaphoreGive(mutexEstado);
    }
  }
  
  // estado 1: porta foi destrancada pelo celular
  if (estadoSistema == 1) {
    if (estadoIma == HIGH) { 
      if (xSemaphoreTake(mutexEstado, portMAX_DELAY) == pdTRUE) {
        Serial.println("Sensor: Porta foi aberta pelo visitante.");
        estadoSistema = 2;
        
        // gatilho firebase
        registrarNoFirebase("Porta Aberta (Sensor)");
        xSemaphoreGive(mutexEstado);

        delay(500);
      }
    } 
    else if (millis() - cronometroEstado1 > tempoLimite) {
      if (xSemaphoreTake(mutexEstado, portMAX_DELAY) == pdTRUE) { 
        Serial.println("TIMEOUT! Trancando por segurança...");
        motorTranca.write(0);
        estadoSistema = 0;
        bot.sendMessage(CHAT_ID, "[!] O visitante não abriu a porta a tempo. Trancada novamente por segurança.", "");
          
        // gatilho firebase
        registrarNoFirebase("Acesso Negado (Timeout)");
        xSemaphoreGive(mutexEstado);
      }
    }
  }
  
  // estado 2: porta tava aberta e agora foi encostada
  if (estadoSistema == 2 && estadoIma == LOW) {
    if (xSemaphoreTake(mutexEstado, portMAX_DELAY) == pdTRUE) { 
      Serial.println("Sensor: Porta encostada. Trancando...");
      motorTranca.write(0); 
      estadoSistema = 0;
      bot.sendMessage(CHAT_ID, "O visitante entrou. Porta fechada e trancada com sucesso.", "");
      
      // gatilho firebase
      registrarNoFirebase("Porta Fechada e Trancada");
      xSemaphoreGive(mutexEstado);

      delay(500);
    }
  }
}

void TarefaHardware(void *pvParameters) {
  for (;;) {
    // 2. le sensor da porta
    int estadoIma = digitalRead(pinoSensorPorta);
    maquinaDeEstados (estadoIma);
    vTaskDelay(pdMS_TO_TICKS(50)); 
  }
}

void setup() {
  Serial.begin(115200);
  mutexEstado = xSemaphoreCreateMutex(); // 0

  filaComandos = xQueueCreate(5, sizeof(int)); // Fila para 5 comandos

  // Fixando as tarefas nos núcleos
  xTaskCreatePinnedToCore(TarefaRede, "Rede_Telegram_Firebase", 10000, NULL, 1, NULL, 0); // Núcleo 0
  xTaskCreatePinnedToCore(TarefaHardware, "Hardware_Porta", 10000, NULL, 1, NULL, 1);     // Núcleo 1
}

void loop() {
  vTaskDelete(NULL); 
}

