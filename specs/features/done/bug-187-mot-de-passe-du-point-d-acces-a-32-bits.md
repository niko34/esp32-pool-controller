# bug-187 — Le mot de passe du point d'accès ne fait que 32 bits, dans un alphabet connu

## Métadonnées
| | |
|---|---|
| Type | bug — 🔴 **sécurité, provisionnement Wi-Fi** |
| Statut | **done** — close le **2026-11-21**. Code livré en **2.95.0**, mesuré sur l'appareil réel le 2026-11-21. Restes portés par bug-186 (2 essais) et bug-200 (2 messages) |
| Priorité | normale — ⚠️ **touche la livraison client**, pas l'appareil de l'utilisateur en service |
| Version cible | **2.95.0** — livrée (commit `486bd85`) |
| Relevé par | `security-reviewer` (P2 de [`bug-181`](../done/bug-181-get-config-expose-la-configuration-sans-authentification.md)), 2026-11-17 — ⭐ **vérifié dans le code** le même jour |
| Décision | l'utilisateur a demandé le 2026-11-17 de **garder le sujet en attente**, plutôt que de l'abandonner |

## Domaines touchés
- [x] **Authentification** (`src/auth.cpp`) · [x] Provisionnement Wi-Fi (`src/main.cpp`)
- [ ] Capteurs — [ ] Régulation — [ ] Filtration — [ ] MQTT — [ ] Frontend

## Agents requis
- [x] **`security-reviewer`** — ⛔ OBLIGATOIRE (c'est l'objet) · [x] architect · [x] embedded-developer · [x] test-engineer · [x] code-reviewer · [x] doc-writer

## Le fait, mesuré dans le code

`src/auth.cpp:108-110` :
```cpp
String apPwd = generateRandomToken().substring(0, 8);
apPwd.toUpperCase();
```
`generateRandomToken()` (`:162-172`) produit **32 caractères hexadécimaux** depuis `esp_random()`
— la source d'aléa est bonne. ⛔ **Mais on n'en garde que 8**, soit **4 octets = 32 bits**.
`toUpperCase()` ne change rien à l'entropie (l'hexadécimal est insensible à la casse).

⇒ **La longueur et l'alphabet sont publics** (ils sont dans ce dépôt) : un attaquant n'a aucune
incertitude à lever. L'espace à parcourir est **exactement** 16⁸ = 2³², hors ligne, après capture
d'une connexion au point d'accès. C'est à portée d'une carte graphique du commerce.

## ⚠️ Ce qui relativise — à ne pas perdre de vue

- Le point d'accès n'est **allumé qu'à l'installation**, ou en repli quand l'appareil perd le
  Wi-Fi. Un appareil en service n'expose rien.
- Depuis `bug-185`, celui qui entre ⛔ **ne peut plus prendre l'appareil** : le droit anonyme se
  limite à choisir un mot de passe **sur un appareil vierge**.
- Ce qu'il peut faire : `/wifi/connect` et `/wifi/disconnect` sont ouverts **sans jeton** quand
  `wifiConfigAllowed()` ⇒ **sortir l'appareil du réseau du client et le rattacher au sien**.

## Comportement attendu
Un mot de passe de point d'accès dont l'espace de recherche met la capture hors de portée.
⚠️ **Contrainte produit** : il doit rester **recopiable à la main** par un installateur — c'est
pourquoi il est aujourd'hui court et en majuscules. ⛔ Arbitrer longueur **contre** lisibilité,
ne pas rallonger sans regarder l'étiquette.

## Lié
- ⚠️ `src/auth.cpp:112-115` journalise ce mot de passe **en clair** à chaque génération, et
  `main.cpp:854` **à chaque démarrage** en mode AP. Volontaire (« seul moyen de le récupérer sans
  écran »), mais à réexaminer dans le même lot.
- [`bug-186`](../todo/bug-186-cinq-essais-de-premier-demarrage-non-prouves-sur-materiel.md) — tout essai
  demande un appareil en état vierge.


---

## ⭐ VÉRIFICATION DE LA FICHE AVANT TRAITEMENT — 2026-11-18

Consigne permanente : *« étudie quand même chaque feature ou bug avant de les traiter parce que
l'application a évolué »*. Relevé réel, sur le code d'aujourd'hui (v2.94.0).

