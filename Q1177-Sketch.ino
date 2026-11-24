// Casa Inteligente do Zero - TM2 #40
// ESP32 + INMP441: a IA ouve a panela de pressão e avisa o Home Assistant via MQTT
#include <pressao_inferencing.h>
#include <ESP_I2S.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include "secret.h"

// ================== CONFIGURAÇÃO ==================
//const char* WIFI_NOME    = "NOME_DO_WIFI";
//const char* WIFI_SENHA   = "SENHA_DO_WIFI";
const char* MQTT_HOST    = "192.168.50.10";   // IP do Home Assistant
const int   MQTT_PORTA   = 1883;
const char* MQTT_USUARIO = "mqtt";
//const char* MQTT_SENHA   = "DIGITE_A_SENHA_AQUI";

const float LIMIAR_LIGA      = 0.80;   // certeza mínima para contar como "apito"
const float LIMIAR_DESLIGA   = 0.50;   // abaixo disso, conta como "silêncio"
const int   CONFIRMA_LIGA    = 3;      // leituras seguidas para LIGAR
const int   CONFIRMA_DESLIGA = 6;      // leituras seguidas para DESLIGAR
// ==================================================

const int PINO_SCK = 26;
const int PINO_WS  = 25;
const int PINO_SD  = 33;

const int TAXA   = EI_CLASSIFIER_FREQUENCY;          // 16000 (vem do modelo)
const int JANELA = EI_CLASSIFIER_RAW_SAMPLE_COUNT;   // 16000 amostras = 1 s
const int PASSO  = JANELA / 2;                       // 0,5 s novo a cada decisão

const char* TOPICO_CONFIG = "homeassistant/binary_sensor/panela_esp32/config";
const char* TOPICO_ESTADO = "casa-que-ouve-esp32/panela/estado";
const char* TOPICO_ATRIB  = "casa-que-ouve-esp32/panela/atributos";
const char* TOPICO_DISP   = "casa-que-ouve-esp32/disponivel";

// Anúncio automático do sensor para o Home Assistant (MQTT Discovery)
const char* CONFIG_SENSOR =
  "{"
  "\"name\":\"Panela de pressão\","
  "\"unique_id\":\"casa_que_ouve_panela_esp32\","
  "\"state_topic\":\"casa-que-ouve-esp32/panela/estado\","
  "\"json_attributes_topic\":\"casa-que-ouve-esp32/panela/atributos\","
  "\"availability_topic\":\"casa-que-ouve-esp32/disponivel\","
  "\"device_class\":\"sound\","
  "\"icon\":\"mdi:pot-steam\","
  "\"payload_on\":\"ON\",\"payload_off\":\"OFF\","
  "\"device\":{"
    "\"identifiers\":[\"casa_que_ouve_esp32\"],"
    "\"name\":\"Casa que Ouve (ESP32)\","
    "\"model\":\"Edge Impulse + ESP32 + INMP441\""
  "}"
  "}";

I2SClass i2s;
WiFiClient wifiCliente;
PubSubClient mqtt(wifiCliente);

int16_t janela[JANELA];      // o último 1 s de som
int32_t bruto[PASSO];        // o meio segundo que acabou de chegar

bool ligado = false;         // estado do sensor (apitando ou não)
int  contador = 0;

// ---------- Wi-Fi: tenta da potência maior para a menor ----------
void conectarWiFi() {
  const wifi_power_t potencias[] = { WIFI_POWER_19_5dBm, WIFI_POWER_15dBm,
                                     WIFI_POWER_11dBm, WIFI_POWER_8_5dBm,
                                     WIFI_POWER_5dBm };
  const char* nomes[] = { "19.5", "15", "11", "8.5", "5" };
  WiFi.mode(WIFI_STA);

  while (true) {
    for (int i = 0; i < 5; i++) {
      Serial.printf("Wi-Fi com potencia %s dBm", nomes[i]);
      WiFi.begin(WIFI_NOME, WIFI_SENHA);
      WiFi.setTxPower(potencias[i]);
      unsigned long inicio = millis();
      while (WiFi.status() != WL_CONNECTED && millis() - inicio < 8000) {
        delay(500);
        Serial.print(".");
      }
      if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("\nWi-Fi OK com %s dBm! IP: %s\n",
                      nomes[i], WiFi.localIP().toString().c_str());
        return;
      }
      Serial.println(" nao conectou.");
      WiFi.disconnect(true);
      delay(1000);
    }
    Serial.println("Nenhuma potencia funcionou. Confira o nome e a senha. Tentando de novo...");
  }
}

