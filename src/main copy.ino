/* BIBLIOTECAS */
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <esp_task_wdt.h>
#include <driver/uart.h>
#include <driver/uart_select.h>
#include <freertos/queue.h>
#include <WiFiMulti.h>
#include <Ethernet2.h>
#include <SPIFFS.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <TimeLib.h>
#include <esp_bt.h>
#include <esp_bt_main.h>
#include <esp_bt_device.h>
#include <esp_gap_bt_api.h>
#include <esp_log.h>

/* DECLARAÇÃO DAS CONSTANTES */
#define version         "0.0.1"                 //VERSÃO CÓDIGO
#define led             2                       //Define o pin do led interno ESP32
#define RX_Serial       18                      //Define o pin do RX Serial1(Leitor) ESP32
#define TX_Serial       19                      //Define o pin do TX Serial1(Leitor) ESP32
// #define mqttServer      "52.251.127.132"        //Path Server MQTT
// #define mqttPort        1883                    //Port acesso MQTT
// #define mqttUser        "scan"                  //User MQTT
// #define mqttPassword    "q1p0w2o9"              //Password MQTT
#define BUF_SIZE (70)                           //Buffer evento leitura

// #define ETHERNET_MAC            "BA:E5:E3:B1:44:DD" // Ethernet MAC address (have to be unique between devices in the same network)
// #define ETHERNET_IP             "192.168.10.666"    // IP address of RoomHub when on Ethernet connection
#define ETHERNET_RESET_PIN      11                  // ESP32 pin where reset pin from W5500 is connected
#define ETHERNET_CS_PIN         5                   // ESP32 pin where CS pin from W5500 is connected


/* DECLARAÇÃO DAS VARIÁVEIS GLOBAIS */
int timeWatchDog = 60;                                        //VARIÁVEL TEMPO WATCHDOG, EM SEGUNDOS
String deviceId;

/* CONSTANTES */
// Porta Servidor Web
const byte WEBSERVER_PORT           = 80;
// Headers do Servidor Web
const char* WEBSERVER_HEADER_KEYS[] = {"User-Agent"};
// Porta Servidor DNS
const byte DNSSERVER_PORT           = 53;

// Tamanho do Objeto JSON
const size_t JSON_SIZE              = JSON_OBJECT_SIZE(256);

/* INICIANDO OBJETOS */
WiFiMulti wifiMulti;
WiFiClient mqttClient;
PubSubClient client(mqttClient);
TaskHandle_t TaskUartEvent, TaskWebServer;
static QueueHandle_t uart1_queue;
EthernetClient ethClient;
IPAddress ipAddress;
DNSServer dnsServer;
WebServer server(WEBSERVER_PORT);

//Socket UDP que a lib utiliza para recuperar dados sobre o horário
WiFiUDP udp;

/* STRUCTS */
struct Config {
  char pubQrCodeReceiver[100];
  char mqttServer[50];
  char mqttPort[10];
  char mqttUser[30];
  char mqttPassword[30];
  char deviceIp[20];
  char macAdress[20];
  bool isExit;
  bool external;
} config;


typedef enum {
  APP_GAP_STATE_IDLE = 0,
  APP_GAP_STATE_DEVICE_DISCOVERING,
  APP_GAP_STATE_DEVICE_DISCOVER_COMPLETE,
  APP_GAP_STATE_SERVICE_DISCOVERING,
  APP_GAP_STATE_SERVICE_DISCOVER_COMPLETE,
} app_gap_state_t;

typedef struct {
  bool dev_found;
  uint8_t bdname_len;
  uint8_t eir_len;
  uint8_t rssi;
  uint32_t cod;
  uint8_t eir[ESP_BT_GAP_EIR_DATA_LEN];
  uint8_t bdname[ESP_BT_GAP_MAX_BDNAME_LEN + 1];
  esp_bd_addr_t bda;
  app_gap_state_t state;
} app_gap_cb_t;

