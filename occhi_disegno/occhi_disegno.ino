// Due OLED 128x64 affiancati = un unico schermo 256x64
// Riceve dalla seriale USB:
//   P x y   -> accende il pixel (x: 0-255, y: 0-63)
//   C       -> cancella tutto
//   F + 2048 byte binari -> frame intero; risponde 'K' quando l'ha mostrato
//     byte 0-1023 = schermo sinistro, 1024-2047 = destro, nel formato del buffer
//     SSD1306: 8 pagine da 128 byte, ogni byte = 8 pixel in verticale (bit 0 in alto)
//
// Rotary encoder: click = cambia colore del LED sulla board, rotazione = luminosità
//   CLK -> GP4, DT -> GP5, SW -> GP6, + -> 3V3, GND -> GND

#include <Wire.h>
#include <U8g2lib.h>
#include <Adafruit_NeoPixel.h>  // da installare: Gestione librerie -> "Adafruit NeoPixel"

// Schermo sinistro su I2C0 (GP0/GP1), destro su I2C1 (GP2/GP3)
// Se uno schermo risulta capovolto usa setFlipMode(1) in setup(), non U8G2_R2:
// i frame binari vengono copiati direttamente nel buffer e ignorano la rotazione software
U8G2_SSD1306_128X64_NONAME_F_HW_I2C     sx(U8G2_R0, U8X8_PIN_NONE);
U8G2_SSD1306_128X64_NONAME_F_2ND_HW_I2C dx(U8G2_R0, U8X8_PIN_NONE);

// I costruttori _F_ dello stesso modello condividono un unico buffer statico:
// senza questo, sx e dx disegnerebbero nella stessa memoria (schermi clonati)
const uint16_t BYTE_SCHERMO = 128 * 64 / 8;
uint8_t bufferDx[BYTE_SCHERMO];

// ---------- LED RGB sulla board (WS2812 su GP16) ----------
// Se rosso e verde risultano scambiati, cambia NEO_GRB in NEO_RGB
Adafruit_NeoPixel led(1, 16, NEO_GRB + NEO_KHZ800);

const uint8_t COLORI[][3] = {
  {255, 0, 0}, {255, 96, 0}, {255, 220, 0}, {0, 255, 0},
  {0, 255, 255}, {0, 0, 255}, {200, 0, 255}, {255, 255, 255},
};
const uint8_t N_COLORI = sizeof(COLORI) / sizeof(COLORI[0]);
const int8_t LIVELLI = 16;     // gradini di luminosità (0 = spento)
uint8_t colore = 0;
int8_t livello = 8;
bool aggiornaLed = true;

void mostraLed() {
  // Scala quadratica: l'occhio percepisce meglio i gradini a bassa luminosità
  uint16_t lum = (uint16_t)livello * livello * 255 / (LIVELLI * LIVELLI);
  led.setPixelColor(0, COLORI[colore][0] * lum / 255, COLORI[colore][1] * lum / 255,
                       COLORI[colore][2] * lum / 255);
  led.show();
}

// ---------- Rotary encoder ----------
const uint8_t PIN_CLK = 4, PIN_DT = 5, PIN_SW = 6;
const int8_t PASSI_PER_SCATTO = 4;  // la maggior parte degli encoder (KY-040) fa 4 transizioni per scatto;
                                    // se ogni scatto cambia di 2 livelli, metti 2

// Letto via interrupt: gli scatti non si perdono anche mentre gli schermi si aggiornano
volatile int32_t passiEncoder = 0;
volatile uint8_t statoAB = 0;

void encoderISR() {
  // Tabella di transizione in quadratura: +1/-1 per i passaggi validi, 0 per rimbalzi e salti
  static const int8_t TABELLA[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};
  statoAB = ((statoAB << 2) | (digitalRead(PIN_CLK) << 1) | digitalRead(PIN_DT)) & 0x0F;
  passiEncoder += TABELLA[statoAB];
}

