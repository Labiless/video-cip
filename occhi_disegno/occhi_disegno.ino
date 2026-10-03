// Due OLED 128x64 affiancati = un unico schermo 256x64
// Riceve dalla seriale USB:
//   P x y   -> accende il pixel (x: 0-255, y: 0-63)
//   C       -> cancella tutto
//   F + 2048 byte binari -> frame intero; risponde 'K' quando l'ha mostrato
//     byte 0-1023 = schermo sinistro, 1024-2047 = destro, nel formato del buffer
//     SSD1306: 8 pagine da 128 byte, ogni byte = 8 pixel in verticale (bit 0 in alto)
//
// Invia alla seriale USB:
//   K            -> frame mostrato, pronto per il successivo
//   E modo n     -> encoder: n scatti (negativo = antiorario) nella funzione "modo";
//                   n = 0 quando un click ha cambiato funzione
//
// Rotary encoder: click = funzione successiva (il LED cambia colore), rotazione = regola il valore
//   EC11: A -> GP4, B -> GP5, C (centrale) -> GND; pulsante: un piedino -> GP6, l'altro -> GND

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

// Una funzione dell'encoder per colore, nello stesso ordine della pagina gif_oled.html
const uint8_t COLORI[][3] = {
  {255, 0, 0},    // 0 rosso:     cambia GIF dalla libreria
  {255, 96, 0},   // 1 arancione: zoom
  {255, 220, 0},  // 2 giallo:    sposta a destra/sinistra
  {0, 255, 0},    // 3 verde:     soglia
  {0, 255, 255},  // 4 ciano:     inverti (orario = on, antiorario = off)
  {0, 0, 255},    // 5 blu:       modalità di conversione
  {200, 0, 255},  // 6 viola:     modalità di adattamento
  {255, 255, 255},// 7 bianco:    contenuto (orario = duplicata, antiorario = schermo unico)
  {255, 30, 120}, // 8 rosa:      anticipo del secondo schermo
  {150, 255, 0},  // 9 lime:      velocità di riproduzione
};
const uint8_t N_MODI = sizeof(COLORI) / sizeof(COLORI[0]);
uint8_t modo = 0;

const uint8_t LUMINOSITA_LED = 10;  // percentuale (100 = massimo)

void mostraLed() {
  led.setPixelColor(0, COLORI[modo][0] * LUMINOSITA_LED / 100, COLORI[modo][1] * LUMINOSITA_LED / 100,
                       COLORI[modo][2] * LUMINOSITA_LED / 100);
  led.show();
}

// ---------- Rotary encoder ----------
const uint8_t PIN_CLK = 4, PIN_DT = 5, PIN_SW = 6;
const int8_t PASSI_PER_SCATTO = 4;  // la maggior parte degli encoder (EC11, KY-040) fa 4 transizioni per scatto;
                                    // se ogni scatto conta doppio, metti 2

// Letto via interrupt: gli scatti non si perdono anche mentre gli schermi si aggiornano
volatile int32_t passiEncoder = 0;
volatile uint8_t statoAB = 0;

void encoderISR() {
  // Tabella di transizione in quadratura: +1/-1 per i passaggi validi, 0 per rimbalzi e salti
  static const int8_t TABELLA[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};
  statoAB = ((statoAB << 2) | (digitalRead(PIN_CLK) << 1) | digitalRead(PIN_DT)) & 0x0F;
  passiEncoder += TABELLA[statoAB];
}

// Comunica alla pagina web: "E modo scatti\n" (scatti = 0 quando cambia solo la funzione)
void inviaEvento(int32_t scatti) {
  Serial.print('E'); Serial.print(' ');
  Serial.print(modo); Serial.print(' ');
  Serial.print(scatti); Serial.print('\n');
}

void gestisciEncoder() {
  noInterrupts();
  int32_t passi = passiEncoder;
  int32_t scatti = passi / PASSI_PER_SCATTO;
  passiEncoder = passi - scatti * PASSI_PER_SCATTO;  // il resto vale per il prossimo scatto
  interrupts();
  // Se girando in senso orario i valori scendono, scambia i fili CLK e DT
  if (scatti) inviaEvento(scatti);

  // Pulsante con antirimbalzo: conta il click quando lo stato resta stabile per 30 ms
  static bool premuto = false, ultimaLettura = false;
  static uint32_t cambiato = 0;
  bool lettura = digitalRead(PIN_SW) == LOW;
  if (lettura != ultimaLettura) { ultimaLettura = lettura; cambiato = millis(); }
  if (lettura != premuto && millis() - cambiato > 30) {
    premuto = lettura;
    if (premuto) {
      modo = (modo + 1) % N_MODI;
      mostraLed();
      inviaEvento(0);
    }
  }
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
  mostraLed();

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