static app_gap_cb_t m_dev_info;

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
  initSPIFFS();
  delay(300);
  // Lê configuração
  loadConfiguration();
  // Salva configuração
  configSave();
  initWifi();
  delay(300);
  connectEthernet();

  client.setServer(config.mqttServer, (int)config.mqttPort);
  client.setCallback(callbackMQTT);
  //Inicia Task Evento Uart
  xTaskCreatePinnedToCore(uart_event_task, "uart_event_task", 10000, NULL, 2, &TaskUartEvent, 0); 
  // xTaskCreatePinnedToCore(webServer, "webServer", 10000, NULL, 2, &TaskWebServer, 0);

  // WebServer
  server.on("/config"    , handleConfig);
  server.on("/configSave", handleConfigSave);
  server.on("/reconfig"  , handleReconfig);
  server.on("/reboot"    , handleReboot);
  server.on("/css"       , handleCSS);
  server.onNotFound(handleHome);
  server.collectHeaders(WEBSERVER_HEADER_KEYS, 1);
  server.begin();
  
  initBluethoot();

  // esp_task_wdt_init(timeWatchDog, true);
}

void loop() {
  // WatchDog ----------------------------------------
  yield();
  // DNS ---------------------------------------------
  dnsServer.processNextRequest();
  // Web ---------------------------------------------
  server.handleClient();
  
  // scanBluethoot();
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

void initSPIFFS() {
  Serial.println("------------------------");
  Serial.println("INICIANDO SPIFFS");
  if (!SPIFFS.begin(true))
  {
    Serial.println("SPIFFS Mount Failed");
    return;
  }
  else
  {
    Serial.println("------------------------");
    Serial.println("SPIFFS OK");
    Serial.println("Total bytes SPIFF: " + String(SPIFFS.totalBytes()));
    Serial.println("------------------------");
  }
}

void loadConfiguration() {
  File file = SPIFFS.open("/config.json", "r");

  StaticJsonDocument<JSON_SIZE> jsonConfig;

  DeserializationError error = deserializeJson(jsonConfig, file);
  if (error){
    Serial.println("Failed to read file, using default configuration");
    configReset();
  }
  else {
    strlcpy(config.mqttServer, jsonConfig["mqttServer"] | "", sizeof(config.mqttServer));
    strlcpy(config.mqttPort, jsonConfig["mqttPort"] | "", sizeof(config.mqttPort));
    strlcpy(config.mqttUser, jsonConfig["mqttUser"] | "", sizeof(config.mqttUser));
    strlcpy(config.mqttPassword, jsonConfig["mqttPassword"] | "", sizeof(config.mqttPassword));
    strlcpy(config.pubQrCodeReceiver, jsonConfig["pubQrCodeReceiver"] | "", sizeof(config.pubQrCodeReceiver));
    strlcpy(config.deviceIp, jsonConfig["deviceIp"] | "", sizeof(config.deviceIp));
    strlcpy(config.macAdress, jsonConfig["macAdress"] | "", sizeof(config.macAdress));
    config.isExit = jsonConfig["isExit"] | false;
    config.external = jsonConfig["external"] | false;

    serializeJsonPretty(jsonConfig, Serial);
    file.close();
  }
}

bool configSave() {
  Serial.println("");
  Serial.println("Salvando Configurações");
  // Grava configuração
  StaticJsonDocument<JSON_SIZE> jsonConfig;

  File file = SPIFFS.open("/config.json", "w+");
  if (file) {
    // Atribui valores ao JSON e grava
    jsonConfig["mqttServer"]        = config.mqttServer;
    jsonConfig["mqttPort"]          = config.mqttPort;
    jsonConfig["mqttUser"]          = config.mqttUser;
    jsonConfig["mqttPassword"]      = config.mqttPassword;
    jsonConfig["pubQrCodeReceiver"] = config.pubQrCodeReceiver;
    jsonConfig["macAdress"]         = config.macAdress;    
    jsonConfig["deviceIp"]          = config.deviceIp;
    jsonConfig["isExit"]            = config.isExit;
    jsonConfig["External"]          = config.external;

    serializeJsonPretty(jsonConfig, file);
    file.close();
    return true;
  }
  else 
  {
    Serial.println("Falha ao Salvar Configurações");
    return false;
  }
}

String getMacAdrees(){
  char mac[15];
  uint64_t chipid = ESP.getEfuseMac();
  sprintf(mac, "%04X%08X", (uint16_t)(chipid >> 32), (uint32_t)chipid);
  Serial.println(mac);
  int index = 0;
  String response;
  for (int i = 0; i <= 5; i++){
    response += String(mac).substring(index, index+2);
    if (i < 5){
      response += ":";
    }
    index = index + 2;
  }
  strlcpy(config.macAdress, (char*)response.c_str(), sizeof(config.mqttServer));
  return response;
}

void configReset() {
  strlcpy(config.mqttServer, "52.251.127.132", sizeof(config.mqttServer));
  strlcpy(config.mqttPort, "1883", sizeof(config.mqttServer));
  strlcpy(config.mqttUser, "scan", sizeof(config.mqttServer));
  strlcpy(config.mqttPassword, "q1p0w2o9", sizeof(config.mqttServer));
  strlcpy(config.pubQrCodeReceiver, "mercadolivre/cxland/qrcodereceiver/192.168.0.1", sizeof(config.mqttServer));
  strlcpy(config.deviceIp, "192.168.0.99", sizeof(config.mqttServer));
  config.isExit            = false;
  config.external          = false;
}

String longTimeStr(const time_t &t){
  // Retorna segundos como "d:hh:mm:ss"
  String s = String(t / SECS_PER_DAY) + ':';
  if (hour(t) < 10) {
    s += '0';
  }
  s += String(hour(t)) + ':';
  if (minute(t) < 10) {
    s += '0';
  }
  s += String(minute(t)) + ':';
  if (second(t) < 10) {
    s += '0';
  }
  s += String(second(t));
  return s;
}

String ipStr(const IPAddress &ip) {
  // Retorna IPAddress em formato "n.n.n.n"
  String sFn = "";
  for (byte bFn = 0; bFn < 3; bFn++) {
    sFn += String((ip >> (8 * bFn)) & 0xFF) + ".";
  }
  sFn += String(((ip >> 8 * 3)) & 0xFF);
  return sFn;
}

/* ETHERNET */
void connectEthernet() {
  delay(500);
  byte* macAdress = new byte[6];
  
  macCharArrayToBytes(getMacAdrees().c_str(), macAdress);
  ipAddress.fromString(config.deviceIp);

  Ethernet.init(ETHERNET_CS_PIN);
  ethernetWizReset(ETHERNET_RESET_PIN);

  Serial.println("Starting ETHERNET connection...");
  Ethernet.begin(macAdress);
  delay(200);

  Serial.print("Ethernet IP is: ");
  Serial.println(Ethernet.localIP());
}

void ethernetWizReset(const uint8_t resetPin) {
    pinMode(resetPin, OUTPUT);
    digitalWrite(resetPin, HIGH);
    delay(250);
    digitalWrite(resetPin, LOW);
    delay(50);
    digitalWrite(resetPin, HIGH);
    delay(350);
}

void macCharArrayToBytes(const char* str, byte* bytes) {
  for (int i = 0; i < 6; i++) {
    bytes[i] = strtoul(str, NULL, 16);
    str = strchr(str, ':');
    if (str == NULL || *str == '\0') {
      break;
    }
    str++;
  }
}

/* WIFI */
void initWifi(){
  WiFi.setHostname("ESP32");
  WiFi.softAP("ESP32");
  Serial.println("WiFi AP ESP32 - IP " + ipStr(WiFi.softAPIP()));
  dnsServer.setErrorReplyCode(DNSReplyCode::ServerFailure);
  dnsServer.start(DNSSERVER_PORT, "*", WiFi.softAPIP());
}

/* MQTT */
void connectMQTT() {
  esp_task_wdt_reset(); //Reseta Timer WatchDog
  int i = 0;
  Serial.println("Tentando conectar ao servidor MQTT...");
  deviceId = getMacAdrees();
  String msg = "Device ID: " + String(deviceId);
  msg += "IP: " + WiFi.localIP().toString();
  
  String desconection = "Device ID: " + String(deviceId) + " desconectado do Broker";
  
  while (!client.connected() && i < 2) {
    client.connect(deviceId.c_str(), config.mqttUser, config.mqttPassword, NULL, 1, false, desconection.c_str(), false);
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
  Serial.println("====================");
  Serial.println("Topicos Subscribe");
  Serial.println("====================");
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
  bool resp = client.publish(topic.c_str(), payload.c_str(), 0, false);
  if (resp)
  {
    Serial.println("MQTT enviado para " + String(topic));
  }
  else
  {
    Serial.println("Falha no envio MQTT para " + String(topic));
  }
  return resp;
}

/* SCAN BLUETHOOT */
void initBluethoot()
{
    esp_err_t ret;

    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    if ((ret = esp_bt_controller_init(&bt_cfg)) != ESP_OK) {
        ESP_LOGE(GAP_TAG, "%s initialize controller failed: %s\n", __func__, esp_err_to_name(ret));
        return;
    }

    if ((ret = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT)) != ESP_OK) {
        ESP_LOGE(GAP_TAG, "%s enable controller failed: %s\n", __func__, esp_err_to_name(ret));
        return;
    }

    if ((ret = esp_bluedroid_init()) != ESP_OK) {
        ESP_LOGE(GAP_TAG, "%s initialize bluedroid failed: %s\n", __func__, esp_err_to_name(ret));
        return;
    }

    if ((ret = esp_bluedroid_enable()) != ESP_OK) {
        ESP_LOGE(GAP_TAG, "%s enable bluedroid failed: %s\n", __func__, esp_err_to_name(ret));
        return;
    }

    bt_app_gap_start_up();
}

void bt_app_gap_start_up()
{
    char *dev_name = "ESP_GAP_INQRUIY";
    esp_bt_dev_set_device_name(dev_name);

    /* set discoverable and connectable mode, wait to be connected */
    esp_bt_gap_set_scan_mode(ESP_BT_SCAN_MODE_CONNECTABLE_DISCOVERABLE);

    /* register GAP callback function */
    esp_bt_gap_register_callback(bt_app_gap_cb);

    /* inititialize device information and status */
    app_gap_cb_t *p_dev = &m_dev_info;
    memset(p_dev, 0, sizeof(app_gap_cb_t));

    /* start to discover nearby Bluetooth devices */
    p_dev->state = APP_GAP_STATE_DEVICE_DISCOVERING;
    esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 10, 0);
}

