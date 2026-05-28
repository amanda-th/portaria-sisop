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
enum EstadoPorta {
  TRANCADA = 0,
  DESTRANCADA = 1,
  ABERTA = 2
};

EstadoPorta estadoSistema = TRANCADA; // inicia a var ja trancada

enum TipoAviso {
  NENHUM = 0,
  AVISO_CAMPAINHA = 1,
  AVISO_PORTA_ABERTA = 2,
  AVISO_TIMEOUT = 3,
  AVISO_PORTA_FECHADA = 4
};

unsigned long cronometroEstado1 = 0; 
const long tempoLimite = 15000;
unsigned long ultimaChecagemTelegram = 0;
const long intervaloChecagem = 1000;
const char* menuBotoes = "[[\"/abrir\", \"/status\"]]";

// interrupção
volatile bool campainhaAcionada = false;
unsigned long ultimoToqueCampainha = 0;
volatile unsigned long ultimoDebounce = 0;

void IRAM_ATTR detectarCampainha();
void registrarNoFirebase(String evento);
void checarMensagens(int numNovasMensagens);
void TarefaRede(void *pvParameters);
void moverMotorSuavemente(int anguloDestino);
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

    int estadoAtual = -1;
    
    if (xSemaphoreTake(mutexEstado, portMAX_DELAY) == pdTRUE) {
      estadoAtual = estadoSistema;
      xSemaphoreGive(mutexEstado);
    }

    if (texto == "/abrir") {
      if (estadoAtual == TRANCADA) { 
        Serial.println("Comando remoto recebido. Destrancando...");
        bot.sendMessage(chat_id, "Acesso autorizado! Destrancando a porta...", "");
          
        int comando = 1; // código interno para a fila de comando
        xQueueSend(filaComandos, &comando, 0);

        vTaskDelay(pdMS_TO_TICKS(800)); // delay pra evitar brownout

        registrarNoFirebase("Acesso autorizado (Telegram)");
      } else {
        bot.sendMessage(chat_id, "A porta já está destrancada ou aberta.", "");
      }
    }

    // --- COMANDO /STATUS ---
    else if (texto == "/status") {
      if (estadoAtual == TRANCADA) {
        bot.sendMessage(chat_id, "Status: Trancada 🔒", "");
      } 
      else if (estadoAtual == DESTRANCADA) {
        bot.sendMessage(chat_id, "Status: Destrancada 🔓 (Aguardando visitante abrir a porta)", "");
      } 
      else if (estadoAtual == ABERTA) {
        bot.sendMessage(chat_id, "Status: Aberta 🚪", "");
      }
    }
  }
}

// --- A TAREFA DE REDE (NÚCLEO 0) ---
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
        case AVISO_CAMPAINHA:
          Serial.println("Rede: Enviando alerta de campainha tocada...");
          bot.sendMessageWithReplyKeyboard(CHAT_ID, "Campainha acionada!...", "", menuBotoes, true);
          registrarNoFirebase("Campainha Tocada");
          break;
        case AVISO_PORTA_ABERTA:
          Serial.println("Rede: Registrando abertura física...");
          registrarNoFirebase("Porta Aberta (Sensor)");
          break;
        case AVISO_TIMEOUT:
          Serial.println("Rede: Enviando alertas de Timeout...");
          bot.sendMessage(CHAT_ID, "O visitante não abriu a porta a tempo. Trancada novamente.", "");
          registrarNoFirebase("Acesso Negado (Timeout)");
          break;
        case AVISO_PORTA_FECHADA:
          Serial.println("Rede: Enviando alerta de porta fechada...");
          bot.sendMessage(CHAT_ID, "O visitante entrou. Porta fechada e trancada com sucesso.", "");
          registrarNoFirebase("Porta Fechada e Trancada");
          break;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(100)); 
  }
}

