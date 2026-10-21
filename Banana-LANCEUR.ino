// Arduino : broches, temps, port serie. Wire : bus I2C de l OLED.
// GFX fournit les dessins ; SSD1306 pilote cet ecran particulier.
#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ===== ECRAN =====
#define OLED_ADDR 0x3C
#define W 128
#define H 64

Adafruit_SSD1306 display(W, H, &Wire, -1);

// ===== BOUTONS =====
// Chaque nom correspond a un indice du tableau keys : NONE=0, LEFT=1, etc.
enum Action { NONE, LEFT, RIGHT, A, B };

// Matrice 3 x 3 : activer une ligne puis lire les trois colonnes.
// Conserver ces broches si le cablage physique reste identique.
const uint8_t rows[] = {D8, D9, D10};
const uint8_t cols[] = {D1, D6, D7};

const Action keymap[3][3] = {
  {RIGHT, A, NONE},
  {NONE, NONE, NONE},
  {NONE, B, LEFT}
};

// keys = boutons actuellement appuyes ; oldKeys = image precedente.
bool keys[5] = {};
bool oldKeys[5] = {};

// Lecture du clavier une fois par image. Une colonne LOW indique un appui
// sur la ligne active. Les autres lignes restent en entree pour ne pas les opposer.
void scanButtons() {
  for (int i = 0; i < 5; i++) {
    oldKeys[i] = keys[i];
    keys[i] = false;
  }

  for (int r = 0; r < 3; r++) {
    pinMode(rows[r], OUTPUT);
    digitalWrite(rows[r], LOW);
    delayMicroseconds(20);

    for (int c = 0; c < 3; c++) {
      if (digitalRead(cols[c]) == LOW)
        keys[keymap[r][c]] = true;
    }

    pinMode(rows[r], INPUT);
  }
}

bool pressed(Action a);
// Vrai uniquement au passage relache -> appuye : maintenir A ne repete pas les tirs.
// Pour un mouvement continu, on utilise keys[LEFT] ou keys[RIGHT].
bool pressed(Action a) {
  return keys[a] && !oldKeys[a];
}

// ===== JEU =====
// Constantes de reglage : TOTAL est le nombre de tirs par partie.
// MAX_BANANAS est la capacite du tableau, pas le nombre deja lance.
const int TOTAL = 20;
const int MAX_BANANAS = 20;
const float NORMAL_SPEED = 1.5f;
const float SUPER_SPEED = 4.5f;
const unsigned long FRAME_MS = 33;
const unsigned long COOLDOWN = 10000;
const unsigned long SHOT_DELAY = 250;

// Machine a etats : ecran titre -> compte a rebours -> jeu -> resultat.
enum Phase { TITLE, COUNTDOWN, PLAYING, END };
Phase phase = TITLE;

// Une structure regroupe les informations d une banane.
// x/y = coin superieur gauche en pixels ; speed = pixels par image.
// active=false libere la case ; special indique un tir avec B.
struct Banana {
  bool active;
  float x, y, speed;
  bool special;
};

// Tableau de bananes : plusieurs peuvent etre en vol en meme temps.
Banana bananas[MAX_BANANAS] = {};

// aimX = centre horizontal de la visee. stock = bananes non encore lancees.
// score = attrapees chez le rattrapeur ; resolved = attrapees + ratees chez lui.
float aimX = 64;
int stock = TOTAL;
int score = 0;
int resolved = 0;
bool launcherWon = false;

bool superUsed = false;
unsigned long lastSuper = 0;
unsigned long lastShot = 0;
unsigned long phaseStart = 0;
unsigned long lastFrame = 0;

// Reception UART conservée entre les tours de loop : les octets
// d'un m^me message ne sont pas forcement tous arrivés en même temps.
uint8_t rxType = 0;
uint8_t rxData[2];
uint8_t rxCount = 0;

// ===== PROTOCOLE UART =====
// F1, x, super -> banane entrant sur ecran 2
// F3 -> debut de partie (recu)
// F4, score, resolues -> resultat (recu)
// F5, victoireLanceur -> fin (recu)

const uint8_t MSG_BANANA = 0xF1;
const uint8_t MSG_START = 0xF3;
const uint8_t MSG_RESULT = 0xF4;
const uint8_t MSG_END = 0xF5;

