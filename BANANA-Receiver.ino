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

// ===== REGLAGES =====
// Constantes de reglage : TOTAL est le nombre de tirs par partie.
// MAX_BANANAS est la capacite du tableau, pas le nombre deja lance.
const int TOTAL = 20;
const int MAX_BANANAS = 20;
// Seuil de victoire du RATTRAPEUR : 16 inclus. Decision apres 20 resolutions.
const int WIN_SCORE = 16;

// Largeurs en pixels : pouvoir modere (+50 %, 16 -> 24).
const int BASKET_NORMAL = 16;
const int BASKET_BIG = 24;
const int BASKET_Y = 55;

const float NORMAL_SPEED = 1.5f;
const float SUPER_SPEED = 4.5f;

const unsigned long FRAME_MS = 33;
// Durees en millisecondes : effet 5 s ; recharge 10 s depuis ACTIVATION.
// Donc, apres la fin de l effet, il reste normalement 5 s de recharge.
const unsigned long POWER_DURATION = 5000;
const unsigned long COOLDOWN = 10000;

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

// basketX est le CENTRE du panier ; basketWidth est sa largeur courante.
float basketX = 64;
int basketWidth = BASKET_NORMAL;

int score = 0;

// score = attrapees ; resolved = attrapees + ratees ; received = entrees recues.
// received peut etre plus grand que resolved si des bananes sont encore en vol.
int resolved = 0;
int received = 0;

bool catcherWon = false;
bool powerUsed = false;
bool powerActive = false;

unsigned long powerStart = 0;
unsigned long lastFrame = 0;
unsigned long phaseStart = 0;

// ===== UART =====
const uint8_t MSG_BANANA = 0xF1;
const uint8_t MSG_START = 0xF3;
const uint8_t MSG_RESULT = 0xF4;
const uint8_t MSG_END = 0xF5;

// Reception UART conservee entre les tours de loop : les octets
// d un meme message ne sont pas forcement tous arrives en meme temps.
uint8_t rxType = 0;
uint8_t rxData[2];
uint8_t rxCount = 0;

// ===== COMMANDES RESEAU =====
// Retour vers le lanceur : F4, score, nombre de bananes terminees.
// Ce deuxieme compteur ne represente plus des points de vie.
void sendResult() {
  Serial1.write(MSG_RESULT);
  Serial1.write((uint8_t)score);
  Serial1.write((uint8_t)resolved);
}

// Reinitialise les compteurs, les pouvoirs et les bananes de la partie precedente.
void startGame() {
  score = 0;

  resolved = 0;
  received = 0;

  basketX = 64;
  basketWidth = BASKET_NORMAL;

  powerUsed = false;
  powerActive = false;
  powerStart = 0;

  for (auto &b : bananas) b.active = false;

  phase = COUNTDOWN;
  phaseStart = millis();

  // Demarre aussi le lanceur
  Serial1.write(MSG_START);
  sendResult();
}

// Le rattrapeur decide du resultat. F5 transmet la victoire du LANCEUR :
// si won vaut true ici, on envoie donc 0 au lanceur.
void endGame(bool won) {
  catcherWon = won;
  phase = END;
  phaseStart = millis();

  for (auto &b : bananas) b.active = false;

  // Le lanceur gagne quand le rattrapeur perd
  Serial1.write(MSG_END);
  Serial1.write((uint8_t)(won ? 0 : 1));
}

// ===== RECEPTION BANANES =====
// Recoit centre x et type du lancer ; cree une banane au-dessus de la zone
// de jeu visible, derriere le bandeau. Ignore les tirs hors jeu ou au-dela de 20.
void spawnBanana(uint8_t x, bool special) {
  if (phase != PLAYING || received >= TOTAL) return;

  for (auto &b : bananas) {
    if (!b.active) {
      b.active = true;
      b.x = constrain((float)x - 4, 0.0f, 120.0f);
      b.y = 11.0f;  // Entre depuis le haut
      b.speed = special ? SUPER_SPEED : NORMAL_SPEED;
      b.special = special;
      received++;
      return;
    }
  }
}

// Traite seulement les octets deja arrives, sans attendre.
// 0xF0 et plus signalent un entete ; les donnees normales sont inferieures.
// Les deux sketches doivent partager le meme format de messages.
void readUart() {
  while (Serial1.available()) {
    uint8_t c = Serial1.read();

    if (c >= 0xF0) {
      rxType = c;
      rxCount = 0;
      continue;
    }

    if (rxType == MSG_BANANA) {
      rxData[rxCount++] = c;

      if (rxCount == 2) {
        spawnBanana(rxData[0], rxData[1] == 1);
        rxType = 0;
        rxCount = 0;
      }
    } else {
      rxType = 0;
    }
  }
}