// func pra tentar evitar o erro de brownout detector
void moverMotorSuavemente(int anguloDestino) {
  // bora acordar
  motorTranca.attach(pinoMotor, 500, 2400);

  int anguloAtual = motorTranca.read(); 
  int tamanhoDoPasso = 5; // aumentar aki pra deixar + rapido

  if (anguloAtual < anguloDestino) {
    // indo para frente (ex: 0 para 90), pulando de 5 em 5 graus
    for (int pos = anguloAtual; pos <= anguloDestino; pos += tamanhoDoPasso) {
      motorTranca.write(pos);
      vTaskDelay(pdMS_TO_TICKS(15)); // manter o delay em 15 ou 10
    }
    motorTranca.write(anguloDestino); 

  } else {
    // voltando (ex: 90 para 0), descendo de 5 em 5 graus
    for (int pos = anguloAtual; pos >= anguloDestino; pos -= tamanhoDoPasso) {
      motorTranca.write(pos);
      vTaskDelay(pdMS_TO_TICKS(15));
    }
    motorTranca.write(anguloDestino);
  }
}

void maquinaDeEstados(int estadoIma) {
  TipoAviso aviso = NENHUM;       // var unica p salvar o babado
  bool pausar = false; // flag pro delay

  switch (estadoSistema) {
    
    case TRANCADA:
      if (campainhaAcionada) {
        if (xSemaphoreTake(mutexEstado, portMAX_DELAY) == pdTRUE) {
          campainhaAcionada = false;
          if (millis() - ultimoToqueCampainha > 5000) { 
            Serial.println("Campainha acionada! Notificando proprietária...");
            aviso = AVISO_CAMPAINHA; // só anota n envia nd
            ultimoToqueCampainha = millis();
          }
          xSemaphoreGive(mutexEstado);
        }
      }
      break;

    case DESTRANCADA:
      if (estadoIma == HIGH) { 
        if (xSemaphoreTake(mutexEstado, portMAX_DELAY) == pdTRUE) {
          Serial.println("Sensor: Porta foi aberta pelo visitante.");
          estadoSistema = ABERTA;
          aviso = AVISO_PORTA_ABERTA;
          xSemaphoreGive(mutexEstado);
          pausar = true;
        }
      } else if (millis() - cronometroEstado1 > tempoLimite) {
        if (xSemaphoreTake(mutexEstado, portMAX_DELAY) == pdTRUE) { 
          Serial.println("TIMEOUT! Trancando por segurança...");
          moverMotorSuavemente(0);
          estadoSistema = TRANCADA;
          aviso = AVISO_TIMEOUT;
          xSemaphoreGive(mutexEstado);
        }
      }
      break;

    case ABERTA:
      if (estadoIma == LOW) {
        if (xSemaphoreTake(mutexEstado, portMAX_DELAY) == pdTRUE) { 
          Serial.println("Sensor: Porta encostada. Trancando...");
          moverMotorSuavemente(0); 
          estadoSistema = TRANCADA;
          aviso = AVISO_PORTA_FECHADA;
          xSemaphoreGive(mutexEstado);
          pausar = true;
        }
      }
      break;
  }

  // se algum dos cases gerou um aviso envia pra fila
  if (aviso != NENHUM) {
    int valorParaFila = aviso;
    xQueueSend(filaLogs, &valorParaFila, 0);
  }

  // se algum case pediu pausa aplica o delay aqui
  if (pausar) {
    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

// bixa louca mds apaguei sem querer a funcao
void TarefaHardware(void *pvParameters) {
  for (;;) {
    int comandoRecebido;
    if (xQueueReceive(filaComandos, &comandoRecebido, 0) == pdTRUE) { // ve se tem algo na fila
      if (comandoRecebido == 1 && xSemaphoreTake(mutexEstado, portMAX_DELAY) == pdTRUE) { // se for o comando de abrir pega
        if (estadoSistema == TRANCADA) {
          Serial.println("Hardware: Destrancando...");
          moverMotorSuavemente(90); // abre
          estadoSistema = DESTRANCADA;
          cronometroEstado1 = millis();
        }
        xSemaphoreGive(mutexEstado); // devolve
      }
    }  
    int estadoIma = digitalRead(pinoSensorPorta);
    maquinaDeEstados(estadoIma);
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}