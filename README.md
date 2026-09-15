# Sistema de Portaria Remota IoT com FreeRTOS, ESP32 e Telegram

![ESP32](https://img.shields.io/badge/Hardware-ESP32-blue?logo=espressif)
![FreeRTOS](https://img.shields.io/badge/RTOS-FreeRTOS-green)
![Telegram API](https://img.shields.io/badge/Telegram-Bot%20API-blue?logo=telegram)
![Firebase](https://img.shields.io/badge/Backend-Firebase-orange?logo=firebase)
![C++](https://img.shields.io/badge/Language-C%2B%2B-00599C?logo=c%2B%2B)

Sistema embarcado de controle de acesso inteligente e fechadura eletrônica autônoma desenvolvido sobre o microcontrolador ESP32. O projeto substitui o loop sequencial tradicional (`void loop()`) por uma arquitetura multitarefa baseada em **FreeRTOS**, explorando o processamento dual-core do chip para desacoplar tarefas de controle mecânico em tempo real de requisições de rede sujeitas a alta latência.

---

## Demonstração em Vídeo

Confira o funcionamento prático do protótipo (notificação de campainha, acionamento via bot e trancamento automático):

  **[Assista à demonstração completa no meu post do LinkedIn](https://lnkd.in/p/dX6DUzfJ)**

---

## Arquitetura do Sistema

A arquitetura foi projetada para garantir que operações lentas de I/O de rede não travem o controle físico e os sensores da porta:

* **Núcleo 0 (Protocol Core):** Gerencia a conectividade Wi-Fi (com rotina de reconexão automática), polling de mensagens via API do Telegram e push de eventos para o Firebase Realtime Database.
* **Núcleo 1 (Application Core):** Opera em tempo estritamente real através de uma Máquina de Estados Finitos. Faz a leitura contínua dos sensores GPIO, trata interrupções de hardware da campainha e atua sobre o servomotor da tranca.

### Comunicação Inter-Tarefas (IPC) & Sincronização
* **`filaComandos` (Queue):** Comunica o sentido *Rede ➔ Hardware*. Transmite instruções recebidas via Telegram (ex: `/abrir`) para o núcleo de controle físico.
* **`filaLogs` (Queue):** Comunica o sentido *Hardware ➔ Rede*. Encapsula eventos físicos (campainha tocada, porta aberta/fechada, timeouts) para envio ao Telegram e ao Firebase.
* **`mutexEstado` (Mutex):** Semáforo de exclusão mútua protegendo a variável global `estadoSistema`, prevenindo condições de corrida (*race conditions*) durante leituras/escritas concorrentes.

---

## Funcionalidades

- **Acionamento Bidirecional:** Notificação remota quando a campainha é pressionada e abertura remota via bot no Telegram (`/abrir` e `/status`).
- **Trancamento Autônomo:** Detecção de fechamento da porta via sensor magnético Reed Switch com acionamento automático da tranca física.
- **Mecanismo de Contingência (Timeout):** Caso a porta seja destrancada remotamente mas não seja aberta fisicamente em até 15 segundos, o sistema cancela a abertura, tranca a porta novamente e dispara um alerta de segurança.
- **Painel Web de Auditoria:** Interface web (HTML/JS) que consome os registros do Firebase em tempo real, gerando histórico detalhado com timestamp e status dos eventos.

---

## Desafio: Mitigação de Brownout

Durante os testes de bancada, a inicialização abrupta do servomotor somada à demanda do rádio Wi-Fi causava picos de corrente que derrubavam a tensão de alimentação abaixo do limite operacional (~2,44V), disparando o *Brownout Detector* do ESP32 e reiniciando o circuito.

A estabilidade elétrica foi alcançada por meio de duas soluções integradas:
1. **Hardware:** Adição de um capacitor eletrolítico de 10 µF em paralelo aos barramentos de alimentação para amortecer transientes rápidos de corrente.
2. **Software:** Desenvolvimento da rotina `moverMotorSuavemente()`, que fraciona o curso angular do servo em pequenos incrementos (*degraus*) intercalados por `vTaskDelay()`, achatando o pico de corrente instantâneo sem bloquear a ULA nem estourar o *Watchdog Timer* (WDT).

---

## Hardware Utilizado

- Módulo ESP32 / ESP32-CAM
- Servomotor SG90 (atuador de tranca)
- Sensor Magnético Reed Switch (monitoramento de abertura)
- Push-button com resistor pull-up (campainha com ISR)
- Módulo Conversor USB-Serial FTDI (5V)
- Capacitor Eletrolítico 10 µF

---

## Stack Tecnológica

- **Linguagem:** C / C++ (Framework Arduino / ESP-IDF)
- **Sistema Operacional:** FreeRTOS (Tasks fixadas por núcleo, Queues, Mutexes, ISR)
- **Nuvem & Mensageria:** Telegram Bot API, Firebase Realtime Database
- **Frontend de Auditoria:** HTML5, CSS3, JavaScript

---

*Projeto acadêmico desenvolvido no âmbito da disciplina de Sistemas Operacionais — Engenharia de Computação (UERGS).*
