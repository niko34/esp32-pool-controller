# bug-183 — `/auth/login` : les échecs ne laissent aucune trace, et le mot de passe se compare en temps variable

## Métadonnées
| | |
|---|---|
| Type | bug (sécurité) |
| Statut | **done** — 2026-11-17 (P1 + P2 `security-reviewer`, `code-reviewer`, `doc-writer`) |
| Priorité | **Haute** — deux défauts indépendants sur la seule porte d'entrée par mot de passe |
| Version cible | **2.93.3** |
| Relevé par | `security-reviewer`, 2026-11-16, passe P2 de [`bug-112`](../done/bug-112-journal-non-borne-sur-route-protegee.md) |
| Specs liées | [`bug-112`](../done/bug-112-journal-non-borne-sur-route-protegee.md) (bornage du journal des refus), [`feature-172`](../todo/feature-172-retention-protegee-des-traces-de-securite.md) (rétention protégée) |

## Domaines touchés
- [x] Web — routes HTTP / Authentification (`src/web_routes_auth.cpp`, `src/auth.*`)
- [x] Observabilité (`src/logger.cpp`)
- [ ] Régulation — [ ] Capteurs — [ ] MQTT — [ ] Frontend — [ ] OTA

## Agents requis
- [x] spec-reader · [x] architect · [x] embedded-developer · [x] test-engineer · [x] code-reviewer · [x] doc-writer
- [x] **`security-reviewer`** — obligatoire : la spec touche `auth.*` et le chemin d'authentification. Passe **P1 sur le design** (le choix du bornage du journal n'est pas visible dans un diff) **et P2 sur le diff**.
- [ ] `pool-chemistry` — sans objet : aucun chemin de dosage, aucun seuil chimique.
- [ ] `ux-ui-designer` — sans objet : aucun fichier `data/`.

## Description

Deux défauts **indépendants** sur `POST /auth/login`, la seule porte d'entrée par mot de passe de l'appareil :

1. **Un échec de connexion ne laisse aucune trace.** Rien n'est journalisé, donc une campagne d'essais de mots de passe est **invisible** — y compris après coup.
2. **Le mot de passe est comparé en temps variable.** `src/web_routes_auth.cpp:58` fait `password != authManager.getPassword()` : la comparaison de `String` s'arrête au premier octet différent, donc le temps de réponse dépend du nombre d'octets corrects.

## Contexte

**Le second défaut est une régression de principe, pas un oubli isolé.** Le projet a **déjà** traité ce sujet : `authManager.secureTokenEquals()` existe depuis la **v2.11.2** (`feature-028`) et impose une comparaison à temps constant sur le **jeton**, y compris côté WebSocket (`src/ws_manager.cpp`, cf. [`docs/subsystems/ws-manager.md`](../../../docs/subsystems/ws-manager.md)). La règle est posée, l'outil existe — **le mot de passe y échappe**.

⚠️ **Portée réelle de l'attaque temporelle, à établir avant de dimensionner le correctif.** Sur un réseau local, la gigue réseau et l'ordonnancement FreeRTOS peuvent noyer l'écart, qui se compte en microsecondes. ⛔ Mais « probablement noyé » n'est pas un argument : le correctif coûte une ligne, la primitive existe, et l'écart se mesure. Le dimensionnement de l'attaque est un **objet d'étude de la spec**, pas une raison de ne rien faire.

**Le premier défaut, lui, est sans ambiguïté.** `bug-112` vient de borner le journal des refus `401` pour qu'une trace survive. ⇒ Mais `/auth/login` ne produit **aucune** ligne à borner : un attaquant qui essaie des mots de passe ne laisse rien derrière lui, et le quota de `checkRateLimit` le ralentit sans jamais le **nommer**.

## ✅ RELEVÉ DANS LE CODE COURANT — 2026-11-17 (⚠️ 3 ajouts, dont un qui change le correctif)

⚠️ **Vérification faite AVANT de traiter la fiche**, à la demande de l'utilisateur (*« l'application
a évolué »*). Résultat : **les affirmations de la fiche tiennent**, numéro de ligne `:58` compris.
⇒ Mais elle est **incomplète** sur trois points.

### ✅ Confirmé tel quel
| Affirmation | État au 2026-11-17 |
|---|---|
| `web_routes_auth.cpp:58` : `username != "admin" \|\| password != authManager.getPassword()` | ✅ **exact**, ligne inchangée |
| Aucun journal sur échec de connexion | ✅ **exact** — un seul `systemLogger` dans tout le fichier (`:214`), sans rapport |
| `secureTokenEquals()` existe | ✅ `auth.h:59` / `auth.cpp:88`, employé par `ws_manager.cpp:327` |
| Le `403` premier démarrage est rendu **après** la vérification du mot de passe | ✅ `:64-67`, après le `401` de `:58` |
| `checkRateLimit` s'applique | ✅ `:42` |
| Coquille de bornage réemployable | ✅ `src/auth_log_throttle_site.h` (`AuthLogThrottleSite`) |

### 🔴 AJOUT 1 — une SECONDE comparaison en temps variable, absente de la fiche
`web_routes_auth.cpp:117` (route de **changement de mot de passe**) :
```cpp
if (currentPassword != authManager.getPassword()) {
```
⇒ Même défaut, même fichier, **non mentionné**. ⛔ Traiter `:58` seul laisserait la moitié du
sujet ouvert. ℹ️ `:124` compare aussi `currentPassword != "admin"` au premier démarrage — ⛔ celui-ci
ne fuit rien (valeur publique), **ne pas le « corriger » par symétrie**.

### ⭐ AJOUT 2 — la primitive existe mais n'est PAS accessible
La fiche dit « la primitive existe ». ⚠️ Précision : la vraie primitive générique est
`constantTimeEquals(const String&, const String&)` — et elle est **`static`** dans `auth.cpp:77`,
donc **invisible depuis `web_routes_auth.cpp`**. `secureTokenEquals()` n'en est qu'une enveloppe
**figée sur `apiToken`** : ⛔ elle ne peut pas servir pour le mot de passe.
⇒ Il faudra **exposer** une primitive (ex. `AuthManager::secureSecretEquals(a, b)`), pas en
« réemployer » une telle quelle.

