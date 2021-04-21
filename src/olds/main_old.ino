/* BIBLIOTECAS */
#include "SPIFFS.h"
#include <sqlite3.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <esp_task_wdt.h>
#include <HTTPUpdate.h>
#include <driver/uart.h>
#include <driver/uart_select.h>
#include <freertos/queue.h>
#include <NTPClient.h> //Biblioteca NTPClient modificada
#include <WiFiUdp.h>   //Socket UDP
#include <time.h>
// #include <sys/time.h>
#include <WiFiMulti.h>
#include "freertos/timers.h"

/* DECLARAÇÃO DAS CONSTANTES */
#define version         "1.0.0"                 //VERSÃO CÓDIGO
#define wtd             32                      //Define o pin do WatchDog externo
#define led             2                       //Define o pin do led interno ESP32
#define PIN_LED_GREEN   26                      //Define o pin do led Verde
#define PIN_LED_RED     27                      //Define o pin do led Vermelho
#define PIN_LED_BLUE    25                      //Define o pin do led Azeul
#define RX_Serial       18                      //Define o pin do RX Serial1(Leitor) ESP32
#define TX_Serial       19                      //Define o pin do TX Serial1(Leitor) ESP32
#define RELE_ONE        22                      //Define o pin do Rele Entrada
#define RELE_TWO        23                      //Define o pin do Rele Saída
#define pathDb          "/spiffs/esp32_thinclient.db"    //Path aquivo Bando de dados
#define mqttServer      "52.251.127.132"        //Path Server MQTT
#define mqttPort        1883                    //Port acesso MQTT
#define mqttUser        "scan"                  //User MQTT
#define mqttPassword    "q1p0w2o9"              //Password MQTT
#define BUF_SIZE (70)                           //Buffer evento leitura



/* DECLARAÇÃO DAS VARIÁVEIS GLOBAIS */
char *binURL = "http://200.161.186.33:8052/Shuttle/sketch/"; //Endereço Server onde busca atualização do firmware
char *zErrMsg = 0;                                           //PONTEIRO UTILIZADO PELO METODO dbExec
const char *data = "Callback function called";               //PONTEIRO UTILIZADO PELO METODO dbCallback
int timeWatchDog = 0;                                        //VARIÁVEL TEMPO WATCHDOG, EM SEGUNDOS
String deviceId;


//Struct com os dados do dia e hora
struct tm dateTime;

/* INICIANDO OBJETOS */
sqlite3 *db1;
WiFiMulti wifiMulti;
WiFiClient mqttClient;
PubSubClient client(mqttClient);
TaskHandle_t TaskLoop2, TaskUartEvent, TaskRead, TaskSetupNTP;
TimerHandle_t wifiReconnectTimer, restartTimer, syncToAccessControl;
static QueueHandle_t uart1_queue;
HTTPClient http;

//Socket UDP que a lib utiliza para recuperar dados sobre o horário
WiFiUDP udp;

//Objeto responsável por recuperar dados sobre horário
NTPClient ntpClient(
  udp,                  //socket udp
  "0.br.pool.ntp.org",  //URL do servwer NTP
  -3 * 3600,            //Deslocamento do horário em relacão ao GMT 0
  60000);               //Intervalo entre verificações online

/* DECLARAÇÃO DAS FUNÇÕES */


/* CONFIGURAÇÃO UART */
uart_config_t uart_config = {
  .baud_rate = 115200,
  .data_bits = UART_DATA_8_BITS,
  .parity = UART_PARITY_DISABLE,
  .stop_bits = UART_STOP_BITS_1,
  .flow_ctrl = UART_HW_FLOWCTRL_DISABLE
};

/* INÍCIO DO PROGRAMA */
void setup() {
  InitSerial();
  delay(300);
  client.setServer(mqttServer, mqttPort);
  client.setCallback(callbackMQTT);
  delay(300);
  xTaskCreatePinnedToCore(uart_event_task, "uart_event_task", 10000, NULL, 2, &TaskUartEvent, 0); //Inicia Task Evento Uart
  
  esp_task_wdt_add(TaskLoop2);                                                     
  esp_task_wdt_add(TaskSetupNTP);
 
  esp_task_wdt_init(timeWatchDog, true);
}

void loop() {
  // vTaskDelay(pdMS_TO_TICKS(100));
  // vTaskDelete(NULL);
}