// Réinitialise les compteurs, les pouvoirs et les bananes de la partie précedente.
void startGame() {
  stock = TOTAL;
  score = 0;
  resolved = 0;

  aimX = 64;
  superUsed = false;
  lastSuper = 0;
  lastShot = 0;

  for (auto &b : bananas) b.active = false;

  phase = COUNTDOWN;
  phaseStart = millis();
}

// Traite seulement les octets deja arrivés, sans attendre.
// 0xF0 et plus signalent un entete ; les donnees normales sont inferieures.
// Les deux sketches doivent partager le meme format de messages.
void readUart() {
  while (Serial1.available()) {
    uint8_t c = Serial1.read();

    if (c >= 0xF0) {
      rxType = c;
      rxCount = 0;

      if (c == MSG_START) {
        startGame();
        rxType = 0;
      }
      continue;
    }

    if (rxType == MSG_RESULT) {
      rxData[rxCount++] = c;
      if (rxCount == 2) {
        score = rxData[0];
        resolved = rxData[1];
        rxType = 0;
      }
    }
    else if (rxType == MSG_END) {
      launcherWon = (c == 1);
      phase = END;
      phaseStart = millis();
      for (auto &b : bananas) b.active = false;
      rxType = 0;
    }
    else {
      rxType = 0;
    }
  }
}

// ===== LANCER UNE BANANE =====
// A appelle cette fonction avec false ; B avec true. Verifie phase, stock,
// delai et place libre AVANT de consommer une banane et eventuellement le pouvoir.
void launchBanana(bool special) {
  if (phase != PLAYING || stock == 0) return;

  unsigned long now = millis();

  if (!special && lastShot != 0 &&
      now - lastShot < SHOT_DELAY) return;

  if (special && superUsed &&
      now - lastSuper < COOLDOWN) return;

  int slot = -1;
  for (int i = 0; i < MAX_BANANAS; i++) {
    if (!bananas[i].active) {
      slot = i;
      break;
    }
  }
  if (slot < 0) return;

  if (special) {
    superUsed = true;
    lastSuper = now;
  }

  lastShot = now;
  stock--;

  // Cree la banane sur cet ecran. Le -4 convertit le centre en bord gauche
  // car le dessin fait 8 pixels. Le ?: choisit vitesse super ou normale.
  bananas[slot] = {
    true,
    constrain(aimX - 4, 0.0f, 120.0f),
    23.0f,
    special ? SUPER_SPEED : NORMAL_SPEED,
    special
  };
}

// ===== ANIMATION ET TRANSFERT =====
// Deplace la visee, gere les nouveaux appuis, puis anime chaque banane.
// UART n envoie la banane que lorsqu elle quitte completement cet ecran.
void updateGame() {
  if (keys[LEFT]) aimX -= 3;
  if (keys[RIGHT]) aimX += 3;
  aimX = constrain(aimX, 4.0f, 123.0f);

  if (pressed(A)) launchBanana(false);
  if (pressed(B)) launchBanana(true);

  for (auto &b : bananas) {
    if (!b.active) continue;

    b.y += b.speed;

    // La banane a completement quitte l'ecran :
    // elle est maintenant transmise au recepteur.
    // H=64 : sortie par le bas. Message F1 + centre x + indicateur special.
    // Le recepteur recree alors sa propre banane ; aucune image n est envoyee.
    if (b.y >= H) {
      Serial1.write(MSG_BANANA);
      Serial1.write((uint8_t)(b.x + 4));
      Serial1.write((uint8_t)(b.special ? 1 : 0));
      b.active = false;
    }
  }
}

// ===== DESSIN =====
// Dessin monochrome 8 x 8 : chaque octet encode une ligne de huit pixels.
const uint8_t PROGMEM bananaBmp[] = {
  0x06, 0x03, 0x03, 0x07,
  0x0E, 0x3C, 0xF8, 0x70
};

// Petit outil de dessin conserve dans cette version ; render utilise centered.
void drawText(const char *s, int x, int y, int size) {
  display.setTextSize(size);
  display.setCursor(x, y);
  display.print(s);
}