### 🔴 AJOUT 3 — ET C'EST LE POINT QUI CHANGE LE CORRECTIF
`constantTimeEquals` commence par :
```cpp
if (a.length() != b.length()) { return false; }
```
Sa justification est **écrite dans le code** (`auth.cpp:74-76`) :
> *« La vérification de longueur en amont est acceptable : **la longueur du token n'est pas
> secrète** (format fixe, 32 caractères hexadécimaux). »*

⛔ **Ce raisonnement est VRAI pour le jeton et FAUX pour un mot de passe.** La longueur d'un mot de
passe, elle, **est** un secret : ce retour anticipé la révèle par le temps de réponse. ⇒ Réemployer
la primitive **telle quelle** sur le mot de passe introduirait une fuite que la fiche ne prévoit
pas — alors qu'elle interdit par ailleurs, avec raison, de journaliser la longueur du mot de passe.

⚠️ **Le correctif doit donc comparer en temps constant SANS sortie anticipée sur la longueur**
(ex. hacher les deux côtés et comparer les empreintes, de longueur fixe). ⛔ Et la mesure exigée
par le livrable **D3** doit inclure un essai **« mot de passe de longueur différente »**, pas
seulement « faux au 1ᵉʳ octet » vs « faux au dernier ».

### ⚠️ Conséquence sur la portée
`bug-183` ne se referme pas sur `/auth/login` : il couvre **toute comparaison de secret en temps
variable dans `web_routes_auth.cpp`**, et l'exposition d'une primitive sûre pour les secrets de
**longueur variable**.

## ⭐ AJOUT DU 2026-11-17 (après bug-184) — le mot de passe est stocké EN CLAIR

**Vérifié dans le code courant, pas déduit** :
- `AuthManager::setPassword()` (`src/auth.cpp`) fait `adminPassword = pwd;` — **aucune
  transformation**. ⛔ Il n'existe **aucune** fonction `hashPassword()` dans `src/` (la
  documentation en citait une : phrase supprimée par bug-184).
- `src/config.cpp:395` : `prefs.putString("auth_password", authCfg.adminPassword);` ⇒ **le mot de
  passe administrateur est écrit tel quel en NVS**.

⇒ **Pourquoi c'est ICI et pas ailleurs** : le remède (stocker une **empreinte** au lieu du mot de
passe) réécrit **exactement les mêmes lignes** que celles que cette fiche doit déjà corriger — les
comparaisons de `/auth/login` (`:58`) et de `/auth/change-password` (`:117`). Les traiter dans deux
lots séparés ferait réécrire deux fois le même code.

⚠️ **Ce que ça NE résout PAS, et qu'il ne faut pas laisser croire** : une empreinte protège le mot
de passe, ⛔ **pas** le jeton d'API, ⛔ pas le mot de passe Wi-Fi, ⛔ pas les identifiants MQTT —
tous écrits en clair dans la même NVS. Celui qui a la puce en main les lit. ⇒ Ce qui couvrirait ces
trois-là est le **chiffrement de la NVS**, un sujet distinct (⚠️ non ouvert à ce jour ;
`security-reviewer` le recommande).

ℹ️ Relevé par `security-reviewer` pendant la passe P2 de
[`bug-184`](../done/bug-184-authentification-basic-accepte-un-mot-de-passe-qui-n-est-pas-le-bon.md),
versé ici sur son avis.

## 🔴 MESURÉ SUR L'APPAREIL LE 2026-11-17 (v2.92.0) — `GET /get-config` n'est PAS authentifiée

⛔ **Relevé par hasard** en vérifiant `bug-184` sur l'appareil réel : `GET /get-config` répond
**`200` sans aucun identifiant**.

**Vérifié dans le code** : `src/web_routes_config.cpp:2411` —
`server->on("/get-config", HTTP_GET, handleGetConfig);` ⇒ **ni `REQUIRE_AUTH`, ni `checkAuth`,
ni `RouteProtection`**. Ce n'est pas une route déclarée publique : c'est une route **sans aucune
garde**.

⚠️ **Comparaison qui situe l'écart** : `GET /data` (les mesures) répond `401` sans jeton.
⇒ La **configuration entière** est plus exposée que les **mesures**.

**Ce qui sort, et ce qui ne sort pas** (mesuré, 116 champs) :
- ✅ **Protégés** : le mot de passe administrateur, le mot de passe MQTT et le jeton d'API sont
  **masqués** (`******`). ⛔ Aucune identification exploitable ne fuit.
- 🔴 **Exposés** : le **nom du réseau Wi-Fi**, l'**identifiant de connexion MQTT**, le nom mDNS,
  et **toute la configuration de régulation** — consignes pH/ORP, limites journalières et horaires,
  débits de pompes, horaires de filtration, mode d'installation.
  *(⛔ les valeurs réelles ne sont PAS recopiées ici : règle « jamais de SSID ni d'identifiant MQTT
  dans le dépôt ».)*

⇒ **Portée honnête** : ce n'est **pas** une prise de contrôle — c'est une **divulgation**. Elle
dresse la carte complète de l'installation pour qui est sur le réseau local, **sans laisser la
moindre trace** (aucune garde ⇒ aucun journal, aucun quota). ⚠️ Et elle **renseigne** l'attaquant
avant les essais de mot de passe que cette fiche doit justement rendre visibles.

⚠️ **À trancher, pas à corriger en silence** : l'interface web lit `/get-config` — ⛔ vérifier si
elle le fait **avant** d'avoir un jeton (page de connexion, assistant). Si oui, la fermer d'un
coup casserait l'ouverture de l'application.

ℹ️ ⛔ **Sans rapport avec `bug-184`** : ce défaut existait avant et n'a pas été introduit par le
retrait de HTTP Basic. Il est noté ici parce que c'est la même surface — l'admission des requêtes.

## ⭐ SECONDE VÉRIFICATION — 2026-11-17 au soir, APRÈS `bug-185` et `bug-181`