| Affirmation de la fiche | Vérification |
|---|---|
| `auth.cpp:108-110` — `substring(0, 8)` + `toUpperCase()` | ✅ **vrai**, mais aux lignes **148-150** (numéros périmés, [ADR-0073](../../../docs/adr/0073-chiffre-consigne-porte-sa-methode.md)) |
| `generateRandomToken()` rend 32 hexadécimaux depuis `esp_random()` | ✅ vrai, lignes **202-212** ; 16 octets, `%02x` |
| 8 hexadécimaux = **32 bits**, alphabet et longueur publics | ✅ vrai ; `toUpperCase()` n'ajoute rien |
| `/wifi/connect` et `/wifi/disconnect` ouverts sans jeton si `wifiConfigAllowed()` | ✅ vrai (`web_routes_config.cpp:2534` et `:2585`) ; `wifiConfigAllowed()` = mode AP **ou** STA non connecté |
| `main.cpp:854` journalise le mot de passe en clair **à chaque démarrage** en mode AP | ⛔ **FAUX AUJOURD'HUI** — voir ci-dessous |

### ⛔ La fiche est périmée sur la journalisation

`main.cpp:850-858` ne journalise le mot de passe **en clair que si `isFirstBootDetected()`**.
Sinon il écrit `AP Password: [voir etiquette ou GET /auth/ap-password]`. Et le second point de
démarrage de l'AP — le **repli** après 3 échecs de reconnexion (`main.cpp:1755`) — ne le
journalise **pas du tout**.

⇒ Il reste **deux** écritures en clair, toutes deux **avant la livraison au client** : à la
génération (`auth.cpp:151-155`) et au démarrage AP tant que l'assistant n'est pas terminé.

⚠️ **Résidu réel, et c'est lui qu'il faut trancher** : ces lignes partent dans le journal
**persistant** (`/system.log`), que `/download-logs` sert ensuite. Un client qui envoie son
journal au support exporte donc, aussi, le mot de passe de son point d'accès.

### ⭐ Trouvé au passage : `CRITICAL` et `WRITE` sont le MÊME niveau

`/auth/ap-password` est marquée `RouteProtection::CRITICAL`. ⛔ Mais `checkAuth`
(`auth.cpp:327-355`) ne distingue que `NONE` du reste : `in.publicRoute = (level ==
RouteProtection::NONE)`. Il n'existe **qu'une seule preuve**, le jeton.

⇒ L'étiquette `CRITICAL` est **décorative**. Il ne faut donc pas justifier la journalisation en
clair par « de toute façon la route est plus protégée » : elle ne l'est pas. Et inversement, la
journalisation **n'abaisse aucun privilège** — qui a le jeton a déjà les deux.
⇒ Un énumératif à trois niveaux dont deux sont identiques invite à ce raisonnement faux :
**sujet de suite à ouvrir**, hors périmètre de cette fiche.

## ⭐ L'ARBITRAGE CHIFFRÉ

Attaque visée : capture d'une poignée de main WPA2 (ou d'un PMKID, qui n'exige **aucun client**),
puis recherche **hors ligne**. Méthode du chiffre : WPA2-PSK = PBKDF2-HMAC-SHA1, 4096 itérations ;
une carte graphique haut de gamme du commerce fait **≈ 2 millions d'essais/seconde**
(hashcat `-m 22000`), une machine à 8 cartes **≈ 16 millions**. ⚠️ Ces débits **montent** avec le
temps : ce qui tient 2 ans aujourd'hui ne tient pas 10 ans.

| Format | Espace | 1 carte | 8 cartes |
|---|---|---|---|
| **8 hexadécimaux — aujourd'hui** | **2³²** | **36 minutes** | **4,5 minutes** |
| 12 hexadécimaux | 2⁴⁸ | 4,5 ans | 7 mois |
| 10 caractères, alphabet de 32 | 2⁵⁰ | 18 ans | 2,2 ans |
| **12 caractères, alphabet de 32** | **2⁶⁰** | **18 000 ans** | **2 300 ans** |
| 16 hexadécimaux | 2⁶⁴ | 292 000 ans | 36 500 ans |

⚠️ L'hexadécimal coûte cher : 4 bits par caractère contre 5. À longueur égale, un alphabet de 32
symboles rapporte **25 % d'entropie en plus** sans rien changer à la lisibilité — à condition de
retirer les caractères qui se confondent.

**Alphabet proposé** (base 32 dite « Crockford », exactement 32 symboles) :
`0123456789ABCDEFGHJKMNPQRSTVWXYZ` — ⛔ **sans `I`, `L`, `O`, `U`**. Comme `I` et `O` sont
absents, `1` et `0` ne se confondent plus avec rien.

**Format proposé** : `ABCD-EFGH-JKMN`, soit **12 caractères utiles + 2 tirets**. Les tirets font
**partie** du mot de passe : ce qui est lu sur l'étiquette est exactement ce qui se tape, ⛔ sans
avoir à deviner s'il faut les saisir.