// ===== SUPER POUVOIR =====
// B agrandit le panier seulement si sa recharge est terminee.
void activatePower(unsigned long now) {
  if (phase != PLAYING) return;

  if (powerUsed && now - powerStart < COOLDOWN)
    return;

  powerUsed = true;
  powerActive = true;
  powerStart = now;
  basketWidth = BASKET_BIG;
}

// Rend la largeur normale apres 5 secondes, sans bloquer clavier ni animation.
void updatePower(unsigned long now) {
  if (powerActive &&
      now - powerStart >= POWER_DURATION) {
    powerActive = false;
    basketWidth = BASKET_NORMAL;
  }
}

// ===== SCORE =====
// Appelee exactement une fois par banane terminee. Toujours incrementer
// resolved, incrementer score seulement si attrapee. Une ratee n enleve aucune vie.
bool resolveBanana(bool caught) {
  resolved++;

  if (caught) score++;
  sendResult();
  if (resolved >= TOTAL) {
    endGame(score >= WIN_SCORE);
    return true;
  }

  return false;
}

// ===== JEU =====
// Actualise pouvoir et panier, puis teste la trajectoire de chaque banane.
void updateGame(unsigned long now) {
  if (pressed(B)) activatePower(now);

  updatePower(now);

  if (keys[LEFT]) basketX -= 3;
  if (keys[RIGHT]) basketX += 3;

  // basketX est le centre du panier
  basketX = constrain(
    basketX,
    (float)basketWidth / 2,
    (float)W - basketWidth / 2
  );

  float left = basketX - basketWidth / 2.0f;
  float right = basketX + basketWidth / 2.0f;

  for (auto &b : bananas) {
    if (!b.active) continue;

    float oldY = b.y;
    b.y += b.speed;

    // Collision au niveau du haut du panier
    const float catchY = BASKET_Y - 8;

    // Tester le franchissement (ancienne ET nouvelle position) evite qu une
    // banane rapide saute le niveau de collision entre deux images.
    bool crossed = oldY < catchY && b.y >= catchY;
    float center = b.x + 4;

    if (crossed) {
      // Comparaison entre centre de la banane et bords du panier, tolerance 1 px.
      // C est une regle de collision simplifiee, pas un test pixel par pixel.
      bool caught =
        center >= left - 1 &&
        center <= right + 1;

      if (caught) {
        b.active = false;
        if (resolveBanana(true)) return;
      }
    }

    // Ratee si elle atteint le sol
    if (b.active && b.y >= H - 8) {
      b.active = false;
      if (resolveBanana(false)) return;
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
    centered("RATTRAPEUR", 23, 1);
    centered("Objectif : 16/20", 36, 1);
    centered("A : JOUER", 53, 1);
  } else if (phase == END) {
    centered(catcherWon ? "GAGNE !" : "PERDU !", 3, 2);
    char line[22];
    snprintf(line, sizeof(line), "Pris : %d/20", score);
    centered(line, 29, 1);
    centered("Objectif : 16/20", 41, 1);
    centered("A : REJOUER", 54, 1);
  } else {
    for (const auto &b : bananas)
      if (b.active) display.drawBitmap((int)b.x, (int)b.y, bananaBmp, 8, 8, SSD1306_WHITE);
    int left = (int)(basketX - basketWidth / 2.0f);
    display.drawRect(left, BASKET_Y, basketWidth, 8, SSD1306_WHITE);
    display.drawFastHLine(left + 2, BASKET_Y + 3, basketWidth - 4, SSD1306_WHITE);
    // Bandeau reserve : aucune banane ni phrase ne recouvre les compteurs.
    display.fillRect(0, 0, W, 19, SSD1306_BLACK);
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.print("Pris:"); display.print(score); display.print("/20");
    display.setCursor(66, 0);
    display.print("Fin:"); display.print(resolved);
    display.setCursor(0, 9);
    if (powerActive) {
      display.print("B:GRAND ");
      display.print((POWER_DURATION - (now - powerStart) + 999) / 1000);
      display.print("s");
    } else if (!powerUsed || now - powerStart >= COOLDOWN) display.print("B:PRET");
    else {
      display.print("B:");
      display.print((COOLDOWN - (now - powerStart) + 999) / 1000);
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
// Arduino repete loop en permanence. Lire UART a chaque tour, puis limiter
// clavier, mouvement et affichage a environ 30 images/s (33 ms minimum).
void loop() {
  readUart();

  unsigned long now = millis();
  // return quitte ce tour de loop, sans arreter le programme.
  // La soustraction de temps unsigned reste adaptee au debordement de millis().
  if (now - lastFrame < FRAME_MS) return;
  lastFrame = now;

  scanButtons();

  if ((phase == TITLE || phase == END) &&
      pressed(A)) {
    startGame();
  }

  if (phase == COUNTDOWN &&
      now - phaseStart >= 3000) {
    phase = PLAYING;
  }

  if (phase == PLAYING) {
    updateGame(now);
  }

  render();
}
```