⚠️ **La vérification ci-dessus a été faite AVANT `bug-185`, qui a réécrit `web_routes_auth.cpp`.**
Reprise intégrale sur le code courant (**2.93.2**). ⛔ Deux sections de cette fiche sont devenues
**périmées**, un livrable est **déjà fait**, et une alerte que je m'apprêtais à lever était **fausse**.

### ✅ Les deux défauts de fond TIENNENT — seules les lignes ont bougé
| | Avant | Aujourd'hui |
|---|---|---|
| Comparaison du mot de passe de connexion | `:58` | **`:78`** — `password == authManager.getPassword()` |
| Comparaison du mot de passe courant | `:117` | **`:131`** — `in.currentMatches = (currentPassword == ...)` |
| Aucun journal sur échec | ✅ | ✅ **toujours vrai** — un seul `systemLogger` dans le fichier (`:271`), sans rapport |
| `constantTimeEquals` **`static`**, inaccessible | ✅ | ✅ **toujours vrai** (`auth.cpp:79`) |
| Sortie anticipée sur la longueur | ✅ | ✅ **toujours vraie** (`auth.cpp:80-82`) |

### ⭐ LE LIVRABLE **A2 EST DÉJÀ FAIT** — par `bug-185`, sans l'avoir cherché
La fiche décrit `username != "admin" || password != ...`, dont le court-circuit **sautait la
comparaison du mot de passe** quand le nom d'utilisateur était faux. ⛔ **Ce code n'existe plus.**
`bug-185` l'a remplacé par :
```cpp
decideLogin(username == "admin", password == authManager.getPassword(), ...)
```
⇒ Les deux comparaisons sont des **arguments d'appel** : le langage les évalue **toutes les deux**,
quoi qu'il arrive. **A2 est acquis** — à cocher, ⛔ pas à réimplémenter.

### ⛔ SECTION PÉRIMÉE — « `GET /get-config` n'est PAS authentifiée »
Fermée par `bug-185`, **mesurée 401 sur l'appareil** le 2026-11-17, et close dans
[`bug-181`](../done/bug-181-get-config-expose-la-configuration-sans-authentification.md).
⇒ ⛔ Toute la section est **sans objet**. Conservée pour l'historique uniquement.

### ⛔ LIVRABLE A5 — la question a changé de nature, pas d'urgence
La fiche demande d'arbitrer le `403` du premier démarrage, au motif qu'il distingue « bon mot de
passe » de « mauvais ». ⚠️ **Depuis `bug-185`, `GET /auth/status` publie `virgin`**
(`web_routes_auth.cpp:46`) — **publiquement, sans jeton, par décision délibérée**.
⇒ L'information que le `403` révèle (« cet appareil est encore au mot de passe d'usine ») est
**déjà publiée exprès**. ⛔ Le `403` ne fuit donc **rien de neuf**. A5 se referme en **écrivant
cette raison**, pas en changeant le code.

### ⚠️ FAUSSE ALERTE, levée par la vérification — à ne pas refaire
`src/ota_manager.cpp:22-23` fait `ArduinoOTA.setPassword(authCfg.adminPassword.c_str())`, et **ne
pose aucun mot de passe** quand il vaut `"admin"`. Lu seul, ce code dit : « un appareil au mot de
passe d'usine accepte un flashage de n'importe qui sur le réseau ».
⛔ **C'EST FAUX.** `OTAManager::begin()` **n'est jamais appelé** : `tools/check_ota_manager_isolated.sh`
est une garde CI qui l'**interdit**, elle est verte, et la chaîne `ArduinoOTA` est **absente du
binaire compilé** (vérifié sur `firmware.bin`). Ce fichier est du code mort, isolé exprès.
⇒ ⛔ **Conséquence pour ce lot** : cette ligne n'est **pas** une contrainte. Elle aurait pu faire
croire qu'on ne peut pas toucher au format du mot de passe — ce serait faux.

### ⚠️ TROISIÈME chemin d'écriture du mot de passe, absent de la fiche
`src/web_routes_config.cpp:1644-1645` — `/save-config` écrit `authCfg.adminPassword` **lui aussi**.
⛔ Tout changement de format du mot de passe devrait traiter **trois** sites, pas deux.
*(⚠️ Cette route ne fait pas tourner le jeton après un changement de mot de passe, contrairement à
`/auth/change-password` — défaut distinct, déjà noté en suivi de `bug-185`.)*

## ⛔ PÉRIMÈTRE ARRÊTÉ — ce lot ne stocke PAS d'empreinte

La fiche propose de **stocker une empreinte** au lieu du mot de passe, au motif que le remède
réécrit les mêmes lignes. ⛔ **Écarté, et voici pourquoi, pour qu'on ne refasse pas le débat :**

1. ⭐ **Le défaut temporel se corrige SANS toucher au stockage** : on compare les **empreintes des
   deux côtés, en mémoire**, à temps constant. Longueur fixe ⇒ ⛔ plus de sortie anticipée, donc
   **plus de fuite de la longueur du mot de passe** (c'est l'AJOUT 3 ci-dessus). Le stockage reste
   ce qu'il est, **aucune migration**, aucun risque sur la seule porte d'entrée de l'appareil.
2. **Stocker une empreinte ne protégerait presque rien de plus** : le jeton d'API, le mot de passe
   Wi-Fi et les identifiants MQTT sont ⛔ **en clair dans la même NVS**. Qui tient la puce les lit.
   ⇒ Le sujet réel est le **chiffrement de la NVS**, pas le format d'un champ.
3. Elle imposerait une **migration** sur trois sites d'écriture, sur l'unique porte d'entrée d'un
   appareil **sans port série**. ⛔ Rapport risque/gain défavorable.

⇒ Le stockage en clair **reste écrit** dans cette fiche comme constat. Il n'est pas traité ici.

## ⭐ AUDIT P1 (design) — 2026-11-17 : **GO SOUS CONDITIONS**, neuf conditions

