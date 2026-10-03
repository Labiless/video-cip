// Due OLED 128x64 affiancati = un unico schermo 256x64
// Riceve dalla seriale USB:
//   P x y   -> accende il pixel (x: 0-255, y: 0-63)
//   C       -> cancella tutto
//   F + 2048 byte binari -> frame intero; risponde 'K' quando l'ha mostrato
//     byte 0-1023 = schermo sinistro, 1024-2047 = destro, nel formato del buffer
//     SSD1306: 8 pagine da 128 byte, ogni byte = 8 pixel in verticale (bit 0 in alto)

#include <Wire.h>
#include <U8g2lib.h>

// Schermo sinistro su I2C0 (GP0/GP1), destro su I2C1 (GP2/GP3)
// Se uno schermo risulta capovolto usa setFlipMode(1) in setup(), non U8G2_R2:
// i frame binari vengono copiati direttamente nel buffer e ignorano la rotazione software
U8G2_SSD1306_128X64_NONAME_F_HW_I2C     sx(U8G2_R0, U8X8_PIN_NONE);
U8G2_SSD1306_128X64_NONAME_F_2ND_HW_I2C dx(U8G2_R0, U8X8_PIN_NONE);

// I costruttori _F_ dello stesso modello condividono un unico buffer statico:
// senza questo, sx e dx disegnerebbero nella stessa memoria (schermi clonati)
const uint16_t BYTE_SCHERMO = 128 * 64 / 8;
uint8_t bufferDx[BYTE_SCHERMO];

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
}

void loop() {
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