// ---------- MQTT: conecta e se apresenta ao Home Assistant ----------
void publicarEstado(float certeza) {
  mqtt.publish(TOPICO_ESTADO, ligado ? "ON" : "OFF", true);
  char atrib[40];
  snprintf(atrib, sizeof(atrib), "{\"certeza\":%.3f}", certeza);
  mqtt.publish(TOPICO_ATRIB, atrib, true);
  Serial.printf(">>> Sensor agora: %s (certeza %.2f)\n", ligado ? "ON" : "OFF", certeza);
}

void conectarMQTT() {
  while (!mqtt.connected()) {
    if (WiFi.status() != WL_CONNECTED) conectarWiFi();
    Serial.print("Conectando no MQTT... ");
    // o "testamento": se o ESP32 sumir, o broker avisa "offline" sozinho
    if (mqtt.connect("casa-que-ouve-esp32", MQTT_USUARIO, MQTT_SENHA,
                     TOPICO_DISP, 0, true, "offline")) {
      Serial.println("OK!");
      mqtt.publish(TOPICO_CONFIG, CONFIG_SENSOR, true);
      mqtt.publish(TOPICO_DISP, "online", true);
      publicarEstado(0.0);
    } else {
      Serial.printf("falhou (codigo %d). Tentando em 5 s...\n", mqtt.state());
      delay(5000);
    }
  }
}

// ---------- Áudio ----------
int pegaAudio(size_t inicio, size_t tamanho, float* saida) {
  numpy::int16_to_float(&janela[inicio], saida, tamanho);
  return 0;
}

void lerPasso() {
  size_t lidos = 0;
  while (lidos < sizeof(bruto)) {
    lidos += i2s.readBytes((char*)bruto + lidos, sizeof(bruto) - lidos);
  }
  // mesma escala e mesma retirada do "desvio" usadas na gravação
  long long soma = 0;
  for (int i = 0; i < PASSO; i++) {
    bruto[i] >>= 16;
    soma += bruto[i];
  }
  int media = soma / PASSO;
  memmove(janela, janela + PASSO, (JANELA - PASSO) * sizeof(int16_t));
  for (int i = 0; i < PASSO; i++) {
    janela[JANELA - PASSO + i] = constrain(bruto[i] - media, -32768, 32767);
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  i2s.setPins(PINO_SCK, PINO_WS, -1, PINO_SD);
  if (!i2s.begin(I2S_MODE_STD, TAXA, I2S_DATA_BIT_WIDTH_32BIT,
                 I2S_SLOT_MODE_MONO, I2S_STD_SLOT_LEFT)) {
    Serial.println("Falha ao iniciar o microfone");
    while (true) delay(1000);
  }

  conectarWiFi();
  mqtt.setServer(MQTT_HOST, MQTT_PORTA);
  mqtt.setBufferSize(1024);   // o anúncio do sensor é maior que o padrão (256 bytes)
  conectarMQTT();

  lerPasso();   // enche o primeiro 1 s
  lerPasso();
  Serial.println("Ouvindo a cozinha...");
}

void loop() {
  if (!mqtt.connected()) conectarMQTT();
  mqtt.loop();

  lerPasso();

  signal_t sinal;
  sinal.total_length = JANELA;
  sinal.get_data = &pegaAudio;

  ei_impulse_result_t resultado = { 0 };
  if (run_classifier(&sinal, &resultado, false) != EI_IMPULSE_OK) {
    Serial.println("Erro na IA");
    return;
  }

  float p = 0;
  for (int i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
    if (strcmp(resultado.classification[i].label, "pressao") == 0) {
      p = resultado.classification[i].value;
    }
  }

  // filtro anti-alarme-falso (o mesmo do Dell e do UNO Q)
  if (!ligado) {
    contador = (p >= LIMIAR_LIGA) ? contador + 1 : 0;
    if (contador >= CONFIRMA_LIGA) { ligado = true;  contador = 0; publicarEstado(p); }
  } else {
    contador = (p < LIMIAR_DESLIGA) ? contador + 1 : 0;
    if (contador >= CONFIRMA_DESLIGA) { ligado = false; contador = 0; publicarEstado(p); }
  }

  Serial.printf("pressao=%.2f  estado=%s  contador=%d  (%d ms)\n",
                p, ligado ? "ON" : "OFF", contador,
                resultado.timing.dsp + resultado.timing.classification);
}