### ⭐ LA CONDITION QUI CHANGE LA CONCEPTION — supprimer `getPassword()`
Le plan prévoyait une garde CI interdisant `== authManager.getPassword()`. ⛔ **Un `grep` se
contourne avec une variable locale.** La vraie garde est le **compilateur** :
⇒ **`AuthManager::getPassword()` est SUPPRIMÉE**, remplacée par une méthode qui rend un
**booléen** (la comparaison se fait **dans** `auth.cpp`). Deux effets :
1. écrire `==` sur le mot de passe devient **impossible** hors de `auth.cpp` ;
2. la **copie du secret** à chaque tentative disparaît (`getPassword()` rendait une `String`
   **par valeur**).
✅ **Vérifié par l'agent principal** : la méthode n'a que **deux** appelants, `:78` et `:131`.
⚠️ Un script CI reste nécessaire **en complément** : `authCfg.adminPassword` est un **second
exemplaire** du secret, global et public (`config.h:313`).

### ⛔ DEUX AFFIRMATIONS DU PLAN ÉTAIENT INEXACTES
- **« granularité de 64 octets »** — mauvais argument. `mbedtls_sha256_update` recopie les octets :
  le temps reste linéaire en la longueur du secret. ⇒ Ça ne compte pas, mais **pour une autre
  raison** : c'est un **décalage CONSTANT**, identique à chaque requête, et l'attaquant n'a
  **aucune référence** pour le calibrer. ⇒ Le commentaire doit porter **ce** raisonnement.
- **« attente de quelques µs sur le verrou SHA »** — faux sur cette cible : le moteur parallèle
  (`SOC_SHA_SUPPORT_PARALLEL_ENG`) **replie sur le SHA logiciel** au lieu d'attendre.

### ⛔ LE PLAN SE CONTREDISAIT SUR LE BORNAGE
Il refusait deux sites pour les deux **natures** d'échec (« doublerait le budget de qui choisit
sa nature ») mais en prenait deux pour les deux **routes** — ⚠️ l'argument vaut à l'identique.
⇒ **UN SEUL site** pour les échecs de mot de passe des deux routes, la route nommée dans le texte.
⚠️ Chiffrage à mettre à jour dans `constants.h` : on passe de 9 à **10** états, ~**380** lignes
par 24 h au pire. Le journal (6 Ko ≈ 27 lignes) reste purgeable — défaut **assumé depuis
`bug-112`**, renvoyé à `feature-172`. Ce lot l'aggrave de ~11 %.

### ⭐ TROIS CHEMINS MUETS QUE LE PLAN LAISSAIT OUVERTS — arbitrés ici
1. **`/auth/change-password` sans mot de passe courant et avec un jeton faux** rend `401` **sans
   aucune trace** : c'est un essai de **jeton**, invisible, alors que le même essai par
   `checkAuth` est journalisé. ⇒ **DÉCISION : journalisé**, par le même site borné.
2. **Le `403` d'un appareil vierge** ⇒ **DÉCISION : NON journalisé.** Sur un appareil vierge, le
   droit anonyme de `/auth/change-password` permet déjà à quiconque de poser le mot de passe :
   ce `403` n'est pas un signal d'attaque, ce serait du bruit.
3. ⭐ **LE SUCCÈS EST MUET** — et c'est l'information **la plus utile de toutes** : après une
   campagne, le journal dira qu'on a essayé, ⛔ **jamais qu'on a réussi**.
   ⇒ **DÉCISION : le succès est journalisé en INFO, avec le nombre d'échecs depuis le dernier
   succès.** Une ligne par connexion réussie — le propriétaire s'y connecte quelques fois par
   jour, et la ligne porte le signal d'attaque.

### ⛔ A5 confirmé, mais avec une MEILLEURE raison que la mienne
Je justifiais le maintien du `403` par « `/auth/status` publie déjà `virgin` ». ⚠️ L'audit donne
plus solide : **l'état vierge est public PAR CONSTRUCTION**, indépendamment de `/auth/status` —
le droit anonyme de `/auth/change-password` le révèle à quiconque l'essaie. ⇒ C'est **cette**
raison qu'il faut écrire, aux **deux** endroits (`:46` et `:82`).

### ⛔ ADR : OUI, finalement — court
Dès lors que la règle devient **opposable en CI** (condition 7), le critère « contrainte
structurante » est rempli. L'ADR fixe deux décisions non évidentes : **deux primitives** parce que
la longueur du jeton n'est pas secrète et celle du mot de passe l'est ; et le **stockage en clair
assumé**.

### ⛔ D3 — réécrit, pas abandonné
La mesure du temps de réponse HTTP est un **rituel** : elle rendrait « indiscernable du bruit »
**avant même le correctif**. Remplacée par : (a) mesure en **cycles CPU**, N=1000, **médiane et
écart interquartile**, sur **cinq** cas (faux au 1ᵉʳ octet, faux au dernier, longueur différente,
exact, secret ≥ 56 octets), aux **mêmes options de compilation** que le binaire livré ;
(b) **désassemblage** de la fonction dans l'ELF livré, montrant l'absence de branchement dépendant
des données ; (c) la mention **« estimation, non mesurée »** sur les ordres de grandeur.

### 🔴 CE QUE CE LOT NE TRAITE PAS — à écrire, sinon on laissera croire que la porte est durcie
⚠️ Le canal temporel est **la plus faible des trois voies d'entrée**. Les deux autres restent
**grandes ouvertes** :
1. ⭐ **Les essais en ligne : 172 800 par jour et par IP, sans aucun verrouillage**, contre un mot
   de passe de 8 caractères minimum. ⛔ **C'est le risque chiffré que `bug-183` ne traite pas.**
2. **Le mot de passe circule en clair sur le Wi-Fi** (HTTP simple, décision produit).
⇒ Ce correctif relève de l'**HYGIÈNE** et de l'application d'une règle que le projet s'est donnée
(`feature-028`), ⛔ **pas** de la fermeture d'un risque chiffré.

