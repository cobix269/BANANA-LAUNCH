 
BANANA LAUNCH

Résumé : Implémentation d’un jeu à 2 joueurs sur des XIAO esp32S3 et communication I2C. 

Règles du jeu : 

Dans ce jeu il y a deux joueurs : l’émetteur et le récepteur. L’émetteur envoie des bananes (en appuyant sur A) au récepteur sans savoir où celui-ci à placer son panier pour rattraper les bananes et inversement le récepteur tente de récupérer les bananes (avec les touches gauche et droite) sans savoir d’où elles vont tomber. Le récepteur a pour objectif de récupérer au minimum 16 bananes sur les 20 que peut envoyer l’émetteur. Les deux joueurs ont chacun un super pouvoir, en appuyant sur B l’émetteur peut augmenter pendant 5 secondes la vitesse de tir des bananes et le récepteur peut augmenter la taille de son panier pendant 5 secondes. Le pouvoir met 10 secondes à se recharger. 
 
<img width="454" height="340" alt="image" src="https://github.com/user-attachments/assets/55a73cef-b343-4829-a02f-c2c606e39614" />

Composants : 
Le schéma électrique et les composantes sont disponibles sur le Github à l’adresse suivante IFT_S3


Structure globale du code : 

Communication : 
La communication se fait par I2C avec le TX et le RX de nos ESP32S3. Ils sont reliés avec des câbles. 
Deux programmes Arduino commentés : le **lanceur**, puis le **rattrapeur**.


## Matériel et branchements

Sur chacune des deux cartes : XIAO ESP32-S3, OLED SSD1306 I2C 128 × 64 et clavier matriciel 3 × 3.

| Élément | Broches / réglage |
| --- | --- |
| OLED SDA / SCL | D4 / D5 |
| Adresse OLED | `0x3C` |
| Lignes du clavier | D8, D9, D10 |
| Colonnes du clavier | D1, D6, D7 |
| UART RX / TX | D0 / D2, 9600 bauds |
| Liaison carte 1 → carte 2 | D2 → D0 |
| Liaison carte 2 → carte 1 | D2 → D0 |
| Masse commune | GND ↔ GND |


## Installation

Dans Arduino IDE, sélectionner **XIAO_ESP32S3** du paquet de cartes **esp32**.
Installer **Adafruit SSD1306** et **Adafruit GFX Library**.

## Messages échangés

Les messages sont des **octets binaires**

| Message | Sens | Données après l'en-tête |
| --- | --- | --- |
| `0xF1` | Lanceur → rattrapeur | Centre horizontal x, puis 0 = normal / 1 = super |
| `0xF3` | Rattrapeur → lanceur | Aucune : nouvelle partie |
| `0xF4` | Rattrapeur → lanceur | Score, puis nombre de bananes terminées |
| `0xF5` | Rattrapeur → lanceur | 1 = lanceur gagnant, 0 = lanceur perdant |

Les données sont inférieures à `0xF0`, réservé au repérage des en-têtes. Modifier ce format exige de modifier les deux programmes ensemble.
La liaison actuelle ne comporte ni accusé de réception, ni contrôle d'intégrité, ni renvoi automatique : un message perdu peut désynchroniser la partie.


### Le trajet d'une banane

1. Le rattrapeur reçoit A : `startGame()` remet les compteurs à zéro et envoie `MSG_START` au lanceur.
2. Les deux cartes passent par `COUNTDOWN`, puis `PLAYING` après trois secondes.
3. A sur le lanceur appelle `launchBanana(false)` ; B appelle `launchBanana(true)`.
4. `updateGame()` fait descendre la banane en ajoutant sa vitesse à `y` à chaque image.
5. Quand `y >= 64`, le lanceur envoie sa position x et son type. Il désactive sa copie locale.
6. `readUart()` du rattrapeur reconstitue le message et appelle `spawnBanana()`.
7. Le rattrapeur teste le passage au niveau du panier. Une prise augmente `score` ; prise ou ratée augmente `resolved`.
8. À `resolved == 20`, il compare `score` à `WIN_SCORE`, puis envoie le résultat au lanceur.

### Les variables à distinguer

- `stock` : bananes pas encore lancées, côté lanceur.
- `received` : bananes entrées côté rattrapeur.
- `resolved` : bananes déjà attrapées ou ratées.
- `score` : bananes attrapées uniquement.

Exemple : stock = 0, received = 18 et resolved = 17 ne signifie pas que la partie est terminée : deux bananes sont encore chez le lanceur et une chez le rattrapeur.

### Les fonctions importantes

| Fonction | Responsabilité |
| --- | --- |
| `setup()` | Préparer matériel et communications une fois au démarrage |
| `loop()` | Répéter réception, clavier, logique et affichage |
| `scanButtons()` / `pressed()` | Lire les boutons et repérer un nouvel appui |
| `startGame()` | Remettre la partie à zéro |
| `readUart()` | Recevoir et interpréter les octets |
| `launchBanana()` | Créer un tir côté lanceur |
| `spawnBanana()` | Créer la copie reçue côté rattrapeur |
| `updateGame()` | Déplacer et appliquer les règles |
| `activatePower()` / `updatePower()` | Déclencher et arrêter le grand panier |
| `resolveBanana()` | Compter une prise ou un raté, vérifier la fin |
| `render()` | Dessiner, sans décider qui gagne |

### Temps, positions et syntaxe

- `millis()` donne le temps écoulé depuis le démarrage, en millisecondes. `now - powerStart` donne le temps écoulé depuis l'activation du pouvoir.
- `FRAME_MS = 33` limite les mises à jour à environ 30 images/s. Les vitesses sont exprimées en **pixels par image** : changer la durée d'une image change aussi la vitesse réelle du jeu.
- L'écran commence en `(0,0)` en haut à gauche. x augmente vers la droite ; y augmente vers le bas. `constrain()` empêche une position de dépasser ses limites.
- `if` teste une condition ; `return` quitte la fonction actuelle ; `for` répète un traitement.
- `bool` vaut vrai/faux ; `int` contient un entier ; `float` autorise les décimales ; `uint8_t` représente un octet ; `unsigned long` sert ici aux temps.
- `=` affecte une valeur ; `==` compare ; `&&` signifie « et » ; `||` signifie « ou » ; `!` signifie « non ».
- `score++` ajoute 1 ; `stock--` retire 1 ; `b.y += b.speed` ajoute la vitesse à y.
- `for (auto &b : bananas)` parcourt toutes les bananes. Le `&` permet de modifier la vraie banane du tableau ; sans lui, on modifierait une copie.
- `special ? SUPER_SPEED : NORMAL_SPEED` choisit la vitesse super si special est vrai, sinon la vitesse normale.

### Affichage

L'OLED fait 128 × 64 pixels. Avec la police standard, un caractère à taille 1 occupe environ 6 × 8 pixels, soit 21 caractères maximum par ligne sans marge ; à taille 2, environ 10 caractères.
`setTextWrap(false)` interdit le retour automatique, mais ne réduit pas un texte trop long : celui-ci serait coupé.
Le bandeau occupe les lignes y=0 à 18. Garder les messages de statut dans ce bandeau pour ne pas recouvrir le panier. Les caractères accentués ne sont pas gérés comme du texte Unicode ordinaire par cette police.
`clearDisplay()` efface le tampon en mémoire ; les dessins le remplissent ; `display.display()` actualise l'écran physique.