void bt_app_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param) {
    app_gap_cb_t *p_dev = &m_dev_info;
    char bda_str[18];
    char uuid_str[37];

    switch (event) {
    case ESP_BT_GAP_DISC_RES_EVT: {
        update_device_info(param);
        break;
    }
    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT: {
        if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED) {
            ESP_LOGI(GAP_TAG, "Device discovery stopped.");
            if ( (p_dev->state == APP_GAP_STATE_DEVICE_DISCOVER_COMPLETE ||
                    p_dev->state == APP_GAP_STATE_DEVICE_DISCOVERING)
                    && p_dev->dev_found) {
                p_dev->state = APP_GAP_STATE_SERVICE_DISCOVERING;
                ESP_LOGI(GAP_TAG, "Discover services ...");
                esp_bt_gap_get_remote_services(p_dev->bda);
            }
        } else if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STARTED) {
            ESP_LOGI(GAP_TAG, "Discovery started.");
        }
        break;
    }
    case ESP_BT_GAP_RMT_SRVCS_EVT: {
        if (memcmp(param->rmt_srvcs.bda, p_dev->bda, ESP_BD_ADDR_LEN) == 0 &&
                p_dev->state == APP_GAP_STATE_SERVICE_DISCOVERING) {
            p_dev->state = APP_GAP_STATE_SERVICE_DISCOVER_COMPLETE;
            if (param->rmt_srvcs.stat == ESP_BT_STATUS_SUCCESS) {
                ESP_LOGI(GAP_TAG, "Services for device %s found",  bda2str(p_dev->bda, bda_str, 18));
                for (int i = 0; i < param->rmt_srvcs.num_uuids; i++) {
                    esp_bt_uuid_t *u = param->rmt_srvcs.uuid_list + i;
                    ESP_LOGI(GAP_TAG, "--%s", uuid2str(u, uuid_str, 37));
                    // ESP_LOGI(GAP_TAG, "--%d", u->len);
                }
            } else {
                ESP_LOGI(GAP_TAG, "Services for device %s not found",  bda2str(p_dev->bda, bda_str, 18));
            }
        }
        break;
    }
    case ESP_BT_GAP_RMT_SRVC_REC_EVT:
    default: {
        ESP_LOGI(GAP_TAG, "event: %d", event);
        break;
    }
    }
    return;
}