### Autres conditions
2. Effacement par **`mbedtls_platform_zeroize`**, ⛔ jamais `memset` nu (le compilateur le
   supprime). ⚠️ Et dire que cet effacement **ne protège pas** le mot de passe, présent en clair
   ailleurs (membre, `authCfg`, NVS, corps HTTP reçu).
5. **Classifieur** : nom faux ⇒ « nom d'utilisateur inconnu » **quel que soit** le mot de passe.
   ⛔ Aucune sortie ne doit dépendre de `passwordMatches` quand le nom est faux — ce serait écrire
   un **oracle du mot de passe dans le journal**. Tué par mutation.
   ⚠️ Et le libellé d'une ligne avec occultés ⛔ ne doit pas laisser croire que la nature annoncée
   vaut pour les N occultés.

## Comportement actuel

- `POST /auth/login` avec de mauvais identifiants : réponse `401`, **aucune ligne de journal**, à aucun niveau.
- La comparaison est `username != "admin" || password != authManager.getPassword()` (`web_routes_auth.cpp:58`) : deux comparaisons de `String`, l'une et l'autre à court-circuit.
- ⚠️ Un `403` distinct est rendu au premier démarrage (mot de passe d'usine) **après** la vérification du mot de passe : la distinction `401` / `403` dit donc à un appelant anonyme si le mot de passe essayé était le bon.

## Comportement attendu

### Les échecs laissent une trace, bornée

- Un échec de connexion produit une ligne **WARNING**, nommant l'IP source.
- ⛔ **Jamais le mot de passe essayé, ni aucun de ses fragments**, ni sa longueur.
- ⭐ **Bornée par le mécanisme de `bug-112`** : un `AuthLogThrottleSite` dédié, recul exponentiel, chemin muet **sans allocation**, drain depuis la `loop()`. ⛔ Sans ce bornage, on rouvre exactement le défaut que `bug-112` vient de fermer — une route non authentifiée qui écrit à volonté.
- La ligne doit distinguer **mauvais mot de passe** de **mauvais nom d'utilisateur** dans le journal (information utile à l'exploitant), ⛔ **sans** que la **réponse HTTP** les distingue.

### La comparaison est à temps constant

- `authManager.secureTokenEquals()` ou l'équivalent sur le mot de passe, sur le modèle de `feature-028`.
- ⚠️ Le nom d'utilisateur est comparé **aussi**, ou alors l'ordre des opérations ne doit rien révéler : aujourd'hui `username != "admin" || …` court-circuite avant même de regarder le mot de passe.
- ⚠️ **Mesurer l'écart avant et après** : une primitive qu'on croit à temps constant mais dont le compilateur réordonne les accès ne prouve rien. Le projet préfère une **mesure** à une intention.

### Le `403` du premier démarrage

À réexaminer : rendu **après** la vérification du mot de passe, il distingue « bon mot de passe, premier démarrage » de « mauvais mot de passe ». ⇒ Arbitrer entre la valeur pédagogique du message et ce qu'il révèle, et **écrire la décision**.

### Cas limites

- Première tentative depuis le démarrage → **journalisée** (cas « jamais journalisé » du module de `bug-112`).
- Tentative isolée une heure après une campagne → journalisée, avec le compte des occultées.
- Corps JSON absent, malformé, ou champs manquants → aucun changement de comportement, mais **vérifier** qu'aucun de ces chemins ne journalise sans borne.
- Authentification désactivée ou premier démarrage → comportement actuel conservé ; ⛔ ne pas ajouter de ligne non bornée sur ces chemins.

## Contraintes techniques

- ⛔ **Le chemin muet n'alloue AUCUNE mémoire** (ADR-0082) : la sortie muette précède toute construction de `String`, y compris celle de l'IP. C'est la condition qui a été vérifiée à la main quatre fois pendant `bug-112`.
- ⛔ La réponse `401` part **toujours**, bornage ou non. Confondre « ne pas journaliser » et « ne pas répondre » transformerait un correctif d'observabilité en faille d'authentification.
- `tools/check_unauth_log_guard.sh` (job CI `unauth-log-guard`) s'applique : tout `systemLogger` dans ce handler devra passer par la coquille, ou porter un `AUTH-LOG-EXEMPT(raison)` **vraie**.
- AsyncWebServer : aucun handler bloquant > 50 ms. ⚠️ Une comparaison à temps constant **ne doit pas** être obtenue par une temporisation.
- Français dans les journaux.

## Fichiers potentiellement concernés

- `src/web_routes_auth.cpp` — le handler `/auth/login` (comparaison l.58, absence de journal)
- `src/auth.{h,cpp}` — la primitive de comparaison à temps constant, si elle doit être étendue au mot de passe
- `src/auth_log_throttle_site.h` — réemploi, ⛔ pas de copie
- `src/constants.h` — constante éventuelle
- `test/test_native_auth_log_throttle/` ou suite neuve
- `docs/subsystems/auth.md`, `docs/API.md`, `CHANGELOG.md`

## Livrables — check-list