---

## ⭐ CONCEPTION RETENUE — décidée par l'utilisateur le 2026-11-18

⚠️ **Cette section REMPLACE une première conception, bien plus compliquée, que l'utilisateur a
écartée** (« c'est trop compliqué tout ça… ce n'est pas un coffre avec des lingots d'or »). Elle
prévoyait de régénérer le mot de passe des appareils déjà installés, donc d'invalider leur
étiquette, donc un bandeau pour prévenir, donc un drapeau persistant pour l'acquitter — et elle
ouvrait la porte à l'enfermement dehors (condition G1 de la revue P1).

⭐ **La conception retenue fait disparaître tout cela** en posant une règle simple :
**l'étiquette ne ment jamais.**

### Les DEUX mots de passe, et qui les choisit

| | Qui le fixe | Modifiable | Après réinitialisation |
|---|---|---|---|
| **De l'appareil** (ouvre l'interface) | **l'utilisateur**, à la première connexion | oui, Paramètres ▸ Sécurité | redevient `admin`, assistant rouvert |
| **Wi-Fi D'ORIGINE** (étiquette) | l'appareil, **une seule fois**, à la mise en service | ⛔ **jamais** | — |
| **Wi-Fi COURANT** (réseau de secours réel) | d'abord celui d'origine | **oui**, Paramètres ▸ Wi-Fi | ⭐ **revient à celui d'origine** |

### Les quatre décisions

1. **Format du mot de passe fabriqué** : 12 caractères, au moins une minuscule, une majuscule,
   un chiffre et un caractère spécial. Alphabet de 72 symboles — la liste de l'utilisateur, moins
   les six caractères qui se confondent à la lecture (`I`, `l`, `1`, `O`, `0`, `o`) et moins `^`,
   **touche morte sur un clavier français** : `^` puis `a` donne `â`, un client saisissant depuis
   un ordinateur échouerait sans comprendre. **≈ 74,0 bits nominaux, 73,5 bits réels** une fois
   retirée la contrainte des quatre familles (chiffres établis en revue P1).

2. ⛔ **AUCUNE régénération automatique.** Un appareil déjà installé garde son mot de passe
   d'origine à 32 bits : son étiquette reste juste. ⭐ Il n'est plus démuni pour autant, puisqu'il
   peut maintenant **en choisir un solide**. ⇒ Les conditions **G1, G2, G3 et A4** de la revue P1
   tombent : il n'y a plus de régénération, donc plus de fenêtre d'enfermement, plus rien à
   annoncer, plus de drapeau à acquitter.

3. **L'utilisateur peut remplacer le mot de passe Wi-Fi courant** depuis Paramètres ▸ Wi-Fi. La
   règle des quatre familles y est **exigée et expliquée**. ⇒ Un gestionnaire de mots de passe
   peut en fabriquer un et le conserver. ⚠️ **Limite à écrire dans l'aide** : aucun gestionnaire
   ne remplit tout seul la fenêtre Wi-Fi d'un téléphone — il le **stocke**, l'utilisateur le tape
   une fois.

4. **La réinitialisation d'usine ramène le mot de passe D'ORIGINE.** C'est ce qui rend
   l'étiquette définitivement vraie, et ce qui ferme la question « comment retrouver l'accès si
   j'ai perdu le mot de passe que j'avais choisi ». ⇒ Deux clés en NVS : l'origine, écrite une
   seule fois et jamais réécrite, et la courante.

### La règle expliquée à l'utilisateur — demande explicite du 2026-11-18

> *« pense à adapter les écrans avec les commentaires pour que l'utilisateur connaisse et
> comprenne cette règle »*

Elle vaut pour **le mot de passe de l'appareil** : les écrans où on le choisit n'annoncent
aujourd'hui que « Minimum 8 caractères » (assistant de première configuration, et
Paramètres ▸ Sécurité — voir la capture `/tmp/ui_securite_390.png`). Ils doivent énoncer la règle
et dire **ce qui manque encore** pendant la frappe.

⚠️ **La règle ne s'applique qu'aux mots de passe CHOISIS À PARTIR DE CE LOT.** Un mot de passe
déjà en place continue de fonctionner : le contrôle est à la saisie, jamais à la connexion — sans
quoi la mise à jour enfermerait dehors l'utilisateur dont le mot de passe ne respecte pas la
règle. ⛔ C'est le point le plus facile à rater de ce lot.


### Le sort de la journalisation en clair — TRANCHÉ

⚠️ **Section rétablie le 2026-11-18** : elle existait dans la première conception, et je l'ai
perdue en la remplaçant par la conception simplifiée. ⛔ C'est `doc-writer` qui l'a vu, en
constatant que l'ADR renvoyait à une décision « écrite dans la fiche » qui n'y était plus.