// Texte centre, sans retour automatique : 21 caracteres maximum a taille 1.
// Mesure le texte pour calculer son x central. y reste choisi manuellement.
void centered(const char *text, int y, int size) {
  int16_t x1, y1; uint16_t w, h;
  display.setTextSize(size);
  display.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
  display.setCursor((W - (int)w) / 2, y);
  display.print(text);
}

// Efface le tampon, dessine l etat courant, puis envoie le tampon a l OLED.
// Le bandeau occupe y=0 a 18 ; la zone de jeu commence a y=19.
// Dessiner le bandeau apres les bananes masque leur passage derriere les textes.
void render() {
  display.clearDisplay();
  display.setTextWrap(false);
  display.setTextColor(SSD1306_WHITE);
  unsigned long now = millis();
  if (phase == TITLE) {
    centered("BANANA 2.0", 2, 2);
    centered("LANCEUR", 23, 1);
    centered("Objectif : 16/20", 36, 1);
    centered("Attente rattrapeur", 53, 1);
  } else if (phase == END) {
    centered(launcherWon ? "GAGNE !" : "PERDU !", 3, 2);
    char line[22];
    snprintf(line, sizeof(line), "Pris : %d/20", score);
    centered(line, 29, 1);
    centered("Objectif : 16/20", 41, 1);
    centered("Attente rattrapeur", 54, 1);
  } else {
    for (const auto &b : bananas)
      if (b.active) display.drawBitmap((int)b.x, (int)b.y, bananaBmp, 8, 8, SSD1306_WHITE);
    int x = (int)aimX;
    display.fillTriangle(x - 4, 20, x + 4, 20, x, 24, SSD1306_WHITE);
    // Bandeau reserve : aucune banane ni phrase ne recouvre les compteurs.
    display.fillRect(0, 0, W, 19, SSD1306_BLACK);
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.print("Pris:"); display.print(score); display.print("/20");
    display.setCursor(66, 0);
    display.print("Stock:"); display.print(stock);
    display.setCursor(0, 9);
    if (!superUsed || now - lastSuper >= COOLDOWN) display.print("B:PRET");
    else {
      display.print("B:");
      display.print((COOLDOWN - (now - lastSuper) + 999) / 1000);
      display.print("s");
    }
    display.setCursor(78, 9); display.print("But:16");
    display.drawFastHLine(0, 18, W, SSD1306_WHITE);
    if (phase == COUNTDOWN) {
      int n = 3 - (now - phaseStart) / 1000;
      display.fillRect(46, 25, 36, 29, SSD1306_BLACK);
      display.setTextSize(3); display.setCursor(55, 28);
      display.print(max(n, 1));
    }
  }
  display.display();
}

// ===== INITIALISATION =====
// Arduino appelle setup une seule fois au demarrage : initialiser le materiel.
void setup() {
  // Serial = USB pour les messages de diagnostic ; Serial1 = liaison entre cartes.
  Serial.begin(115200);

  // RX = D0, TX = D2
  // UART 9600 bauds, 8 bits, sans parite, 1 stop. RX=D0, TX=D2.
  // Relier TX de chaque carte au RX de l autre et relier les masses GND.
  Serial1.begin(9600, SERIAL_8N1, D0, D2);

  for (int r = 0; r < 3; r++)
    pinMode(rows[r], INPUT);

  for (int c = 0; c < 3; c++)
    pinMode(cols[c], INPUT_PULLUP);

  // Bus de l ecran : SDA=D4, SCL=D5 ; adresse declaree en haut (0x3C).
  Wire.begin(D4, D5);

  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("Ecran OLED introuvable");
    while (true) delay(1000);
  }
}

// ===== BOUCLE PRINCIPALE =====
// Arduino répète loop en permanence. Lire UART a chaque tour, puis limiter
// clavier, mouvement et affichage a environ 30 images/s (33 ms minimum).
void loop() {
  readUart();

  unsigned long now = millis();
  // return quitte ce tour de loop, sans arreter le programme.
  // La soustraction de temps unsigned reste adaptee au debordement de millis().
  if (now - lastFrame < FRAME_MS) return;
  lastFrame = now;

  scanButtons();

  if (phase == COUNTDOWN &&
      now - phaseStart >= 3000) {
    phase = PLAYING;
  }

  if (phase == PLAYING) {
    updateGame();
  }

  render();
}
```