- [x] A1 — Comparaison du mot de passe à **temps constant**, ⛔ **SANS sortie anticipée sur la longueur** (empreintes de longueur fixe des deux côtés). ⚠️ Sur **les DEUX** sites : `/auth/login` (`:78`) **et** `/auth/change-password` (`:131`).
- [x] A2 — ✅ **DÉJÀ ACQUIS par `bug-185`** : `decideLogin(username == "admin", password == ..., ...)` évalue les deux comparaisons comme arguments d'appel. ⛔ Vérifié, pas à réimplémenter.
- [x] A3 — Échec de connexion **journalisé** en WARNING, avec l'IP, ⛔ jamais le mot de passe ni sa longueur
- [x] A4 — Ligne **bornée** par un `AuthLogThrottleSite` dédié + drain dans la `loop()`
- [x] A5 — Décision écrite sur le `403` du premier démarrage (conservé tel quel, ou aligné sur le `401`)
- [x] D1 — Tests natifs : bornage, premier échec journalisé, chemin muet sans allocation
- [x] D2 — **Validation par mutation** de chaque test neuf — ⚠️ **5 mutants sur 6 tués ; M1 SURVIT**, et c'est irréductible : remplacer `fixedWidthEquals` par un `memcmp` à sortie anticipée rend la **même valeur**, donc aucun test de résultat ne peut le voir. Couvert autrement : désassemblage (D3) + `tools/check_secret_compare_guard.sh`.
- [x] D3 — ⭐ **RÉÉCRIT** (passe P1), puis livré : A1 est démontré par le **désassemblage de l'ELF livré**, ⛔ pas par un chronométrage HTTP. Le volet « mesure en cycles CPU » est **explicitement reporté**, raison écrite. Voir la section « D3 — ce qui est prouvé » ci-dessous.
- [x] C1 — `docs/subsystems/auth.md` (dix sites, 380 lignes/24 h, `passwordMatches()`, famille `[Auth] …` et les trois limites du compteur), `docs/API.md` (le 401 de `/auth/login` ne distingue pas nom et mot de passe), `CHANGELOG.md` `[2.93.3]`, **addendum** à l'ADR-0127, **nouvel [ADR-0132](../../../docs/adr/0132-le-mot-de-passe-se-compare-par-empreintes-en-temps-fixe.md)** et `docs/BUILD.md` (job `secret-compare-guard` ; comptage de tête remis de 22 à **31 jobs**, il se contredisait avec sa propre fin de cellule).

## Critères d'acceptation

- [x] Un échec de connexion produit une ligne de journal nommant l'IP, **sans** le mot de passe
- [x] 500 échecs consécutifs produisent un nombre **borné** de lignes, et ne purgent pas le journal
- [x] Un échec isolé, une heure après, reste journalisé
- [x] ⛔ **CRITÈRE RÉÉCRIT** — l'ancienne formulation (« écart de temps de réponse indiscernable du bruit, chiffres à l'appui ») était **inatteignable** : distinguer un écart de ~0,1 µs sous une gigue Wi-Fi de l'ordre de la milliseconde demande ~10⁸ requêtes (≈ 580 jours au plafond de 172 800/jour). Un tel essai n'aurait rien prouvé, ni dans un sens ni dans l'autre.
      ⇒ Remplacé par : **le binaire livré ne contient aucun branchement dépendant du contenu dans la comparaison**, établi par désassemblage. ✅ Vérifié (section D3).
- [x] La réponse HTTP ne distingue pas mauvais nom d'utilisateur et mauvais mot de passe
- [x] `pio run` sans nouveau warning ; `check_unauth_log_guard.sh` et `check_memory_discipline.sh` verts

## ⭐ D3 — CE QUI EST PROUVÉ, ET CE QUI NE L'EST PAS (2026-11-17)

### ⛔ Pourquoi le chronométrage HTTP a été ABANDONNÉ, et non oublié

La fiche demandait de mesurer l'écart de temps de réponse entre « faux au 1ᵉʳ octet » et
« faux au dernier ». **Ce chiffre n'aurait rien démontré.** L'écart à détecter est de l'ordre
de 0,1 µs ; la gigue d'un aller-retour Wi-Fi est de 10²–10³ µs. Séparer les deux demande de
l'ordre de **10⁸ requêtes**, soit ~580 jours au plafond de 172 800 essais/jour — et encore,
avec un appareil jumeau étalonné en absolu. *(Ordres de grandeur estimés, non mesurés.)*

⇒ Un résultat « indiscernable du bruit » aurait été **obtenu de toute façon**, correctif ou
non. Le présenter comme une preuve aurait été malhonnête.

### ⛔ Pourquoi la garde CI ne suffit pas non plus

`fixedWidthEquals` est `inline` dans un en-tête. Transformer « OU des XOR sur 32 octets » en
« sortir dès que l'écart est non nul » est une optimisation **légale** : le compilateur a le
droit de l'appliquer, et aucune lecture du **source** ne peut l'exclure.
⇒ `tools/check_secret_compare_guard.sh` empêche la **régression délibérée du source**. Il ne
dit rien du binaire. Les deux revues l'ont relevé indépendamment, et l'en-tête du script ainsi
que le commentaire du job CI le disent maintenant noir sur blanc.

### ✅ CE QUI PROUVE A1 : le désassemblage du binaire livré

ELF `firmware.elf` — SHA256 `5c1f16b693bdde2fe28fa0fd3536165949ff10b9a384b76ea32329f8ad463c30`
BIN `firmware.bin` — SHA256 `a58e2696f9ffcee950dcb2cb581125819d94543188e2ecbbc8b2c91826daf914`
Outil : `xtensa-esp32-elf-objdump -d`, symbole `_ZNK11AuthManager15passwordMatchesERK6String`
(`0x400ee714`, cadre de pile `entry a1, 224` — pile, pas de tas).

La comparaison est compilée en une **boucle matérielle Xtensa `loop`** à compte **fixe de 32
tours** (`movi a11, 32` puis `loop a11, 0x400ee7dd`), dont le corps fait 8 instructions :

```
400ee7c8:  add.n  a8, a10, a9     ; &a[i]
400ee7ca:  add.n  a12, a3, a9     ; &b[i]
400ee7cc:  l8ui   a8, a8, 0
400ee7cf:  addi.n a9, a9, 1
400ee7d1:  l8ui   a12, a12, 0
400ee7d4:  xor    a8, a8, a12
400ee7d7:  or     a8, a2, a8
400ee7da:  extui  a2, a8, 0, 8    ; diff reste sur 8 bits
```

- **Zéro branchement dans le corps** (compté : 0). `loop` est à surcoût nul et à compte fixe :
  les 32 tours s'exécutent **toujours**, quelle que soit la position de l'écart.
- Le verdict sort par `nsau` + `srli` (`0x400ee7f6`) — conversion **sans branchement** de
  « a2 == 0 » en 0/1.
- Les **seuls** branchements conditionnels de la fonction sont : deux `bbsi a8, 7` (mode de
  stockage court/long d'une `String`, qui dépend de la **longueur** de l'entrée, pas de son
  contenu) et le test du canari de pile. ⛔ Aucun ne dépend de la relation entre le candidat
  et le secret.