static void update_device_info(esp_bt_gap_cb_param_t *param)
{
  char bda_str[18];
  uint32_t cod = 0;
  int32_t rssi = -129; /* invalid value */
  esp_bt_gap_dev_prop_t *p;
  ESP_LOGI(GAP_TAG, "Device found: %s", bda2str(param->disc_res.bda, bda_str, 18));
  for (int i = 0; i < param->disc_res.num_prop; i++) {
    p = param->disc_res.prop + i;
    switch (p->type) {
    case ESP_BT_GAP_DEV_PROP_COD:
      cod = *(uint32_t *)(p->val);
      ESP_LOGI(GAP_TAG, "--Class of Device: 0x%x", cod);
    break;
    case ESP_BT_GAP_DEV_PROP_RSSI:
      rssi = *(int8_t *)(p->val);
      ESP_LOGI(GAP_TAG, "--RSSI: %d", rssi);
    break;
    case ESP_BT_GAP_DEV_PROP_BDNAME:
    break;
    default:
    break;
    }
  }
  /* search for device with MAJOR service class as "rendering" in COD */
  app_gap_cb_t *p_dev = &m_dev_info;
  if (p_dev->dev_found && 0 != memcmp(param->disc_res.bda, p_dev->bda, ESP_BD_ADDR_LEN)) {
      return;
  }
  if (!esp_bt_gap_is_valid_cod(cod) ||
          !(esp_bt_gap_get_cod_major_dev(cod) == ESP_BT_COD_MAJOR_DEV_PHONE)) {
      return;
  }
  memcpy(p_dev->bda, param->disc_res.bda, ESP_BD_ADDR_LEN);
  p_dev->dev_found = true;
  for (int i = 0; i < param->disc_res.num_prop; i++) {
      p = param->disc_res.prop + i;
      switch (p->type) {
      case ESP_BT_GAP_DEV_PROP_COD:
          p_dev->cod = *(uint32_t *)(p->val);
          break;
      case ESP_BT_GAP_DEV_PROP_RSSI:
          p_dev->rssi = *(int8_t *)(p->val);
          break;
      case ESP_BT_GAP_DEV_PROP_BDNAME: {
          uint8_t len = (p->len > ESP_BT_GAP_MAX_BDNAME_LEN) ? ESP_BT_GAP_MAX_BDNAME_LEN :
                        (uint8_t)p->len;
          memcpy(p_dev->bdname, (uint8_t *)(p->val), len);
          p_dev->bdname[len] = '\0';
          p_dev->bdname_len = len;
          break;
      }
      case ESP_BT_GAP_DEV_PROP_EIR: {
          memcpy(p_dev->eir, (uint8_t *)(p->val), p->len);
          p_dev->eir_len = p->len;
          break;
      }
      default:
          break;
      }
  }
  if (p_dev->eir && p_dev->bdname_len == 0) {
      get_name_from_eir(p_dev->eir, p_dev->bdname, &p_dev->bdname_len);
      ESP_LOGI(GAP_TAG, "Found a target device, address %s, name %s", bda_str, p_dev->bdname);
      p_dev->state = APP_GAP_STATE_DEVICE_DISCOVER_COMPLETE;
      ESP_LOGI(GAP_TAG, "Cancel device discovery ...");
      esp_bt_gap_cancel_discovery();
  }
}

