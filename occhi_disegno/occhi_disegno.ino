// Due OLED 128x64 affiancati = un unico schermo 256x64
// Riceve dalla seriale USB:
//   P x y   -> accende il pixel (x: 0-255, y: 0-63)
//   C       -> cancella tutto
//   M n     -> seleziona la funzione n dell'encoder (come dopo n click)
//   Q       -> chiede la funzione attiva: risponde con "E modo 0"
//   R n r   -> seguito da n frame registrati (r = orientamento degli schermi: 0, 90, 180, 270),
//              ciascuno = 2 byte di durata in ms (little endian)
//              + 2048 byte come per F; li salva nella flash e risponde "S n",
//              oppure "S errore <motivo>" (fs, troppi, file, scrittura, interrotto).
//              R 0 cancella la registrazione.
//
// Risponde anche a Q con "I fs <1 se la flash ha spazio per i file, altrimenti 0> <frame salvati>"
//
// Senza pagina collegata (porta chiusa o board a batteria) riproduce in loop la registrazione;
// l'encoder continua a regolare zoom, posizione, inverti, velocità e glitch (le altre funzioni
// vengono saltate dal click, perché senza pagina non possono funzionare).
// Serve una partizione per i file: Strumenti -> Flash Size -> "2MB (Sketch: 1MB, FS: 1MB)"
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
#include <LittleFS.h>

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
// (la libreria, poi i riquadri della pagina da in alto a sinistra a in basso a destra)
const uint8_t COLORI[][3] = {
  {255, 0, 0},     // 0 rosso:     cambia GIF dalla libreria
  {130, 0, 255},   // 1 viola:     modalità di adattamento
  {255, 96, 0},    // 2 arancione: zoom
  {255, 220, 0},   // 3 giallo:    sposta a destra/sinistra
  {0, 60, 255},    // 4 blu:       modalità di conversione
  {0, 255, 0},     // 5 verde:     soglia
  {0, 255, 255},   // 6 ciano:     inverti (orario = on, antiorario = off)
  {255, 255, 255}, // 7 bianco:    contenuto (orario = duplicata, antiorario = schermo unico)
  {255, 0, 200},   // 8 magenta:   anticipo del secondo schermo
  {150, 255, 0},   // 9 lime:      velocità di riproduzione
  {255, 0, 200},   // 10 glitch:   lampeggia alternando ciano e questo magenta
};
const uint8_t MODO_GLITCH = 10;
const uint8_t N_MODI = sizeof(COLORI) / sizeof(COLORI[0]);
uint8_t modo = 0;

const uint8_t LUMINOSITA_LED = 10;  // percentuale (100 = massimo)

void mostraLed() {
  // Nel modo glitch il LED alterna ciano e magenta ogni 120 ms
  const uint8_t *c = COLORI[modo];
  if (modo == MODO_GLITCH && (millis() / 120) % 2) c = COLORI[6];
  led.setPixelColor(0, c[0] * LUMINOSITA_LED / 100, c[1] * LUMINOSITA_LED / 100, c[2] * LUMINOSITA_LED / 100);
  led.show();
}