void gestisciEncoder() {
  noInterrupts();
  int32_t passi = passiEncoder;
  int32_t scatti = passi / PASSI_PER_SCATTO;
  passiEncoder = passi - scatti * PASSI_PER_SCATTO;  // il resto vale per il prossimo scatto
  interrupts();
  if (scatti) {
    // Se girando in senso orario la luminosità scende, scambia i fili CLK e DT
    livello = constrain(livello + scatti, 0, LIVELLI);
    aggiornaLed = true;
  }

  // Pulsante con antirimbalzo: conta il click quando lo stato resta stabile per 30 ms
  static bool premuto = false, ultimaLettura = false;
  static uint32_t cambiato = 0;
  bool lettura = digitalRead(PIN_SW) == LOW;
  if (lettura != ultimaLettura) { ultimaLettura = lettura; cambiato = millis(); }
  if (lettura != premuto && millis() - cambiato > 30) {
    premuto = lettura;
    if (premuto) { colore = (colore + 1) % N_COLORI; aggiornaLed = true; }
  }

  if (aggiornaLed) { mostraLed(); aggiornaLed = false; }
}

// ---------- Seriale ----------
char riga[32];
uint8_t len = 0;
bool aggiornaSx = false, aggiornaDx = false;

// Ricezione di un frame binario in corso
bool inFrame = false;
uint16_t ricevuti = 0;
uint32_t ultimoByte = 0;
bool frameCompleto = false;

void gestisci(char *cmd) {
  if (cmd[0] == 'P') {
    int x, y;
    if (sscanf(cmd + 1, "%d %d", &x, &y) == 2 && x >= 0 && x < 256 && y >= 0 && y < 64) {
      if (x < 128) { sx.drawPixel(x, y);       aggiornaSx = true; }
      else         { dx.drawPixel(x - 128, y); aggiornaDx = true; }
    }
  } else if (cmd[0] == 'C') {
    sx.clearBuffer(); dx.clearBuffer();
    aggiornaSx = aggiornaDx = true;
  }
}

// Copia i byte del frame direttamente nei buffer dei due schermi
void riceviFrame() {
  while (ricevuti < 2 * BYTE_SCHERMO && Serial.available()) {
    uint8_t *dest = ricevuti < BYTE_SCHERMO ? sx.getBufferPtr() + ricevuti
                                            : bufferDx + (ricevuti - BYTE_SCHERMO);
    uint16_t resto = (ricevuti < BYTE_SCHERMO ? BYTE_SCHERMO : 2 * BYTE_SCHERMO) - ricevuti;
    uint16_t n = Serial.readBytes(dest, min((int)resto, Serial.available()));
    ricevuti += n;
    ultimoByte = millis();
  }
  if (ricevuti == 2 * BYTE_SCHERMO) {
    inFrame = false;
    frameCompleto = true;
  }
}

void setup() {
  Serial.begin(115200);

  Wire.setSDA(0);  Wire.setSCL(1);
  Wire1.setSDA(2); Wire1.setSCL(3);

  dx.getU8g2()->tile_buf_ptr = bufferDx;  // buffer separato per lo schermo destro

  sx.setBusClock(400000);  // I2C veloce: aggiornamenti più fluidi
  dx.setBusClock(400000);
  sx.begin();
  dx.begin();

  sx.clearBuffer(); sx.sendBuffer();
  dx.clearBuffer(); dx.sendBuffer();

  led.begin();

  pinMode(PIN_CLK, INPUT_PULLUP);
  pinMode(PIN_DT, INPUT_PULLUP);
  pinMode(PIN_SW, INPUT_PULLUP);
  statoAB = (digitalRead(PIN_CLK) << 1) | digitalRead(PIN_DT);
  attachInterrupt(digitalPinToInterrupt(PIN_CLK), encoderISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_DT), encoderISR, CHANGE);
}

void loop() {
  gestisciEncoder();

  // Frame interrotto (es. pagina chiusa a metà invio): lo scarta
  if (inFrame && millis() - ultimoByte > 200) inFrame = false;

  // Legge tutti i comandi arrivati...
  while (Serial.available() && !frameCompleto) {
    if (inFrame) { riceviFrame(); continue; }
    char c = Serial.read();
    if (c == 'F' && len == 0) {
      inFrame = true; ricevuti = 0; ultimoByte = millis();
    } else if (c == '\n') {
      riga[len] = 0;
      gestisci(riga);
      len = 0;
    } else if (c != '\r' && len < sizeof(riga) - 1) {
      riga[len++] = c;
    }
  }

  if (frameCompleto) {
    sx.sendBuffer();
    dx.sendBuffer();
    aggiornaSx = aggiornaDx = false;
    frameCompleto = false;
    Serial.write('K');  // pronto per il frame successivo
    return;
  }

  // ...poi aggiorna solo gli schermi che sono cambiati
  if (aggiornaSx) { sx.sendBuffer(); aggiornaSx = false; }
  if (aggiornaDx) { dx.sendBuffer(); aggiornaDx = false; }
}