| Situation | Décision | Pourquoi |
|---|---|---|
| **Fabrication** sur appareil vierge | ⭐ **clair CONSERVÉ** | aucune étiquette n'existe encore et aucun écran n'est joignable : le câble série est le seul chemin. Sans cette ligne, un appareil fraîchement flashé serait **inutilisable** |
| Démarrage du point d'accès **avant** la fin de l'assistant | **clair conservé** | même raison ; comportement déjà en place avant ce lot |
| Démarrage du point d'accès **après** l'assistant | déjà masqué : `[voir etiquette ou GET /auth/ap-password]` | l'étiquette existe, et l'interface est joignable |
| Démarrage de **secours** (perte du Wi-Fi) | ne journalise rien du tout | comportement déjà en place |
| ⭐ **Changement par l'utilisateur** | ⛔ **JAMAIS en clair** | il vient de le taper, il l'a sous les yeux. Le journal dit seulement que la clé a changé |
| ⭐ **Migration de l'origine** (appareil d'avant ce lot) | ⛔ **JAMAIS en clair** | la valeur ne change pas : elle est déjà sur l'étiquette. La réécrire dans le journal serait une divulgation gratuite |

⚠️ **Résidu assumé, écrit noir sur blanc** : sur un appareil vierge, les deux lignes en clair
partent dans le journal **persistant** (`/system.log`), que `/download-logs` sert ensuite. ⇒ Un
client qui envoie son journal au support envoie **aussi** la clé de son point d'accès.

⛔ **Ce lot ne le corrige pas**, et c'est délibéré : masquer ces lignes rendrait un appareil neuf
inutilisable pour qui n'a pas de câble série. La vraie réponse est la rétention protégée des
traces, traitée par
[`feature-172`](../todo/feature-172-retention-protegee-des-traces-de-securite.md).
⇒ Atténuation disponible **dès aujourd'hui** pour qui s'en soucie : changer la clé depuis
l'interface. La valeur journalisée à la fabrication n'est alors plus celle en service.

### Ce qui reste des conditions de la revue P1

