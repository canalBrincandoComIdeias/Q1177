// Teste do INMP441: mostra o volume do som no Serial Plotter
#include <ESP_I2S.h>

const int PINO_SCK = 26;
const int PINO_WS  = 25;
const int PINO_SD  = 33;

I2SClass i2s;
const int N = 800;  // 800 amostras = 50 ms a 16 kHz
int32_t buffer[N];

void setup() {
  Serial.begin(115200);
  i2s.setPins(PINO_SCK, PINO_WS, -1, PINO_SD);
  if (!i2s.begin(I2S_MODE_STD, 16000, I2S_DATA_BIT_WIDTH_32BIT,
                 I2S_SLOT_MODE_MONO, I2S_STD_SLOT_LEFT)) {
    Serial.println("Falha ao iniciar o microfone");
    while (true) delay(1000);
  }
}

void loop() {
  size_t lidos = i2s.readBytes((char*)buffer, sizeof(buffer));
  int n = lidos / 4;
  if (n == 0) return;

  double soma = 0;
  int pico = 0;
  for (int i = 0; i < n; i++) {
    int s = buffer[i] >> 14;  // o INMP441 manda 24 bits dentro de 32; aqui ajustamos a escala
    soma += (double)s * s;
    if (abs(s) > pico) pico = abs(s);
  }
  int volume = sqrt(soma / n);
  Serial.printf("volume:%d,pico:%d\n", volume, pico);
}