void aggiornaLampeggio() {
  static uint32_t ultimaFase = 0;
  uint32_t fase = millis() / 120;
  if (modo == MODO_GLITCH && fase != ultimaFase) { ultimaFase = fase; mostraLed(); }
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

// ---------- Effetti senza pagina ----------
// Valori usati solo durante la riproduzione autonoma della registrazione
const uint8_t MODO_ZOOM = 2, MODO_POSIZIONE = 3, MODO_INVERTI = 6, MODO_VELOCITA = 9;
const uint16_t VELOCITA_PCT[] = {10, 25, 50, 75, 100, 125, 150, 200, 300, 400, 600};  // come nella pagina
const uint8_t N_VELOCITA = sizeof(VELOCITA_PCT) / sizeof(VELOCITA_PCT[0]);
const int16_t PASSO_SPOSTA = 4;     // pixel per scatto
const uint8_t PASSO_GLITCH = 4;     // punti percentuali per scatto
uint8_t velocitaIdx = 4;            // 100%
uint16_t zoomPct = 100;
int16_t spostaX = 0;
bool invertiLocale = false;
uint8_t glitchPct = 0;
bool ridisegna = false;             // un effetto è cambiato: rielabora il frame mostrato
bool paginaCollegata = true;

bool utileSenzaPagina(uint8_t m) {
  return m == MODO_ZOOM || m == MODO_POSIZIONE || m == MODO_INVERTI || m == MODO_VELOCITA || m == MODO_GLITCH;
}

// Passa alla prossima funzione utilizzabile (senza pagina salta quelle che non funzionano)
void funzioneSuccessiva(bool includiAttuale) {
  if (includiAttuale && (paginaCollegata || utileSenzaPagina(modo))) return;
  do { modo = (modo + 1) % N_MODI; } while (!paginaCollegata && !utileSenzaPagina(modo));
}

void applicaSenzaPagina(int32_t d) {
  for (int32_t k = 0; k < abs(d); k++) {
    bool su = d > 0;
    switch (modo) {
      case MODO_ZOOM:      zoomPct = su ? min(800, zoomPct * 110 / 100) : max(25, zoomPct * 100 / 110); break;
      case MODO_POSIZIONE: spostaX += su ? PASSO_SPOSTA : -PASSO_SPOSTA; break;
      case MODO_INVERTI:   invertiLocale = su; break;
      case MODO_VELOCITA:  if (su && velocitaIdx < N_VELOCITA - 1) velocitaIdx++;
                           if (!su && velocitaIdx > 0) velocitaIdx--; break;
      case MODO_GLITCH:    glitchPct = su ? min(100, glitchPct + PASSO_GLITCH) : max(0, glitchPct - PASSO_GLITCH); break;
    }
  }
  ridisegna = true;
}

void gestisciEncoder() {
  noInterrupts();
  int32_t passi = passiEncoder;
  int32_t scatti = passi / PASSI_PER_SCATTO;
  passiEncoder = passi - scatti * PASSI_PER_SCATTO;  // il resto vale per il prossimo scatto
  interrupts();
  // Se girando in senso orario i valori scendono, scambia i fili CLK e DT
  if (scatti) {
    if (paginaCollegata) inviaEvento(scatti);
    else applicaSenzaPagina(scatti);
  }

  // Pulsante con antirimbalzo: conta il click quando lo stato resta stabile per 30 ms
  static bool premuto = false, ultimaLettura = false;
  static uint32_t cambiato = 0;
  bool lettura = digitalRead(PIN_SW) == LOW;
  if (lettura != ultimaLettura) { ultimaLettura = lettura; cambiato = millis(); }
  if (lettura != premuto && millis() - cambiato > 30) {
    premuto = lettura;
    if (premuto) {
      funzioneSuccessiva(false);
      mostraLed();
      if (paginaCollegata) inviaEvento(0);
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

// ---------- Registrazione nella flash ----------
// File: 2 byte con il numero di frame, 2 byte con l'orientamento degli schermi,
// poi per ogni frame 2 byte di durata + 2048 byte di immagine
const char *FILE_REG = "/registrazione2.bin";
const uint8_t BYTE_INTESTAZIONE = 4;
const uint16_t MAX_FRAME_REG = 300;
const uint16_t BYTE_FRAME_REG = 2 + 2 * BYTE_SCHERMO;
bool fsPronto = false;

// Ricezione di una registrazione dalla pagina
bool inRegistrazione = false;
File fileReg;
uint16_t frameAttesi = 0, frameSalvati = 0, byteNelFrame = 0;
uint8_t bufReg[BYTE_FRAME_REG];

void errore(const char *motivo) {
  Serial.print("S errore "); Serial.print(motivo); Serial.print('\n');
}

// Frame della registrazione salvata (0 se non c'è)
uint16_t frameSalvatiInFlash() {
  if (!fsPronto) return 0;
  File f = LittleFS.open(FILE_REG, "r");
  uint16_t n = 0;
  if (f) { if (f.read((uint8_t *)&n, 2) != 2) n = 0; f.close(); }
  return n;
}

void iniziaRicezioneRegistrazione(int n, int rot) {
  if (!fsPronto) { errore("fs"); return; }
  LittleFS.remove(FILE_REG);
  if (n <= 0) { Serial.print("S 0\n"); return; }   // R 0 = cancella
  if (n > MAX_FRAME_REG) { errore("troppi"); return; }
  fileReg = LittleFS.open(FILE_REG, "w");
  if (!fileReg) { errore("file"); return; }
  uint16_t intestazione[2] = {(uint16_t)n, (uint16_t)rot};
  fileReg.write((uint8_t *)intestazione, BYTE_INTESTAZIONE);
  frameAttesi = n; frameSalvati = 0; byteNelFrame = 0;
  ultimoByte = millis();  // il timeout parte da adesso, non dall'ultimo frame ricevuto
  inRegistrazione = true;
}

void riceviRegistrazione() {
  while (inRegistrazione && Serial.available()) {
    int n = Serial.readBytes(bufReg + byteNelFrame, min((int)(BYTE_FRAME_REG - byteNelFrame), Serial.available()));
    byteNelFrame += n;
    ultimoByte = millis();
    if (byteNelFrame == BYTE_FRAME_REG) {
      if (fileReg.write(bufReg, BYTE_FRAME_REG) != BYTE_FRAME_REG) {  // flash piena
        fileReg.close();
        LittleFS.remove(FILE_REG);
        inRegistrazione = false;
        errore("scrittura");
        // i byte rimanenti della registrazione verranno ignorati come testo non valido
        return;
      }
      byteNelFrame = 0;
      if (++frameSalvati == frameAttesi) {
        fileReg.close();
        inRegistrazione = false;
        Serial.print("S "); Serial.print(frameSalvati); Serial.print('\n');
      }
    }
  }
}

void annullaRicezioneRegistrazione() {
  fileReg.close();
  LittleFS.remove(FILE_REG);
  inRegistrazione = false;
  errore("interrotto");
}

// ---------- Riproduzione autonoma ----------
bool inRiproduzione = false;
File fileRip;
uint16_t frameRip = 0, indiceRip = 0, rotRip = 0;
uint32_t prossimoRip = 0;   // quando finisce il frame in corso
uint32_t ultimoContatto = 0;

// Gli effetti lavorano sulla superficie "logica", cioè come la vedi (64 x 256 se gli schermi sono in verticale)
const int W_FIS = 256, H_FIS = 64;  // superficie fisica: i due schermi affiancati
int LW = W_FIS, LH = H_FIS;
uint8_t frameGrezzo[2 * BYTE_SCHERMO];  // frame come registrato, prima degli effetti
uint8_t logico[W_FIS * H_FIS];                  // 1 byte per pixel, dopo zoom/posizione/inverti

void logicoAFisico(int lx, int ly, int &px, int &py) {
  switch (rotRip) {
    case 90:  px = ly;         py = H_FIS - 1 - lx; break;
    case 180: px = W_FIS - 1 - lx; py = H_FIS - 1 - ly; break;
    case 270: px = W_FIS - 1 - ly; py = lx;         break;
    default:  px = lx;         py = ly;
  }
}

// Formato del buffer SSD1306: 8 pagine da 128 byte, ogni byte = 8 pixel in verticale (bit 0 in alto)
inline bool pixelGrezzo(int px, int py) {
  return frameGrezzo[(px >= 128 ? BYTE_SCHERMO : 0) + (py >> 3) * 128 + (px & 127)] >> (py & 7) & 1;
}

inline void accendiSchermo(int px, int py) {
  uint8_t *buf = px < 128 ? sx.getBufferPtr() : bufferDx;
  buf[(py >> 3) * 128 + (px & 127)] |= 1 << (py & 7);
}

// Divisione arrotondata verso il basso anche per i negativi (evita una colonna doppia al centro)
inline int divGiu(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

// Glitch: spostamenti casuali ma fissi (seme costante), scalati dall'intensità come nella pagina
int8_t fascia[W_FIS];          // per riga: verso e ampiezza dello scorrimento della sua fascia
int8_t glitchVx[W_FIS * H_FIS], glitchVy[W_FIS * H_FIS];
int campoPerLW = 0;

void preparaGlitch() {
  if (campoPerLW == LW) return;
  campoPerLW = LW;
  uint32_t seme = 1234;
  auto casuale = [&seme]() { seme ^= seme << 13; seme ^= seme >> 17; seme ^= seme << 5; return seme; };
  for (int y = 0; y < LH;) {
    int alt = 1 + casuale() % 8;
    int8_t off = (casuale() % 2) ? (int8_t)((int)(casuale() % 255) - 127) : 0;
    for (int k = 0; k < alt && y < LH; k++, y++) fascia[y] = off;
  }
  for (int p = 0; p < LW * LH; p++) {
    glitchVx[p] = (int)(casuale() % 255) - 127;
    glitchVy[p] = (int)(casuale() % 255) - 127;
  }
}

// Applica zoom, posizione, inverti e glitch al frame grezzo e lo mette nei buffer degli schermi
void elaboraFrame() {
  static int16_t srcX[W_FIS], srcY[W_FIS];
  int cx = LW / 2, cy = LH / 2;
  // Zoom e posizione: per ogni pixel mostrato, quale pixel della registrazione va preso
  for (int lx = 0; lx < LW; lx++) srcX[lx] = cx + divGiu((lx - cx - spostaX) * 100, zoomPct);
  for (int ly = 0; ly < LH; ly++) srcY[ly] = cy + divGiu((ly - cy) * 100, zoomPct);
  for (int ly = 0; ly < LH; ly++) {
    for (int lx = 0; lx < LW; lx++) {
      int sxL = srcX[lx], syL = srcY[ly];
      bool acceso = false;
      if (sxL >= 0 && sxL < LW && syL >= 0 && syL < LH) {
        int px, py;
        logicoAFisico(sxL, syL, px, py);
        acceso = pixelGrezzo(px, py);
      }
      logico[ly * LW + lx] = acceso != invertiLocale;
    }
  }

  memset(sx.getBufferPtr(), 0, BYTE_SCHERMO);
  memset(bufferDx, 0, BYTE_SCHERMO);
  int32_t g = glitchPct;
  int16_t spostaRiga[W_FIS];
  int16_t sparso[256];  // spostamento per ogni valore di direzione (-127..127)
  if (g) {
    preparaGlitch();
    // Fasce: fino a metà larghezza; dispersione: fino a 24 px, cresce con g² come nella pagina
    for (int y = 0; y < LH; y++) spostaRiga[y] = fascia[y] * g * (LW / 2) / (127 * 100);
    for (int v = -127; v <= 127; v++) sparso[v + 128] = v * g * g * 24 / (127 * 10000);
  }
  for (int ly = 0; ly < LH; ly++) {
    for (int lx = 0; lx < LW; lx++) {
      int p = ly * LW + lx;
      if (!logico[p]) continue;
      int nx = lx, ny = ly;
      if (g) {
        nx += spostaRiga[ly] + sparso[glitchVx[p] + 128];
        ny += sparso[glitchVy[p] + 128];
        if (ny < 0 || ny >= LH) continue;
        nx = ((nx % LW) + LW) % LW;  // in orizzontale rientra dall'altro lato
      }
      int px, py;
      logicoAFisico(nx, ny, px, py);
      accendiSchermo(px, py);
    }
  }
  sx.sendBuffer();
  dx.sendBuffer();
}

void avviaRiproduzione() {
  static uint32_t ultimoTentativo = 0;  // senza registrazione riprova solo una volta al secondo
  if (!fsPronto || millis() - ultimoTentativo < 1000) return;
  ultimoTentativo = millis();
  fileRip = LittleFS.open(FILE_REG, "r");
  if (!fileRip) return;
  uint16_t intestazione[2];
  if (fileRip.read((uint8_t *)intestazione, BYTE_INTESTAZIONE) != BYTE_INTESTAZIONE || intestazione[0] == 0) {
    fileRip.close();
    return;
  }
  frameRip = intestazione[0];
  rotRip = intestazione[1];
  LW = (rotRip == 90 || rotRip == 270) ? H_FIS : W_FIS;
  LH = (rotRip == 90 || rotRip == 270) ? W_FIS : H_FIS;
  indiceRip = 0;
  prossimoRip = millis();
  inRiproduzione = true;
  ridisegna = false;
  // Se l'encoder era su una funzione che senza pagina non funziona, passa alla prima utilizzabile
  funzioneSuccessiva(true);
  mostraLed();
}

void fermaRiproduzione() {
  if (inRiproduzione) fileRip.close();
  inRiproduzione = false;
}

void passoRiproduzione() {
  uint32_t ora = millis();
  if ((int32_t)(ora - prossimoRip) < 0) {           // il frame attuale non è ancora finito
    if (ridisegna) { ridisegna = false; elaboraFrame(); }  // ma un effetto è cambiato
    return;
  }
  if (ora - prossimoRip > 1000) prossimoRip = ora;  // molto in ritardo: riparte da qui
  // Salta i frame che sarebbero già finiti, così la durata totale resta quella registrata
  uint32_t durata = 0;
  for (uint16_t tentativi = 0; tentativi <= frameRip; tentativi++) {
    if (indiceRip == frameRip) { fileRip.seek(BYTE_INTESTAZIONE); indiceRip = 0; }  // ricomincia il loop
    uint16_t registrata;
    if (fileRip.read((uint8_t *)&registrata, 2) != 2) { fermaRiproduzione(); return; }
    indiceRip++;
    durata = (uint32_t)registrata * 100 / VELOCITA_PCT[velocitaIdx];
    if (durata == 0) durata = 1;
    if ((int32_t)(ora - (prossimoRip + durata)) < 0) break;  // questo frame è ancora in corso
    prossimoRip += durata;
    fileRip.seek(fileRip.position() + 2 * BYTE_SCHERMO);
  }
  fileRip.read(frameGrezzo, 2 * BYTE_SCHERMO);
  ridisegna = false;
  elaboraFrame();
  prossimoRip += durata;
}

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
  } else if (cmd[0] == 'M') {
    // La pagina ha selezionato una funzione cliccando il suo riquadro
    int m;
    if (sscanf(cmd + 1, "%d", &m) == 1 && m >= 0 && m < N_MODI) {
      modo = m;
      mostraLed();
      inviaEvento(0);
    }
  } else if (cmd[0] == 'Q') {
    inviaEvento(0);  // la pagina chiede quale funzione è attiva
    Serial.print("I fs "); Serial.print(fsPronto ? 1 : 0);
    Serial.print(' '); Serial.print(frameSalvatiInFlash()); Serial.print('\n');
  } else if (cmd[0] == 'R') {
    int n, rot = 0;
    if (sscanf(cmd + 1, "%d %d", &n, &rot) >= 1) iniziaRicezioneRegistrazione(n, rot);
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

  fsPronto = LittleFS.begin();  // false se in Flash Size non c'è spazio per i file
  if (fsPronto) LittleFS.remove("/registrazione.bin");  // formato vecchio, senza orientamento

  pinMode(PIN_CLK, INPUT_PULLUP);
  pinMode(PIN_DT, INPUT_PULLUP);
  pinMode(PIN_SW, INPUT_PULLUP);
  statoAB = (digitalRead(PIN_CLK) << 1) | digitalRead(PIN_DT);
  attachInterrupt(digitalPinToInterrupt(PIN_CLK), encoderISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_DT), encoderISR, CHANGE);
}

void loop() {
  gestisciEncoder();
  aggiornaLampeggio();

  // Frame o registrazione interrotti (es. pagina chiusa a metà invio): li scarta
  if (inFrame && millis() - ultimoByte > 200) inFrame = false;
  if (inRegistrazione && millis() - ultimoByte > 2000) annullaRicezioneRegistrazione();

  // La pagina è collegata se la porta è aperta (DTR) o se ha mandato qualcosa da poco.
  // Altrimenti, dopo un attimo, parte la registrazione salvata
  if (Serial.available() || Serial) ultimoContatto = millis();
  paginaCollegata = millis() - ultimoContatto < 1500;
  if (paginaCollegata) fermaRiproduzione();
  else if (!inRiproduzione) avviaRiproduzione();
  if (inRiproduzione) { passoRiproduzione(); return; }

  // Legge tutti i comandi arrivati...
  while (Serial.available() && !frameCompleto) {
    if (inFrame) { riceviFrame(); continue; }
    if (inRegistrazione) { riceviRegistrazione(); continue; }
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