| Condition | Sort |
|---|---|
| G1 — régénération avant que le réseau soit établi | ⭐ **sans objet** : plus de régénération |
| G2 — annoncer le changement d'étiquette | ⭐ **sans objet** : l'étiquette ne change jamais |
| G3 — écrire puis relire la NVS | ⚠️ **maintenue**, et élargie : elle vaut pour l'écriture du mot de passe CHOISI par l'utilisateur |
| G4 — jamais de chaîne vide (point d'accès OUVERT) | ⚠️ **maintenue**, et élargie : vaut aussi pour la saisie utilisateur (longueur ≥ 8 refusée côté appareil, pas seulement dans la page) |
| A1 — tirage radio démarrée | ⚠️ **maintenue** pour l'appareil vierge |
| A4 — clair/masqué lu sur `wizardCompleted` | ⚠️ **maintenue** |
| A5 — le secret hors `/get-config`, `/data`, WebSocket, MQTT, `localStorage` | ⚠️ **maintenue** |


---

## ⭐ FIRMWARE — fait le 2026-11-18

### Le module pur `src/ap_password_logic.h`

⭐ **Deux rôles délibérément ASYMÉTRIQUES**, et c'est le point à ne pas confondre :

| | Alphabet | Pourquoi |
|---|---|---|
| **Fabriquer** la clé de l'étiquette | **restreint**, 72 symboles | elle se recopie à la main puis se tape sur un téléphone |
| **Contrôler** un mot de passe choisi | **tout l'ASCII imprimable** | un gestionnaire de mots de passe produit ce qu'il veut ; refuser un mot de passe solide parce qu'il contient `^` serait absurde |

Exclus de la **fabrication** seulement : `I` `l` `1` `O` `0` `o` (confusion de lecture) et `^`
(⛔ **touche morte** sur un clavier français : `^` puis `a` donne `â`).

### Les quatre pièges que le code ferme, et qu'aucun essai sur matériel n'aurait montrés

1. **Biais de modulo.** `octet % 72` ferait sortir 40 symboles **33 % plus souvent** que les 32
   autres. ⇒ Rejet des octets ≥ 216 (= 3 × 72). Un test compte les 256 octets et exige
   exactement 3 occurrences par symbole ; un second montre le biais **si** la garde est retirée.
2. ⛔ **Chaîne vide = point d'accès OUVERT.** `softAP()` ne pose WPA2 que si la phrase est non
   vide, et ne signale rien. Une source d'aléa défaillante aurait donc ouvert le réseau par
   lequel `/wifi/connect` s'appelle **sans jeton**. ⇒ Boucles bornées, échec **explicite**,
   ancien mot de passe conservé, et un message `critical` qui dit de redémarrer.
3. **Écriture NVS non confirmée.** `saveMqttConfig()` est `void` et peut échouer en silence.
   L'utilisateur recopierait alors sur son étiquette une valeur qui disparaîtrait au démarrage
   suivant. ⇒ `reloadApPasswordFromNvs()` **relit** la clé ; si elle diffère, l'ancien mot de
   passe est remis et la route répond 500.
4. **Un accent collé depuis un gestionnaire.** Un octet UTF-8 serait compté comme « caractère
   spécial », passerait la règle des quatre familles, et casserait la connexion WPA2.
   ⇒ `apPwdFamilyOf` rend 0 hors ASCII imprimable, et le contrôle rend `kBadCharacter`.

### Validation par mutation — 7 sur 7

| Mutation | Tuée par |
|---|---|
| Rejet d'octet supprimé | le test d'équiprobabilité |
| Borne de rejet fausse (224) | idem |
| L'échec rend une chaîne **non vide** | le test du point d'accès ouvert |
| Contrôle des familles retiré du tirage | le test des 400 graines |
| Familles testées **avant** la longueur | ⭐ voir ci-dessous |
| `^` remis dans l'alphabet de fabrication | le test de la touche morte |
| Octet hors ASCII compté comme spécial | le test de l'accent |

⭐ **La mutation « familles avant longueur » a d'abord SURVÉCU**, et c'est le défaut le plus
instructif du lot : le cas d'essai écrit était `Kq7#`, qui porte **déjà les quatre familles**.
Inverser l'ordre rendait donc le **même verdict**, et le test restait vert sur un code faux. Il
fallait un mot de passe **simultanément trop court et incomplet** (`Kq7`). ⚠️ Même leçon qu'à
bug-189 : un test vert ne prouve rien tant qu'une mutation ne l'a pas fait mourir.

### Les deux planchers, et pourquoi ils diffèrent

| | Plancher | Raison |
|---|---|---|
| Mot de passe de l'**appareil** | **8** | il s'attaque **en ligne**, et le ralentissement de [`bug-189`](../done/bug-189-aucun-verrouillage-sur-les-essais-de-mot-de-passe-en-ligne.md) le protège : 1 essai/minute |
| Clé du **point d'accès** | **12** | elle s'attaque **hors ligne** après capture d'une poignée de main : aucun ralentissement ne s'applique |

### Les portes fermées, toutes

`/auth/change-password` **et** `/save-config` appliquent la même règle. ⛔ Sans la seconde,
`/save-config` aurait été la porte de contournement — exactement ce qu'elle était pour la
longueur minimale avant bug-185.

⚠️ **Fausse piste écartée en route** : le commentaire du site d'écriture de `/save-config`
annonce une garde de longueur qui n'y figure pas. Elle existe bien, **en amont** dans le même
handler (`web_routes_config.cpp:1600`). Lire le seul site d'écriture faisait conclure à un défaut
qui n'existe pas.

### Mesures

```
⚠️ CHIFFRES PÉRIMÉS — remplacés par les « Mesures finales » plus bas, relevées après
les corrections des deux revues. Conservés pour mémoire, pas pour citation.
```

⚠️ **7 tests existants sont tombés en rouge au premier essai**, et c'était le comportement
attendu : le nouveau champ `newFamiliesOk` vaut `false` par défaut, donc tout modèle de test qui
l'ignorait refusait désormais le changement. ⛔ Corrigé **dans les tests**, jamais en mettant le
défaut à `true` — un appelant qui oublierait le champ sauterait alors la règle en silence. La
table de combinaisons de `decidePasswordChange` passe de 128 à **256** cas, et le compte
d'autorisations reste **32** : un compte qui aurait doublé aurait signalé une règle non appliquée.


---

## ⭐ LES DEUX REVUES, ET CE QU'ELLES ONT CHANGÉ — 2026-11-18

| Passe | Verdict rendu | État |
|---|---|---|
| `security-reviewer` **P1** (design) | GO SOUS CONDITIONS, 7 conditions | conception changée depuis ⇒ G1, G2 **sans objet** |
| `security-reviewer` **P2** (diff) | **GO SOUS CONDITIONS**, 5 conditions | ✅ **toutes levées** |
| `code-reviewer` | **Request changes**, 4 points | ✅ **tous traités** |

### ⛔ Ce que P2 a trouvé, et que je croyais déjà bouché

**1. Une garde qui n'existait que dans le journal.** Le code écrivait « le point d'accès NE
DÉMARRERA PAS » en cas d'échec de fabrication… et **rien ne l'en empêchait**. `softAP()` ne pose
WPA2 que si la phrase est non vide : une phrase vide crée un réseau **OUVERT**, sans erreur. Et
un point d'accès ouvert sur un appareil vierge, c'est le droit anonyme de choisir le mot de passe
administrateur offert à qui passe à portée radio.
⇒ La garde est désormais au **site d'appel**, aux **deux** sites `softAP` — n'en garder qu'un
aurait laissé la porte ouverte par l'autre.

**2. La relecture NVS protégeait la mauvaise écriture.** Je l'avais mise sur le changement
**choisi** par l'utilisateur, pas sur la **fabrication**. Or c'est la fabrication qui part sur
l'étiquette : si l'écriture échouait en silence, l'installateur recopiait une valeur que le
démarrage suivant ne retrouvait pas. ⇒ L'étiquette aurait été fausse **dès le premier jour**,
sans aucun signal. Le lot combattait précisément ça.

**3. L'entropie n'était pas celle annoncée.** `AuthManager::begin()` s'exécute **avant** le
démarrage de la radio, et `esp_random()` n'est vraiment aléatoire que radio ou ADC active. Les
73,5 bits étaient une **affirmation**, pas une propriété — et la valeur est désormais
**définitive**, puisque c'est l'origine. ⇒ Tirage encadré par `bootloader_random_enable()`.
⭐ `code-reviewer` a vu que le **jeton d'API** était tiré vingt lignes plus haut, **hors** de la
fenêtre : une incohérence dans une seule et même fonction. La fenêtre couvre les deux.

### ⛔ Ce que `code-reviewer` a trouvé, et c'était un défaut que J'AVAIS introduit

⚠️ **Un mot de passe d'appareil avec un accent était REFUSÉ.** En réutilisant le contrôle de la
clé WPA2 pour le mot de passe de l'appareil, je lui avais imposé l'ASCII strict **et** le plafond
de 63 caractères de WPA2. Or ce mot de passe voyage **en JSON**, pas dans une trame Wi-Fi :
« Été2024! » est un bon mot de passe, et le refuser était absurde — pour un utilisateur français
tout particulièrement. Pire, le message disait « il manque une minuscule, une majuscule… » alors
qu'elles y étaient toutes : **refus juste, explication fausse**.

⇒ `apPwdCheck` prend désormais `allowNonAscii`, et les verdicts `kTooLong` / `kBadCharacter` ont
chacun **leur** message aux **trois** sites. Un octet non ASCII est **toléré mais ne compte pour
aucune famille** : « ééééé » ne remplace pas une majuscule.

⚠️ **Et la page promettait autre chose que l'appareil** : elle affichait encore « sans accent »
pour le mot de passe de l'appareil, et n'avait aucune règle de longueur maximale. Les trois
listes de critères sont désormais alignées, avec **deux jeux de règles** distincts.

### Autres corrections de la revue de code
- **Mutex manquant** : `apPassword` est maintenant écrit depuis la tâche HTTP alors que la boucle
  principale le lit pour démarrer le réseau de secours. Réaffecter un `String` libère son ancien
  tampon ⇒ lecture dans une zone libérée, donc phrase WPA2 corrompue **au moment précis où le
  réseau de secours sert**. Verrou récursif posé.
- **La restauration n'était pas relue** : si l'écriture prenait et la relecture échouait, puis que
  la restauration échouait aussi, l'interface annonçait « l'ancien reste en service » pendant que
  la mémoire gardait le nouveau. Relue, et message `critical` si l'état reste incertain.
- `getApPasswordOrigin()` était déclarée sans appelant — employée.
- Le test « 128 combinaisons » en parcourt 256 — renommé.
- Les liaisons de la carte Wi-Fi ne contrôlaient que deux éléments sur six — toutes contrôlées.

### ⭐ DEUX défauts de TEST trouvés par la mutation, et un par le compilateur

1. **« familles avant longueur »** (déjà raconté) : cas d'essai qui portait déjà les quatre
   familles ⇒ l'ordre ne pouvait pas se voir.
2. **« l'accent compte comme une famille »** : mon cas manquait **deux** familles, donc faire
   compter l'accent comme majuscule rendait quand même le même verdict. Il fallait un mot de
   passe où l'accent soit le **seul candidat** possible pour la famille manquante.
3. ⛔ **Le piège du C sur `\x`** : dans `"\xC3\xA9E1!…"`, le `E` est un **chiffre hexadécimal** et
   se fait **avaler par la séquence d'échappement**. Mes chaînes d'essai ne contenaient pas ce que
   je croyais, et l'une passait au vert **par accident**. Corrigé par concaténation.

⚠️ Trois fois la même leçon : **un test vert ne prouve rien tant qu'il n'a pas été attaqué.**

### Mesures finales — ⚠️ relevées APRÈS la dernière correction

```
pio run              SUCCESS · 0 warning dans src/
                     Flash 1 967 297 o (52,7 %) · RAM 98 552 o (30,1 %)
pio test -e native   2 696 cas · 0 régression
build_fs.sh          SUCCESS · littlefs.bin 512 Ko
mutation             13 sur 13 tuées
9 gardes d'hygiène   vertes
```

---

## ✅ ESSAI RÉEL SUR L'APPAREIL — 2026-11-21, NON DESTRUCTIF

⛔ **Aucune écriture NVS n'a eu lieu** : tous les envois sont des **refus**, et le mot de passe
actuel était **correct** à chaque essai — donc ⛔ aucun comptage par le ralentissement de
bug-189 (qui ne compte que `kDenyBadCurrent`). Vérifié en fin de campagne : reconnexion avec le
mot de passe d'origine, **HTTP 200**.

### A — Lecture de la clé du point d'accès (`GET /auth/ap-password`)

| Champ | Relevé |
|---|---|
| HTTP | **200** |
| `ap_ssid` | `PoolControllerAP` |
| longueur de la clé | **8 caractères**, **hexadécimal majuscule** |
| `ap_password_legacy` | **`true`** |

⭐ **L'INVARIANT DU LOT EST VÉRIFIÉ SUR MATÉRIEL, et c'est le résultat le plus important de la
journée** : l'appareil de production, monté en 2.95.0 puis 2.96.1, porte **toujours sa clé
d'origine à 32 bits**. ⛔ Le lot n'a **rien régénéré** — **l'étiquette du boîtier reste vraie**.
C'est le chemin `auth.cpp:207-214` : une clé héritée est **adoptée comme origine**, elle n'est pas
remplacée. ⇒ Le drapeau `legacy` fait exactement son travail : il dit « conseille à ton
propriétaire d'en choisir une autre », ⛔ il ne la change pas dans son dos.

### B — Les cinq refus de la clé Wi-Fi, chacun avec SON message

| Essai | HTTP | Message rendu |
|---|---|---|
| 11 caractères | 400 | « … doit faire au moins **12** caractères » |
| accentuée | 400 | « … n'accepte ni accent, ni caractère invisible, ni espace… » |
| sans majuscule | 400 | « … au moins une minuscule, une majuscule, un chiffre et un caractère spécial » |
| espace final | 400 | « … ni caractère invisible, ni espace en début ou en fin » |
| 64 caractères | 400 | « … ne peut pas dépasser **63** caractères » |

⭐ **Les quatre verdicts restent distincts jusqu'à l'écran.** C'était l'objet de la correction de
`security-reviewer` P2 (« refus juste, explication fausse ») : il est **mesuré**, plus seulement
testé en natif.

### C — L'accent sur le mot de passe de l'APPAREIL (le défaut que la revue avait attrapé)

| Essai | HTTP | Verdict rendu |
|---|---|---|
| `motdepasse2024!` (pas de majuscule) | 400 | famille manquante |
| `Été2024!` | 400 | **famille manquante** — ⛔ **PAS** « caractère interdit » |
| `ééééé2024!A` | 400 | **famille manquante** |
| 7 caractères | 400 | trop court |

⭐ **Les deux moitiés de la correction sont prouvées sur matériel** : l'accent est **toléré**
(sinon le verdict serait « caractère interdit ») **et** il ne **compte pour aucune famille**
(sinon `ééééé2024!A` passerait, l'accent tenant lieu de minuscule). ⚠️ C'est la mutation du
test natif n° 2, rejouée de bout en bout à travers JSON → HTTP → `apPwdCheck`.

### 🔴 DEUX DÉFAUTS DE MESSAGE TROUVÉS PAR CET ESSAI

**1. Un message en ANGLAIS au milieu de messages français.** Un mot de passe d'appareil trop
court rend :

> `New password must be at least 8 characters`

⚠️ Toute l'interface est en français, et les **quatre** autres verdicts de cette même route le
sont (c'est bug-187 qui les a traduits). Ce message-là est **antérieur** au lot et n'a pas été
revu avec les autres — `web_routes_auth.cpp`, branche `kDenyTooShort`. ⛔ Il n'a pas été vu en
natif : les suites comparent le **verdict**, pas la **phrase**.

**2. La clé Wi-Fi emprunte un message qui ne dit pas de quoi il parle.** Quatre des cinq refus
commencent par « Le mot de passe **du point d'accès**… » ; celui de la famille manquante dit
seulement « Le mot de passe doit contenir… » — **mot pour mot celui du mot de passe de
l'appareil**. ⚠️ Sur un écran où les deux objets coexistent, l'utilisateur ne sait pas lequel on
lui reproche.

⛔ Ces deux points **ne remettent en cause aucune garde** : les refus sont justes, c'est leur
**formulation** qui cloche.

### ⛔ CE QUI NE PEUT PAS ÊTRE ESSAYÉ SUR L'APPAREIL DE PRODUCTION

⭐ **« En choisir une autre » est un aller SANS RETOUR sur cet appareil**, et c'est une découverte
de l'essai : aucune route ne restaure l'origine (`getApPasswordOrigin()` ne sert qu'à la
réinitialisation d'usine). ⚠️ Et la clé actuelle — **8 hexadécimaux** — ne pourrait **pas être
ressaisie** : elle est en dessous du plancher de 12 **et** elle échoue à la règle des quatre
familles. ⇒ La seule marche arrière serait une **réinitialisation d'usine**, ⛔ interdite sur
l'appareil de production.

| Essai restant | Pourquoi il ne peut pas se faire ici |
|---|---|
| Choisir une autre clé de point d'accès | Irréversible sans réinitialisation d'usine (ci-dessus) |
| Mot de passe d'appareil accentué **accepté** de bout en bout | Changerait le mot de passe de l'appareil en service, ferait tourner le jeton d'API et révoquerait les sessions ouvertes |

⇒ Les deux rejoignent l'appareil **dédié** de
[bug-186](../todo/bug-186-cinq-essais-de-premier-demarrage-non-prouves-sur-materiel.md), où la
réinitialisation est justement le point de départ.

### ✅ CLÔTURE — où part chaque reste (décisions du 2026-11-21)

- [x] ~~Lire la clé~~ — **fait** : legacy, 8 hexadécimaux, étiquette intacte
- [x] ~~Mot de passe sans majuscule refusé avec le bon message~~ — **fait**, 9 messages relevés
- [x] ~~Accent toléré et ne comptant pour aucune famille~~ — **fait sur matériel**
- [→] **Choisir une autre clé** — porté par [bug-186](../todo/bug-186-cinq-essais-de-premier-demarrage-non-prouves-sur-materiel.md), avec son critère d'acceptation propre
- [→] **Accentué accepté de bout en bout** (persistance NVS d'un UTF-8 multi-octets) — porté par bug-186
- [→] **M1 / M2, les deux défauts de message** — portés par [bug-200](../todo/bug-200-le-mode-maintenance-suspend-tout-le-dosage-sans-que-l-ecran-le-dise.md), qui recompile déjà le firmware ⇒ ⭐ pas de seconde cérémonie de signature, pas de second redémarrage
- [ ] 📸 Captures à refaire (étape **humaine**) : `10_Paramètres_2_Wifi.jpg` (obligatoire), `10_Paramètres_5_Sécurité.jpg`, `01_Assistant_Etape_1_Mot_de_passe.jpg`

⚠️ **Pourquoi la fiche se ferme avec des restes** : ⛔ aucun d'eux ne porte sur un **comportement
non livré**. Le code est en service depuis la **2.95.0**, revu par `security-reviewer` (P1 + P2)
et `code-reviewer`, 2 696 cas natifs, 13 mutants sur 13 tués, et la campagne du 2026-11-21 l'a
mesuré sur l'appareil réel. Ce qui reste, ce sont **deux essais qui exigent un appareil que l'on
peut réinitialiser**, et **deux phrases à réécrire**. Chacun a désormais un foyer **nommé**, avec
sa propre case à cocher — ⛔ rien ne meurt avec cette fiche.
- [ ] 📸 Captures à refaire (étape humaine) : `10_Paramètres_2_Wifi.jpg` (obligatoire),
      `10_Paramètres_5_Sécurité.jpg`, `01_Assistant_Etape_1_Mot_de_passe.jpg`.

## Critères d'acceptation
- [x] L'espace de recherche est porté hors de portée d'une attaque hors ligne, **chiffré dans la fiche**
- [x] Le mot de passe reste recopiable à la main (dire combien de caractères, et pourquoi)
- [x] Le sort de la journalisation en clair est **tranché par écrit** — section dédiée ci-dessus, six situations, résidu nommé et renvoyé à `feature-172`
- [x] Un appareil déjà installé **ne change pas** de mot de passe de point d'accès sans raison