static char *bda2str(esp_bd_addr_t bda, char *str, size_t size)
{
    if (bda == NULL || str == NULL || size < 18) {
        return NULL;
    }

    uint8_t *p = bda;
    sprintf(str, "%02x:%02x:%02x:%02x:%02x:%02x",
            p[0], p[1], p[2], p[3], p[4], p[5]);
    return str;
}

static char *uuid2str(esp_bt_uuid_t *uuid, char *str, size_t size)
{
    if (uuid == NULL || str == NULL) {
        return NULL;
    }

    if (uuid->len == 2 && size >= 5) {
        sprintf(str, "%04x", uuid->uuid.uuid16);
    } else if (uuid->len == 4 && size >= 9) {
        sprintf(str, "%08x", uuid->uuid.uuid32);
    } else if (uuid->len == 16 && size >= 37) {
        uint8_t *p = uuid->uuid.uuid128;
        sprintf(str, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                p[15], p[14], p[13], p[12], p[11], p[10], p[9], p[8],
                p[7], p[6], p[5], p[4], p[3], p[2], p[1], p[0]);
    } else {
        return NULL;
    }

    return str;
}

static bool get_name_from_eir(uint8_t *eir, uint8_t *bdname, uint8_t *bdname_len)
{
    uint8_t *rmt_bdname = NULL;
    uint8_t rmt_bdname_len = 0;

    if (!eir) {
        return false;
    }

    rmt_bdname = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &rmt_bdname_len);
    if (!rmt_bdname) {
        rmt_bdname = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, &rmt_bdname_len);
    }

    if (rmt_bdname) {
        if (rmt_bdname_len > ESP_BT_GAP_MAX_BDNAME_LEN) {
            rmt_bdname_len = ESP_BT_GAP_MAX_BDNAME_LEN;
        }

        if (bdname) {
            memcpy(bdname, rmt_bdname, rmt_bdname_len);
            bdname[rmt_bdname_len] = '\0';
        }
        if (bdname_len) {
            *bdname_len = rmt_bdname_len;
        }
        return true;
    }

    return false;
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
          xTaskCreatePinnedToCore(QrCodeReader, "read", 10000, NULL, 2, NULL, 0);
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