/* CONFIGURAÇÕES */
void InitSerial() {
  Serial.begin(115200);
  delay(300);
  if (Serial) {
    long baud = (Serial.baudRate());
    Serial.println("Porta Serial aberta com sucesso, Baud Rate: " + String(baud));
  }

  uart_param_config(UART_NUM_1, &uart_config);
  uart_set_line_inverse(UART_NUM_1, UART_INVERSE_RXD);
  uart_set_pin(UART_NUM_1, TX_Serial, RX_Serial, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
  uart_driver_install(UART_NUM_1, BUF_SIZE * 2, BUF_SIZE * 2, 10, &uart1_queue, 0);
}


/* MQTT */
void connectMQTT() {
  esp_task_wdt_reset(); //Reseta Timer WatchDog
  int i = 0;
  Serial.println("Tentando conectar ao servidor MQTT...");
  String msg = "Device ID: " + String(deviceId);
  msg += " conectado em SSID: " + WiFi.SSID() + "(" + WiFi.RSSI() + "dBm) - IP: " + WiFi.localIP().toString();
  String desconection = "Device ID: " + String(deviceId) + " desconectado do Broker";
  while (!client.connected() && i < 2) {
    client.connect(deviceId.c_str(), mqttUser, mqttPassword, NULL, 1, false, desconection.c_str(), false);
    // printSerial(false, String(i) + ", ");
    i = i + 1;
    esp_task_wdt_reset(); //Reseta Timer WatchDog
  }
  switch (client.connected()) {
    case true:
      getSubscribes();
      Serial.println("");
      Serial.println(msg);
      break;
    default:
      Serial.println("");
      Serial.println("Falhou ao tentar conectar com Broker.");
      break;
  }
  esp_task_wdt_reset(); //Reseta Timer WatchDog
}

void getSubscribes() {
  Serial.println("===========================================");
  Serial.println("Topicos Subscribe");
  Serial.println("===========================================");
}

void callbackMQTT(char *topico, byte *mensagem, unsigned int tamanho) {
  esp_task_wdt_reset(); //Reseta Timer WatchDog
  mensagem[tamanho] = '\0';
  String strMensagem = String((char *)mensagem);
  strMensagem.replace("\0", "");
  strMensagem.replace("\n", "");
  strMensagem.replace("\r", "");
  strMensagem.trim();
  Serial.println("----------------------------");
  Serial.println("Mensagem recebida! Topico: ");
  Serial.println(topico);
  Serial.println(". Tamanho: ");
  Serial.println(String(tamanho).c_str());
  Serial.println(". Mensagem: ");
  Serial.println(strMensagem);
}

bool publishMQTT(String topic, String payload) {
  bool processed = false;
  Serial.println("Publish Sketch");
  bool resp = client.publish(topic.c_str(), payload.c_str(), 1, false);
  if (resp)
  {
    Serial.println("MQTT enviado para " + String(topic));
    processed = true;
  }
  else
  {
    Serial.println("Falha no envio MQTT para " + String(topic));
    processed = false;
  }
  return processed;
}

/* MANIPULADOR EVENTO SERIAL*/
static void uart_event_task(void *pvParameters) {
  uart_event_t event;
  String payload = emptyString;
  int len = NULL;
  while (true) {
    if (xQueueReceive(uart1_queue, (void *)&event, (portTickType)portMAX_DELAY)) {
      switch (event.type) {
        case UART_DATA:
          // if (VerificaSerial()) {
          //   xTaskCreatePinnedToCore(QrCodeReader, "read", 10000, NULL, 2, NULL, 0);
          // } TODO: Nesse ponto que envia a leitura realizada
          break;
        case UART_FIFO_OVF:
          payload = "hw overflow";
          Serial.println(payload);
          break;
        case UART_BUFFER_FULL:
          //firstRead = true;
          payload = "data > buffer";
          Serial.println(payload);
          break;
        case UART_BREAK:
          payload = "UART_BREAK";
          Serial.println(payload);
          break;
        case UART_PARITY_ERR:
          payload = "UART_PARITY_ERR";
          Serial.println(payload);
          break;
        case UART_FRAME_ERR:
          payload = "UART_FRAME_ERR";
          Serial.println(payload);
          break;
        case UART_PATTERN_DET:
          payload = "UART_PATTERN_DET";
          Serial.println(payload);
          break;
        default:
          payload = "unknown error";
          Serial.println(payload);
          break;
      }
    }
  }
  vTaskDelete(NULL);
}

// void QrCodeReader(void *args) {
//   printSerial(true, "Task Leitura Serial");
//   uint8_t *dataReader = (uint8_t *)malloc(BUF_SIZE);
//   String qrCode = emptyString;
//   qrCode.reserve(10);
//   int len = uart_read_bytes(UART_NUM_1, dataReader, BUF_SIZE, pdMS_TO_TICKS(100));
//   if (len > 0)
//   {
//     qrCode = String((char *)dataReader).substring(0, 8);
//     qrCode.trim();
//     printSerial(true, "Tamanho do QrCode: " + String(qrCode.length()));
//     uart_flush(UART_NUM_1);
//     uart_flush_input(UART_NUM_1);
//     while (true)
//     {
//       vTaskDelay(10);
//       if (!busyDb)
//       {
//         vTaskSuspend(TaskLoop2);
//         vTaskDelay(pdMS_TO_TICKS(100));
//         IsValid(qrCode);
//         vTaskResume(TaskLoop2);
//         vTaskDelay(pdMS_TO_TICKS(100));
//         busyQRCode = false;
//         free(dataReader);
//         dataReader = NULL;
//         vTaskDelete(NULL);
//         return;
//       }
//     }
//   }
//   uart_flush(UART_NUM_1);
//   uart_flush_input(UART_NUM_1);
//   free(dataReader);
//   dataReader = NULL;
//   busyQRCode = false;
//   vTaskDelete(NULL);
//   return;
// }