- Les **deux** `mbedtls_platform_zeroize` sont présents et appelés (`0x400ee7e3`, `0x400ee7ee`,
  littéral `0x400d119c` → `0x401c26d0 mbedtls_platform_zeroize`) : ⛔ non supprimés par
  l'optimiseur, ce qu'un `memset` nu n'aurait pas garanti.
- `getPassword` : **0 symbole** dans l'ELF.

⇒ **Le mutant M1 n'est pas présent dans ce binaire.** C'est la propriété visée, établie sur
l'artefact livré et non sur une intention du source.

### ⛔ Volet (a) — mesure en cycles CPU : REPORTÉ, raison écrite

Chronométrer `fixedWidthEquals` seule avec `esp_cpu_get_cycle_count()` (N=1000, écart à
l'octet 0 contre écart à l'octet 31) tuerait M1 pour de bon : la boucle coûte ~100–200 cycles
et l'écart d'un `memcmp` serait de ~30–150 cycles, bien au-dessus de la résolution du compteur.
*(Ordres de grandeur estimés, non mesurés.)*

⛔ **Non fait, délibérément** : sans port série, ce banc imposerait d'ajouter une **route
d'instrumentation** ou un **binaire spécial** sur l'appareil de **production**. Ajouter une
surface d'attaque pour mesurer une propriété que le désassemblage établit déjà sur le binaire
réellement livré est un mauvais échange. ⇒ À reprendre si un appareil dédié existe (voir
[`bug-186`](../todo/bug-186-cinq-essais-de-premier-demarrage-non-prouves-sur-materiel.md)).

---

## ⭐ AUDIT P2 (diff réel) — 2026-11-17 : **GO SOUS CONDITIONS** + **Request changes**

Deux passes **indépendantes** et simultanées sur le diff non commité : `security-reviewer`
(P2, exigée par la condition 6 de P1) et `code-reviewer`. Elles ont convergé sur les mêmes
manques sans se voir.

### ✅ Conditions levées dans ce tour

| # | Exigée par | Traitement |
|---|---|---|
| Garde CI | les deux | `tools/check_secret_compare_guard.sh` + job `secret-compare-guard` dans `hygiene-specs.yml`. **4 règles**, validées par **8 mutations** (8 refus corrects). |
| Garde CI — site d'appel | `code-reviewer` B1 | ⚠️ **Trou réel** : les règles A–C protégeaient `fixedWidthEquals` sans vérifier que `passwordMatches` l'**appelle**. Un `memcmp(dCandidate, dSecret, 32)` passait les trois. ⇒ **Règle D** ajoutée (appel obligatoire + `memcmp`/`strcmp` interdits dans le corps), mutation-testée. |
| Journal : IP des occultés | `security-reviewer` E | `r.otherIps` était calculé puis **ignoré**, alors que `auth.cpp/appendOcculted` l'affiche. On lisait « et 12 autres échecs » comme « 12 autres essais **de cette adresse** ». ⇒ `(route, nature et IP non précisées)` quand d'autres adresses sont occultées. |
| Ligne de succès non bornée | les deux | `AUTH-LOG-EXEMPT(bug-183)` posé, avec la raison **vraie** et le résidu assumé (≈27 connexions réussies noient le journal de 6 144 o — défaut de `bug-112`, renvoyé à `feature-172` ; ce lot y ajoute une voie, il ne la crée pas). |
| Libellé du compteur | `code-reviewer` Q2 | Disait « échecs depuis la dernière connexion réussie » alors qu'il agrège **les deux routes** `/auth/*` et vit **en RAM**. ⇒ « N échecs d'authentification sur /auth/\* depuis le démarrage ou la dernière connexion réussie ». |
| `mbedtls_platform_zeroize` | `security-reviewer` 2 | `auth.cpp` dit désormais ce que l'effacement **ne protège pas** : membre, `authCfg`, NVS non chiffrée, corps HTTP reçu. |
| `constants.h` « dixième » | les deux | → « onzième ». Dix sites sont maintenant comptés. |
| Saturation réécrite à la main | `code-reviewer` m2 | Remplacée par `authLogThrottleSatInc()`, helper pur déjà testé en natif. |
| Ordre des inclusions | `code-reviewer` m3 | Corrigé. |

### ⭐ Ce que les revues ont CONFIRMÉ en le vérifiant elles-mêmes

- **ADR-0082** : dans `logAuthFailure`, avant `if (!r.emit) return;`, il n'y a qu'une boucle CAS
  sur un atomique, un test de pointeur, un `IPAddress` par valeur et un hachage FNV. La première
  `String` est **après**. Conforme, sans avoir eu besoin d'exemption.
- **Dimensionnement** recompté **à la main par les deux revues** : 10 sites, 380 lignes/24 h,
  ≈87 Ko. Les chiffres du diff sont justes.