void QrCodeReader(void *args) {
  uint8_t *dataReader = (uint8_t *)malloc(BUF_SIZE);
  String qrCode = emptyString;
  qrCode.reserve(10);
  int len = uart_read_bytes(UART_NUM_1, dataReader, BUF_SIZE, pdMS_TO_TICKS(100));
  if (len > 0)
  {
    qrCode = String((char *)dataReader).substring(0, 8);
    qrCode.trim();

    uart_flush(UART_NUM_1);
    uart_flush_input(UART_NUM_1);
    while (true)
    {
      vTaskDelay(pdMS_TO_TICKS(100));
      publishMQTT(config.pubQrCodeReceiver, qrCode);
      vTaskDelay(pdMS_TO_TICKS(100));
      free(dataReader);
      dataReader = NULL;
      vTaskDelete(NULL);
      return;
    }
  }
  uart_flush(UART_NUM_1);
  uart_flush_input(UART_NUM_1);
  free(dataReader);
  dataReader = NULL;
  vTaskDelete(NULL);
  return;
}

/* WEBSERVER */
// void webServer(void *pvParameters){
//   while (true) {
//     // WatchDog ----------------------------------------
//     yield();
//     // DNS ---------------------------------------------
//     dnsServer.processNextRequest();

//     // Web ---------------------------------------------
//     server.handleClient();
//   }
// }

// Requisições Web --------------------------------------
void handleHome() {
  // Home
  File file = SPIFFS.open("/Home.htm", "r");
  if (file) {
    file.setTimeout(100);
    String s = file.readString();
    file.close();

    // Atualiza conteúdo dinâmico
    s.replace("#deviceIp#", config.deviceIp);
    s.replace("#externo#", config.external == true ? "Sim" : "Não");
    s.replace("#saida#", config.isExit == true ? "Sim" : "Não");
    s.replace("#mqttServer#", config.mqttServer);
    s.replace("#mqttPort#", config.mqttPort);
    s.replace("#mqttUser#", config.mqttUser);
    s.replace("#mqttPass#", config.mqttPassword);
    s.replace("#topicoThinClient#", config.pubQrCodeReceiver);
    s.replace("#macAdress#", config.macAdress);
    s.replace("#active#"   , longTimeStr(millis() / 1000));
    s.replace("#userAgent#", server.header("User-Agent"));

    // Envia dados
    server.send(200, F("text/html"), s);
    Serial.println("Home - Cliente: " + ipStr(server.client().remoteIP()) +
        (server.uri() != "/" ? " [" + server.uri() + "]" : ""));
  } else {
    server.send(500, F("text/plain"), F("Home - ERROR 500"));
    Serial.print("Home - ERRO lendo arquivo");
  }
}