- **Compteur mutualisé login / change-password** : agrégation **défendable** (mêmes échecs
  d'authentification sur `/auth/*`, et le site borné fusionne déjà les deux routes). Le défaut
  était dans le libellé, corrigé ci-dessus. Le compteur ne sort que dans une ligne de journal,
  et `/get-logs` comme `/download-logs` sont sous `REQUIRE_AUTH` en première instruction.
- **Fuite par la longueur** : résiduelle et **non exploitable**. SHA-256 travaille par blocs de
  64 o (≤ 55 o = une compression) ; la longueur du candidat est choisie par l'attaquant donc ne
  lui apprend rien, et celle du secret est **fixe** — décalage constant sans référence
  calibrable. Gain si elle était connue : moins d'un facteur 2 sur une recherche exhaustive.
  *(Estimations, non mesurées.)*
- **Bénéfice non annoncé** de la suppression de `getPassword()` : la copie `String` du secret
  **par valeur** à chaque tentative disparaît — un exemplaire en clair de moins dans le tas, sur
  le chemin le plus sollicité par un attaquant.

### ⚠️ Une garde verte qui ne prouvait rien ici

`check_unauth_log_guard.sh` est vert, mais sa règle 3 **ne reconnaît que les lambdas** : les
trois `systemLogger` neufs sont dans des **fonctions libres**, donc invisibles pour elle. Leur
innocuité vient de la **lecture** des deux revues, ⛔ pas du script. ⇒ Spec de suivi à ouvrir
(étendre la règle aux fonctions libres prenant un `AsyncWebServerRequest*`).

### 🔴 LES DEUX PORTES QUE CE LOT NE FERME PAS — à ne pas laisser croire durcies

1. **Essais en ligne sans verrouillage.** `/auth/login` accepte **120 requêtes/minute et par
   IP**, soit **≈172 800 essais par jour et par adresse**, sans aucun verrouillage de compte.
   ⚠️ La borne se **multiplie** par le nombre d'adresses que l'attaquant prend sur le réseau.
   Un dictionnaire d'un million de mots de passe s'épuise en **~6 jours depuis une seule IP**.
   ⇒ **C'est le risque chiffré réel du chemin d'authentification**, et il est sans commune
   mesure avec le canal temporel que ce lot ferme. Un mot de passe aléatoire de 8 minuscules
   tient des siècles : le risque porte sur les mots de passe **faibles**.
2. **Mot de passe et jeton en clair sur le Wi-Fi.** Qui écoute le réseau lit le mot de passe à
   chaque connexion et le jeton à chaque requête. ⛔ Et le jeton suffit à **écrire**
   `adminPassword` via `/save-config` : il vaut contrôle complet de l'appareil.
   ⇒ Décision produit **assumée** (HTTP clair, 2026-11-17) : ⛔ ni TLS, ni proxy, ni VPN.

⇒ **Aucune des deux ne bloque la clôture de bug-183.** Ce lot ne les aggrave pas, et il rend
la première **visible dans le journal** — ce qu'elle n'était pas. Elles partent en specs de
suivi, la première en priorité haute avec sa propre passe P1.

### ⛔ DÉCISIONS ÉCRITES (demandées, pas codées)

- **Le succès de `/auth/change-password` reste moins tracé que celui de `/auth/login`.** Il rend
  pourtant un jeton lui aussi — y compris le changement **anonyme** sur appareil vierge, qui est
  la **prise de possession de l'appareil**. Sa seule trace est « Mot de passe administrateur
  modifié », sans IP ni compteur, et le compteur n'est pas remis à zéro.
  ⇒ **Conservé tel quel dans ce lot.** Raison : cette voie se **révèle d'elle-même** (le
  propriétaire est éjecté, les WebSocket sont révoquées), alors qu'un attaquant discret passera
  par `/auth/login`, qui est désormais tracé. Renvoyé en suivi.
- **Corps HTTP fragmenté** : les handlers de `/auth/login` et `/auth/change-password` ignorent
  `index`/`total`. Un corps en plusieurs morceaux ferait tourner le handler deux fois → double
  réponse et compteur d'échecs gonflé. ⛔ **Préexistant**, les corps réels tiennent dans un
  fragment, et le quota est consommé à chaque appel : ce lot n'ouvre **aucun** contournement.
  ⇒ Hors périmètre, versé en suivi.
- **Chiffrement de la NVS** (appareil volé) : recommandé par les deux passes, **non ouvert**.

### ⚠️ Formulation inexacte corrigée

La section « FAUSSE ALERTE » de cette fiche disait `ArduinoOTA` « absente du binaire ».
**Inexact** : l'instance globale et son constructeur sont liés. Ce qui est absent (0 symbole),
c'est `ArduinoOTAClass::begin`, `handle`, `setPassword` et `OTAManager::begin` — donc **aucun
écouteur n'est démarré**. La conclusion tient, la formulation était trop forte.

### Mesures de ce tour (les miennes, reconstruites caches effacés)

```
esp32dev_core3   SUCCESS   00:00:35.986   warnings : 0
pio test -e native   2630 cas : 2630 réussis   (92 suites)
tools/check_secret_compare_guard.sh   OK (règles A, B, C, D)
check_unauth_log_guard / check_memory_discipline / check_specs / check_doc_links   OK
grep -rn getPassword src/   →   0
hygiene-specs.yml   →   31 jobs (YAML validé)
```

## Plan de test manuel

1. 200 tentatives avec un mauvais mot de passe → toutes rendent `401` ; le journal n'est pas purgé ; un nombre borné de lignes apparaît, avec le compte des occultées.
2. Attendre le drain, envoyer une tentative isolée → journalisée.
3. Mesurer le temps de réponse sur mille tentatives « faux au 1ᵉʳ octet » et mille « faux au dernier octet » ; comparer les distributions, pas les moyennes.
4. Mauvais **nom d'utilisateur** avec le bon mot de passe → même réponse, même délai apparent qu'un mauvais mot de passe.
5. `mb` (`max_alloc_8bit`) de `/get-system-info?hf=1` stable avant / après la rafale.

## Notes pour agent

- ⚠️ **Ne pas livrer A3 sans A4.** Ajouter un journal sur une route non authentifiée **sans** le borner, c'est rouvrir `bug-112` au lieu de le prolonger. Les deux vont ensemble ou aucun des deux.
- ⚠️ Le projet a déjà la primitive à temps constant **et** la règle (`feature-028`, v2.11.2) : ce lot **applique** une règle existante, il n'en invente pas. Si un autre chemin compare encore un secret avec `==` ou `!=`, le dire — ⛔ sans élargir le périmètre sans arbitrage.
- La coquille de bornage est **factorisée** depuis `bug-112` (`src/auth_log_throttle_site.h`) : ⛔ ne pas en écrire une quatrième copie, c'est précisément ce qui a été corrigé.

## Hors périmètre

- La rétention séparée des traces de sécurité → [`feature-172`](../todo/feature-172-retention-protegee-des-traces-de-securite.md).
- `rateLimitMap` : allocation par requête non authentifiée et absence de plafond dur → `feature-131`.
- Les autres routes sans authentification → [`bug-181`](../done/bug-181-get-config-expose-la-configuration-sans-authentification.md), [`feature-070`](../todo/feature-070-durcissement-routes-debug-et-cooldown-reset.md).