void handleConfig() {
  // Config
  File file = SPIFFS.open("/Config.htm", "r");
  if (file) {
    file.setTimeout(100);
    String s = file.readString();
    file.close();

    // Atualiza conteúdo dinâmico
    s.replace("#deviceIp#", config.deviceIp);
    s.replace("#externalOn#", config.external ? "checked" : "");
    s.replace("#externalOff#", !config.external ? "checked" : "");
    s.replace("#isExitOn#", config.isExit ? "checked" : "");
    s.replace("#isExitOff#", !config.isExit ? "checked" : "");
    s.replace("#mqttServer#", config.mqttServer);
    s.replace("#mqttPort#", config.mqttPort);
    s.replace("#mqttUser#", config.mqttUser);
    s.replace("#mqttPass#", config.mqttPassword);
    s.replace("#topicoThinClient#", config.pubQrCodeReceiver);
 
    // Send data
    server.send(200, "text/html", s);
  } else {
    server.send(500,"text/plain", "Config - ERROR 500");
    Serial.println("Config - ERRO lendo arquivo");
  }
}

void handleConfigSave() {
  // Grava Config
  if (server.args() == 8) {
    String serverArgs;

    serverArgs = server.arg("deviceIp");
    serverArgs.trim();
    strlcpy(config.deviceIp, serverArgs.c_str(), sizeof(config.deviceIp));

    serverArgs = server.arg("external");
    serverArgs.trim();
    config.external = (bool)serverArgs.c_str();

    serverArgs = server.arg("isExit");
    serverArgs.trim();
    config.isExit = (bool)serverArgs.c_str();

    serverArgs = server.arg("mqttPass");
    serverArgs.trim();
    strlcpy(config.mqttPassword, serverArgs.c_str(), sizeof(config.mqttPassword));

    
    serverArgs = server.arg("mqttPort");
    serverArgs.trim();
    strlcpy(config.mqttPort, serverArgs.c_str(), sizeof(config.mqttPort));


    serverArgs = server.arg("mqttServer");
    serverArgs.trim();
    strlcpy(config.mqttServer, serverArgs.c_str(), sizeof(config.mqttServer));


    serverArgs = server.arg("mqttUser");
    serverArgs.trim();
    strlcpy(config.mqttUser, serverArgs.c_str(), sizeof(config.mqttUser));


    serverArgs = server.arg("topicoThinClient");
    serverArgs.trim();
    strlcpy(config.pubQrCodeReceiver, serverArgs.c_str(), sizeof(config.pubQrCodeReceiver));

    // Grava configuração
    if (configSave()) {
      server.send(200, F("text/html"), F("<html><meta charset='UTF-8'><script>alert('Configuração salva.');history.back()</script></html>"));
      Serial.println("ConfigSave - Cliente: " + ipStr(server.client().remoteIP()));
    } else {
      server.send(200, F("text/html"), F("<html><meta charset='UTF-8'><script>alert('Falha salvando configuração.');history.back()</script></html>"));
      Serial.print("ConfigSave - ERRO salvando Config");
    }
  } else {
    server.send(200, F("text/html"), F("<html><meta charset='UTF-8'><script>alert('Erro de parâmetros.');history.back()</script></html>"));
  }
}

void handleReconfig() {
  // Reinicia Config
  configReset();

  // Grava configuração
  if (configSave()) {
    server.send(200, F("text/html"), F("<html><meta charset='UTF-8'><script>alert('Configuração reiniciada.');window.location = '/'</script></html>"));
    Serial.println("Reconfig - Cliente: " + ipStr(server.client().remoteIP()));
  } else {
    server.send(200, F("text/html"), F("<html><meta charset='UTF-8'><script>alert('Falha reiniciando configuração.');history.back()</script></html>"));
    Serial.print("Reconfig - ERRO reiniciando Config");
  }
}

void handleReboot() {
  // Reboot
  File file = SPIFFS.open("/Reboot.htm", "r");
  if (file) {
    server.streamFile(file, "text/html");
    file.close();
    Serial.println("Reboot - Cliente: " + ipStr(server.client().remoteIP()));
    delay(100);
    ESP.restart();
  } else {
    server.send(500, F("text/plain"), F("Reboot - ERROR 500"));
    Serial.println("Reboot - ERRO lendo arquivo");
  }
}

void handleCSS() {
  // Arquivo CSS
  File file = SPIFFS.open("/Style.css", "r");
  if (file) {
    // Define cache para 3 dias
    server.sendHeader("Cache-Control", "public, max-age=172800");
    server.streamFile(file, "text/css");
    file.close();
    Serial.println("CSS - Cliente: " + ipStr(server.client().remoteIP()));
  } else {
    server.send(500, F("text/plain"), F("CSS - ERROR 500"));
    Serial.println("CSS - ERRO lendo arquivo");
  }
}
