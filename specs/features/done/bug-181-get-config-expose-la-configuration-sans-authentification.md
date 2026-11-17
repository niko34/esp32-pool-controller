## Métadonnées
- **ID** : bug-181
- **Slug** : get-config-expose-la-configuration-sans-authentification
- **Type** : bug — 🔴 **sécurité applicative** (divulgation d'informations)
- **Statut** : **done** — déployé en **2.93.1** le 2026-11-17 et **mesuré sur l'appareil** ; ⚠️ le cas « point d'accès » reporté à `bug-186` — ⚠️ **périmètre réduit au seul `/wifi/status`** depuis le 2026-11-17
- **Priorité** : normale
- **Version cible** : **2.93.1** (déployée et mesurée le 2026-11-17 ; le « 2.89.0 » d'origine était périmé)
- **Spec(s) liée(s)** : `bug-179`, `bug-180` (relevés dans la même campagne) · [`bug-185`](../done/bug-185-fenetre-de-premier-demarrage-ouverte-indefiniment.md) (a fermé `/get-config`) · [`bug-186`](../todo/bug-186-cinq-essais-de-premier-demarrage-non-prouves-sur-materiel.md) (porte l'essai D3)

## Domaines touchés
- [ ] Capteurs · [ ] Régulation · [ ] Filtration / Éclairage
- [x] Web — routes HTTP
- [x] MQTT / Home Assistant (l'identifiant et le broker fuient)
- [ ] UART / écran LVGL
- [x] Authentification
- [ ] Historique / persistance
- [x] Frontend (`data/`) — ⚠️ la page de login et l'assistant consomment ces routes

## Agents requis
- [x] spec-reader · [x] architect · [ ] pool-chemistry · [ ] ux-ui-designer
- [x] embedded-developer · [x] web-ui-developer · [x] test-engineer · [x] code-reviewer · [x] doc-writer

⚠️ `security-reviewer` : **OBLIGATOIRE** — c'est l'objet de la spec, et elle touche `auth` + `web_routes_*`.

## Description
`GET /get-config` répond **200 sans authentification** — c'est **délibéré** (route publique pour le
premier démarrage) et elle **masque** le mot de passe et le jeton d'administration. ⛔ **Mais le
masquage s'arrête là** : l'adresse du broker MQTT, son identifiant, le SSID Wi-Fi et 110 champs de
configuration partent en clair à quiconque est sur le réseau.

## Contexte

**Relevé le 2026-11-15** en vérifiant le livrable A0 de `bug-179`, sur l'appareil réel.

```
/get-config        → HTTP 200  (sans aucune authentification)   ⚠️
/data              → HTTP 401  ✅
/get-logs          → HTTP 401  ✅
/get-system-info   → HTTP 401  ✅
```

### Ce qui est masqué — le mécanisme EXISTE et fonctionne
`web_routes_config.cpp:118` : `isAuthenticated = checkTokenAuth || checkBasicAuth`, puis `:403-410`
masquent `auth_password` (`"******"`) et `auth_token` (`"********..."`).
✅ **Vérifié** : le jeton ainsi exposé **ne donne aucun accès** (HTTP 401 avec lui). ⛔ **Aucune
fuite d'identifiant d'administration.**

### Ce qui ne l'est PAS — 116 champs, émis inconditionnellement
| Champ | État | Source |
|---|---|---|
| `server` (broker MQTT) | **en clair** | hors du bloc `isAuthenticated` |
| `port` (1883) | **en clair** | idem |
| `username` (MQTT) | **en clair** | `web_routes_config.cpp:176` |
| `password` (MQTT) | ✅ masqué | — |
| `wifi_ssid` | **en clair** | `web_routes_config.cpp:249` |
| `wifi_ip`, `mdns_host` | **en clair** | — |
| ~110 champs fonctionnels | **en clair** | consignes, horaires, limites de dosage, étalonnages |

⇒ Un attaquant sur le réseau local obtient **la cible et la moitié du couple d'identifiants** du
broker MQTT. Il ne lui manque que le mot de passe.

### 🔴 LE SSID A UNE SECONDE VOIE — masquer `/get-config` seul ne fermerait RIEN
```
GET /wifi/status  (sans authentification)
→ {"mode":"STA","connected":true,"ssid":"<SSID>","ap_ssid":"","ap_ip":"0.0.0.0"}
```
⚠️ Route publique assumée (« utilisées depuis la page de login en mode AP »,
`web_routes_config.cpp`). ⛔ **Tout correctif qui ne traite qu'une des deux voies donne l'illusion
d'avoir réglé le problème** — c'est le motif d'application partielle que ce projet documente déjà.

## ⭐ MISE À JOUR DU 2026-11-17 — une moitié est fermée, l'autre NON

`bug-185` (livrée en **2.93.0**, déployée le 2026-11-17) a fermé **`/get-config`**, et plus
fortement que cette fiche ne le demandait : ⛔ **la route entière exige désormais une
authentification** (`REQUIRE_AUTH(WRITE)`), au lieu d'une liste blanche champ par champ.

**Mesuré sur l'appareil le 2026-11-17 :**
```
GET /get-config  sans jeton  -> 401   ✅ (était 200)
GET /get-config  avec jeton  -> 200   ✅ inchangé
GET /wifi/status sans jeton  -> 200   ⛔ SSID TOUJOURS EN CLAIR, en mode STA
```

⚠️ **La seconde voie du SSID reste ouverte** — exactement ce que cette fiche annonçait :
« tout correctif qui ne traite qu'une des deux voies donne l'illusion d'avoir réglé le problème ».
⛔ **Cette fiche RESTE en `todo`** pour ce seul motif.

⇒ **Périmètre résiduel** : livrable **A2** uniquement (`/wifi/status`, SSID restreint au mode AP),
plus ses tests. Les livrables A0/A1 sont **sans objet** pour `/get-config` : la route n'émet plus
rien sans jeton, donc il n'y a plus de liste à tenir.

⚠️ **Et le risque principal reste entier** : `/get-config` est désormais fermée **sans que le
premier démarrage ait été rejoué sur un appareil neuf**. Le livrable **D3** de cette fiche est
suivi dans [`bug-186`](../todo/bug-186-cinq-essais-de-premier-demarrage-non-prouves-sur-materiel.md).

## ⭐ VÉRIFICATION DU CODE — 2026-11-17 (reprise du lot)

⚠️ Conformément à la consigne « étudier chaque fiche avant de la traiter », tout a été relu
dans le code d'aujourd'hui. **Trois affirmations tiennent, une est périmée, et un piège
n'était pas écrit.**

### F1 — `/wifi/status` n'a AUCUNE garde ✅ confirmé
`web_routes_config.cpp:2451-2461` : pas de `checkAuth`, pas de `wifiConfigAllowed`, rien.
⛔ **C'est la seule des quatre routes Wi-Fi dans ce cas** :

| Route | Garde |
|---|---|
| `/wifi/status` | ⛔ **aucune** |
| `/wifi/scan` | `wifiConfigAllowed() \|\| checkAuth(WRITE)` |
| `/wifi/connect` | `wifiConfigAllowed() \|\| checkAuth(WRITE)` |
| `/wifi/disconnect` | `wifiConfigAllowed() \|\| checkAuth(CRITICAL)` |
| `/wifi/ap/disable` | `REQUIRE_AUTH(CRITICAL)` |

⇒ Le motif existe déjà dans le projet : ce lot **aligne**, il n'invente pas.

### F2 — le commentaire du code est FAUX ⚠️ à corriger
`web_routes_config.cpp:2450` annonce « Routes Wi-Fi publiques (utilisées depuis la page de
login en mode AP) ». ⛔ **`login.html` n'appelle jamais `/wifi/status`.** Les deux seuls
consommateurs sont `data/wifi.html:555` et `data/wizard.html:1512`.

### F3 — la troisième voie de SSID citée par la fiche est DÉJÀ fermée ✅
`handleGetSystemInfo` (`:2418`) émet aussi `wifi_ssid` — mais la route exige un jeton
(**mesuré 401** le 2026-11-17). `ws_manager.cpp:1663` et `mqtt_manager.cpp:1416` sont
derrière une session authentifiée et le broker. ⇒ **Il ne reste qu'une voie ouverte.**

### F4 — l'assistant est AUTHENTIFIÉ quand il interroge cette route ⭐ décisif
Ordre des étapes : **1. Mot de passe → 2. Wi-Fi** (`data/wizard.html:707-711`). L'étape 1
enregistre le jeton (`:1202`), et les deux pages passent par `authFetch`, qui l'attache.
⇒ ⛔ **Restreindre le SSID ne casse donc PAS le premier démarrage** — c'était le risque
principal annoncé par cette fiche.

### F5 — ⚠️ LE PIÈGE, non écrit jusqu'ici
`wifiConfigAllowed()` devient **faux à l'instant précis où la connexion réussit**
(`:75-84` : vrai en mode AP, ou en STA **non connecté**). Or `data/wifi.html:556-560`
sonde en boucle et conclut au succès par `status.connected && status.ssid === ssid`.

⇒ ⛔ **Une page SANS jeton verrait le SSID disparaître exactement au moment du succès**, et
afficherait « Échec de la connexion » sur une connexion RÉUSSIE. Chemin réel : ouvrir
`/wifi.html` à la main sans être connecté (la page est servie publiquement,
`web_server.cpp:224`) — `app.js` n'y mène que depuis une session ouverte.
⇒ La sonde doit **tolérer un SSID absent**, sans perdre la vérification quand il est là.

### F6 — ⚠️ CE QUE LE CORRECTIF N'APPORTE PAS, à écrire honnêtement
Le nom d'un réseau Wi-Fi est **diffusé en clair par la box elle-même**, dans chaque trame
de balise : quiconque est à portée radio le lit déjà, sans toucher à l'appareil. Le gain
réel se limite donc à **trois** choses, et il faut le dire ainsi :
1. un réseau **masqué** (SSID non diffusé) cesse d'être trahi par l'appareil ;
2. un visiteur **sur le réseau mais hors de portée radio** (VPN, réseau ponté) n'apprend
   plus à quel réseau l'appareil appartient ;
3. **cohérence** : plus une seule des cinq routes Wi-Fi sans garde.
⛔ Ne pas présenter ce lot comme « le SSID n'est plus exposé ». Il ne l'est plus **par
l'appareil**.

### F7 — ⚠️ `wifiConfigAllowed()` N'EST PAS « mode point d'accès »
`:75-84` rend vrai en `WIFI_MODE_AP` **ou** en STA **non connecté**. Or pendant une
tentative de connexion lancée depuis la page servie en point d'accès, le mode est
**`WIFI_MODE_APSTA`**, pas `WIFI_MODE_AP`. ⛔ Un prédicat écrit `mode == WIFI_MODE_AP`
— lecture littérale du critère d'acceptation de cette fiche — **casserait la page de
configuration Wi-Fi en plein usage légitime**. Le test doit porter sur le **bit AP**
(`AP` ou `AP+STA`). *(Relevé par `orchestrator`.)*

### F8 — ⚠️ la garde CI ne reconnaît pas `checkTokenAuth`
`tools/check_unauth_log_guard.sh:57` : `REQUIRE_AUTH|\w*Authorized\s*\(|checkAuth\s*\(`.
⛔ `checkTokenAuth(` ne contient pas `checkAuth(` → **tout `systemLogger` ajouté dans ce
handler ferait tomber la CI**. Le handler doit rester **muet**. *(Relevé par `orchestrator`.)*

### F9 — ⭐ LE PIÈGE F5 EST DÉJÀ NEUTRALISÉ — mesuré, pas déduit
`data/wifi.html:609-623` : la page **se garde elle-même**. Sans jeton en session, elle
**renvoie vers `/login.html`** avant toute sonde. Vérifié en photographiant la page servie
par l'appareil à 390 px : on obtient l'écran de **connexion**, pas le formulaire Wi-Fi.

⇒ Le scénario « page sans jeton qui conclut à tort à un échec » ne peut pas se produire
par un usage normal.

⚠️ **CORRECTION APPORTÉE PAR L'AUDIT P1 — F9 ÉTAIT TROP FORT.** Cette garde vérifie la
**présence** d'un jeton, puis interroge `/auth/status`… **qui est publique et répond toujours**
(`src/web_routes_auth.cpp:33-50`). ⛔ Une page portant un jeton **périmé** franchit donc le
contrôle. « Toujours un jeton » ⛔ **n'est pas** « toujours un jeton valide ».

⇒ **CONSÉQUENCE** : le terme « bit AP » du prédicat n'est **PAS un filet de confort, il est
NÉCESSAIRE**. Déroulé du cas « jeton périmé », par l'audit :
- en point d'accès ou AP+STA → le bit AP expose, la page conclut juste ;
- en STA connecté → `/wifi/connect` rend **401 avant toute sonde** (`:2500-2506`) ;
- en STA non connecté → l'appareil n'est joignable par personne.

⇒ La conclusion sur le périmètre tient (⛔ `data/` non modifié), mais **pour une autre raison
que celle que j'avais écrite**. Le motif n'est pas « la page a toujours un jeton » ; c'est
« aucun chemin réseau ne mène à un faux échec ».

⇒ **CONSÉQUENCE SUR LE PÉRIMÈTRE** : `data/` n'a **pas** à être modifié. Pas de livrable
d'interface, donc ⛔ **ni `ux-ui-designer` ni `web-ui-developer`** — ⚠️ non pas parce que
« c'est petit », mais parce que **la mesure montre qu'il n'y a rien à y changer**. Le terme
« bit AP » du prédicat reste néanmoins, comme **filet** : il garantit que la page de
configuration survivrait même si cette garde côté page disparaissait un jour.

### F10 — trois erreurs de documentation PRÉEXISTANTES sur cette route
- `docs/features/page-settings.md:45` : « Depuis `GET /wifi/status` : `ssid`, `ip`, `mode`,
  `mdns_hostname` » — ⛔ **faux** : la page Réglages ne consomme pas cette route, et la route
  n'émet ni `ip` ni `mdns_hostname`.
- `docs/features/page-settings.md:542` : renvoie à `src/web_routes_wifi.cpp`, **qui n'existe pas**.
- même bloc, `:541` : renvoie à `src/auth_manager.cpp`, **qui n'existe pas non plus** (c'est
  `src/auth.cpp`). *(Les deux premières relevées par `orchestrator`, la troisième en vérifiant.)*

## ⭐ AUDIT DE SÉCURITÉ P1 (design) — 2026-11-17 : **GO SOUS CONDITIONS**

### ⛔ CONDITION 1 — le défaut de la structure OUVRE, il ne ferme pas
La structure proposée initialisait `authEnabled = false`, et la fonction commence par
`if (!authEnabled) return true`. ⇒ **une structure construite par défaut EXPOSE le SSID.**
Un site d'appel qui oublierait ce champ rouvrirait très exactement le défaut corrigé, et
⛔ **aucun test du module pur ne le verrait**.
⇒ `WifiSsidExposureInput{}` doit rendre **MASQUÉ** (initialiser `authEnabled = true`, ou
inverser la polarité du champ), et un test natif l'affirme.

### ⛔ CONDITION 2 — le bit AP se dérive DANS le module pur
⚠️ Le vrai risque n'est pas dans la fonction, il est **au site d'appel** : quelqu'un y écrirait
`apModeBit = wifiConfigAllowed()` et aucun test du module ne le verrait. ⇒ le module reçoit le
**mode brut** et dérive le bit lui-même, avec **repli fermé sur toute valeur inconnue** (0, et
4 = `WIFI_MODE_NAN` en IDF 5), testé valeur par valeur. ⛔ Le handler ne porte plus aucune
expression booléenne de décision.
⇒ **Ma réserve sur le champ `staConnected` est tranchée** : on le garde (il rend tuable la
mutation « réintroduire `wifiConfigAllowed()` »), mais ⛔ il ne protégeait **pas** ce que la
conception croyait. Un test d'invariance le nomme, pour qu'il casse la compilation si on le
supprime.

### ⛔ CONDITIONS 3 à 6
3. Handler **muet et jamais refusant** : seul appel `checkTokenAuth` ; ⛔ ni `systemLogger`, ni
   `checkAuth`, ni `sendAuthRequired`, ni `checkRateLimit` (⚠️ **les trois journalisent**).
   200 dans tous les cas, cinq clés toujours présentes.
4. Suite native **exhaustive (16 lignes)** + **6 mutations** tuées, dont « défaut de structure
   remis à `false` » et « retrait de `tokenValid` ».
5. **Résidus écrits** (voir ci-dessous) dans la fiche, `docs/API.md` et le `CHANGELOG`.
6. **Passage P2 sur le diff réel**, avec mesure sur l'appareil en STA : sans jeton `ssid` vide,
   avec jeton `ssid` rempli, `connected` identique dans les deux cas.

### ⚠️ RÉSIDUS — ce qui restera exposé APRÈS ce lot, à écrire noir sur blanc
1. **En AP+STA connecté, un anonyme du réseau domestique lit toujours le SSID.** États
   concernés : premier démarrage, `forceWifiConfig`, connexion en cours, point d'accès de
   secours avant sa coupure.
2. ⭐ **QUATRIÈME VOIE, hors HTTP — MQTT.** `<base>/diagnostic` porte `WiFi.SSID()` **et**
   l'adresse IP (`mqtt_manager.cpp:1416-1419`) et est publié **RETENU**
   (`safePublish(..., true)`, `:1560`) : ⛔ **lisible par tout abonné du broker, même appareil
   éteint**. `<base>/logs` relaie en outre « WiFi connecté: &lt;SSID&gt; » (`main.cpp:866`).
   La protection est celle du **broker**, pas de l'appareil.
3. **Vérificateur de jeton sans quota ni trace** : hors point d'accès, un `ssid` non vide
   équivaut à « ce jeton est valide ». Jeton de 128 bits comparé en temps constant ⇒
   inexploitable par force brute, mais ⛔ contourne la rétention de traces visée par `bug-183`
   et `feature-172`.
4. Le critère d'acceptation de cette fiche, « SSID **qu'en mode AP** », est **reformulé en
   « bit AP »** (AP ou AP+STA) — la lecture littérale casserait le portail (F7).

### ⚠️ Vérifié par l'agent principal, pas seulement rapporté
`AuthAdmissionInput{}` rend bien **`kAllow`** (`auth_admission_logic.h:25-37`) alors que son
commentaire affirme « Fail-closed PAR CONSTRUCTION ». ⛔ Le même défaut que la condition 1,
mais sur le chemin d'admission de **toutes** les routes, **dosage compris**. Non exploitable
aujourd'hui (`checkAuth` affecte toujours le champ) — ⛔ **hors périmètre de ce lot**, fiche de
suivi à ouvrir.
Et `mqtt_manager.cpp:1416` nomme sa clé `"wifi_"` — ⚠️ **tronquée**, visiblement accidentelle.

## ⭐ AUDIT P2 (diff réel) — 2026-11-17 : **GO SOUS CONDITIONS**, 3 conditions

| Condition P1 | État au P2 |
|---|---|
| 1 — défaut de structure **fermé** | ✅ tenue (`authEnabled = true`, affirmé par un test dédié) |
| 2 — bit AP dérivé **dans le module pur** | ✅ tenue (le handler ne porte aucune expression de décision) |
| 3 — handler **muet et jamais refusant** | ✅ tenue |
| 4 — suite exhaustive + mutations | ✅ tenue (56 combinaisons, sur-ensemble des 16 exigées) |
| 5 — documentation | ✅ **levée depuis** (`doc-writer`) |
| 6 — mesure sur l'appareil | ⛔ **NON TENUE** — exige le déploiement |

### ⛔ CONDITION P2-1 — recompiler avant toute signature : **DÉFAUT RÉEL, ATTRAPÉ**
L'audit a relevé que `src/wifi_status_exposure_logic.h` avait été écrit **après** `firmware.bin`
(restauration du fichier par `test-engineer` à l'issue des mutations). ⇒ le binaire sur disque
**précédait** le dernier état des sources. ⚠️ Signer là-dessus aurait scellé une version qui
n'était pas celle du code — très exactement l'incident du 2026-11-14, pris cette fois **avant**
la signature.
⇒ ✅ **Recompilé**, et vérifié : **0 fichier de `src/` postérieur au binaire**.

### ⛔ CONDITION P2-2 — documentation ⇒ ✅ levée
### ⛔ CONDITION P2-3 — mesure sur l'appareil en STA, **avant** de passer la fiche en `done`
Sans jeton : `ssid` **vide**. Avec jeton : `ssid` **rempli**. `connected` **identique** dans les
deux cas, cinq clés présentes.
⚠️ **Motif, et il est contraignant** : ⛔ aucun test natif ne voit le **câblage** du handler. Une
ternaire inversée ou un `tokenValid` mal affecté laisserait **les 9 tests au vert**.
⛔ Le cas « point d'accès » ne se mesure pas sans couper l'appareil ⇒ **reporté à `bug-186`**,
écrit ici et non passé sous silence.

### ⭐ Correction apportée à l'agent principal par l'audit
Le commentaire du `static_assert`, écrit par l'agent principal, disait qu'une renumérotation
« laisserait le SSID fuir ». ⛔ **C'était à l'envers** : si `AP`/`AP+STA` quittent 2 et 3, le bit
AP se tait et le SSID est **MASQUÉ** — c'est le **portail de configuration qui casse**. La fuite
vient du cas **inverse** (un autre mode prenant 2 ou 3). ⇒ Commentaire réécrit avec les **deux**
défauts, plus la limite que la garde ne couvre pas (un mode NEUF portant un point d'accès
retomberait sur « masqué », sans erreur de compilation).

### Mesures de l'agent principal (⛔ pas recopiées d'un rapport)
`pio run` SUCCESS · `pio test -e native` : **91 suites, 2619 cas, 2619 réussis** ·
`check_unauth_log_guard` · `check_specs` · `check_doc_links` · `check_memory_discipline` : verts ·
`FIRMWARE_VERSION 2.93.1`.

### ⚠️ Specs de durcissement à ouvrir (relevées par les deux passes, ⛔ hors périmètre)
1. ⭐ **`AuthAdmissionInput{}` rend `kAllow`** (`auth_admission_logic.h:25-37`) alors que son
   commentaire affirme « Fail-closed PAR CONSTRUCTION ». ⛔ Chemin d'admission de **toutes** les
   routes, **dosage compris**. Non exploitable aujourd'hui. **À traiter en premier.**
2. **MQTT `<base>/diagnostic` retenu** porte SSID et adresse IP ; clé tronquée `"wifi_"`.
3. ⚠️ **Mot de passe du point d'accès à 32 bits** (8 caractères hexadécimaux, `auth.cpp:108-110`).
   Établi : l'entropie. ⛔ **Soupçonné, NON vérifié** : qu'une capture radio permette de le
   retrouver hors ligne. Derrière ce point d'accès, `/wifi/connect` et `/wifi/disconnect`
   (effacement des identifiants) sont ouverts **sans jeton**.
4. **Oracle de validité du jeton** sur `/wifi/status`, sans quota ni trace — à rattacher à
   `bug-183` / `feature-172`.

## ✅ MESURE SUR L'APPAREIL — 2026-11-17, 192.168.1.48, **2.93.1 déployée**

La condition **P2-3** est **LEVÉE**. Firmware seul (`--use-sig firmware`) : `data/` n'a pas changé.

| `GET /wifi/status` | `ssid` | `connected` | nb de clés |
|---|---|---|---|
| **sans jeton** | ⭐ **`""` (vide)** | `true` | **5** |
| **avec jeton** | **non vide** (12 car.) | `true` | **5** |

⇒ Les trois exigences de l'audit sont tenues : `ssid` vide sans jeton, rempli avec, et
`connected` **identique** dans les deux cas — ⛔ c'est elle qui aurait cassé l'assistant si on
l'avait masquée par erreur.

### Non-régression des routes voisines, mesurée dans la foulée
```
/get-config       sans jeton 401  · avec jeton 200
/get-system-info  sans jeton 401  · avec jeton 200
/data             sans jeton 401  · avec jeton 200
/wifi/scan        sans jeton 401  · avec jeton 200
/auth/status      sans jeton 200  · avec jeton 200   (publique par contrat)
```
**Santé** : 0 erreur au journal, `max_alloc_8bit` 53 236 o.

### ⛔ CE QUI RESTE NON PROUVÉ SUR MATÉRIEL
Le cas **point d'accès** (`ssid` servi **sans** jeton en mode AP / AP+STA) ⛔ **n'a pas été
mesuré** : il exige de couper l'appareil de son réseau. C'est la moitié du prédicat qui garantit
que le **portail de configuration** survit — ⇒ **reporté à `bug-186`**, dont c'est exactement le
périmètre (appareil dédié réinitialisé, horizon début 2027).
⚠️ Il est couvert en natif (cas 4 et 5 de la suite, et la mutation « AP strict sans APSTA » est
tuée), ⛔ mais **pas sur un appareil**.

## Comportement attendu
1. ⛔ **Rien de sensible hors du bloc `isAuthenticated`.** Le non authentifié ne reçoit que le
   **strict nécessaire** au premier démarrage et à la page de login.
2. ⚠️ **Déterminer ce strict nécessaire par LECTURE du frontend**, pas par supposition : que lisent
   réellement `data/login.html`, `data/wifi.html`, `data/wizard.html` et le chemin de premier
   démarrage de `data/app.js` **avant** d'être authentifiés ?
3. `/wifi/status` : même traitement. Si le SSID y est nécessaire **en mode AP** (pour aider
   l'utilisateur à se reconnecter), le limiter **à ce mode** — ⛔ pas en mode station.
4. Le masquage doit être **par défaut** : ⚠️ aujourd'hui un champ ajouté hors du bloc est exposé
   **sans que rien ne le signale**. ⇒ Inverser la logique si possible (liste blanche publique).

### Cas limites
- ⛔ **Premier démarrage** : l'assistant doit rester **entièrement fonctionnel** sans authentification.
- **Mode point d'accès** (`PoolControllerAP`) : la page de configuration Wi-Fi doit fonctionner.
- **Authentification désactivée** (`authEnabled = false`) : comportement inchangé, tout est ouvert
  par choix de l'utilisateur.
- ⚠️ Un client tiers (Home Assistant, script) qui lirait `/get-config` sans jeton **cesserait de
  fonctionner** ⇒ à signaler dans le CHANGELOG comme **changement de comportement**.

## Comportement actuel
`handleGetConfig` ne bloque pas : elle calcule `isAuthenticated` et s'en sert uniquement pour
masquer deux champs. Les 114 autres sont émis dans tous les cas.

## Contraintes techniques
- ⛔ **Ne pas casser le premier démarrage ni la page de login** — c'est le risque principal.
- Champs de réponse : ⚠️ un champ **absent** plutôt que masqué peut casser un front qui le lit.
  ⇒ Décider **masquage** ou **omission**, et s'y tenir.
- ESP32, pas de dépendance neuve. Logique de décision **pure** et testable (ADR-0017).

## Fichiers potentiellement concernés
- `src/web_routes_config.cpp` — `handleGetConfig` (`:118`, `:176`, `:249`, `:403-410`), `/wifi/status`
- `data/login.html`, `data/wifi.html`, `data/wizard.html`, `data/app.js` — ce qu'ils lisent avant authentification
- `docs/API.md`, `CHANGELOG.md`

## Livrables — check-list
- [~] A0 — ⭐ **sans objet pour `/get-config`** depuis `bug-185` (route entière authentifiée). Reste valable pour `/wifi/status`.
- [x] A1 — ✅ **fait autrement** par `bug-185` : `/get-config` entière derrière l'authentification (mesuré 401 le 2026-11-17)
- [x] A2 — livré v2.93.1 ; ⛔ **TOUJOURS OUVERT** — `/wifi/status` rend le SSID en clair sans jeton, en mode STA (mesuré le 2026-11-17). **C'est le seul motif qui maintient cette fiche en `todo`.**
- [x] A3 — Logique de décision en module **pur**, testable en natif
- [x] C1 — `docs/API.md` (contrat des deux routes), `CHANGELOG.md` (**changement de comportement**)
- [x] D1 — Suite native sur la décision d'exposition
- [ ] D2 — Validation par MUTATION (⚠️ dont « un champ neuf est exposé par défaut » ⇒ doit tuer)
- [ ] D3 — ⭐ **Essai de terrain** — ⚠️ **déplacé dans [`bug-186`](../todo/bug-186-cinq-essais-de-premier-demarrage-non-prouves-sur-materiel.md)**, repoussé par l'utilisateur au 2026-11-17 (appareil réinitialisé, horizon début 2027)

## ADR attendu ?
- **Probable** : non — durcissement, sans alternative structurante. ⚠️ Sauf si l'inversion
  liste noire → liste blanche est jugée structurante : alors critère 1.

## Critères d'acceptation
- [ ] `/get-config` **sans** authentification n'expose ni `server`, ni `username` MQTT, ni `wifi_ssid`
- [x] `/wifi/status` **sans** authentification n'expose le SSID **qu'avec le « bit AP »** (AP **ou** AP+STA ; reformulé, cf. résidu 4) — v2.93.1
- [ ] ⭐ **Le premier démarrage et la page de login fonctionnent** (essai réel)
- [ ] `/get-config` **avec** authentification est **inchangée**
- [ ] Un champ ajouté par erreur hors de la liste blanche **n'est pas exposé**
- [ ] Suite native verte, mutations tuées · `pio run` sans nouveau warning · `build_fs.sh` passe

## Plan de test manuel
1. ⭐ **Premier démarrage** (appareil réinitialisé ou mode AP) : l'assistant va au bout **sans authentification**.
2. Page de login : s'affiche et permet de se connecter.
3. `curl` sans jeton sur `/get-config` et `/wifi/status` ⇒ aucun champ sensible.
4. `curl` **avec** jeton ⇒ réponse complète, inchangée.
5. Home Assistant / MQTT : toujours fonctionnels.

## Notes pour agent
- ⚠️ **Défaut PRÉEXISTANT**, trouvé en vérifiant autre chose (`bug-179` A0). Divulgation
  d'informations, ⛔ **pas** une compromission : les identifiants d'administration sont masqués et
  le jeton exposé **ne donne aucun accès** (vérifié).
- ⛔ **Le risque du correctif est de casser le premier démarrage en silence** — un appareil neuf ou
  réinitialisé deviendrait inconfigurable. L'essai D3 n'est pas optionnel.
- ⚠️ **Les deux voies du SSID doivent être traitées ENSEMBLE**, sans quoi le correctif ne corrige rien.

## Notes techniques — A2 (v2.93.1)
- Module pur `src/wifi_status_exposure_logic.h`, suite `test/test_native_wifi_status_exposure/`. Route publique, toujours 200, cinq clés, `ssid` vide si masqué. Documenté dans `docs/API.md`, `CHANGELOG.md`, `docs/subsystems/auth.md`.
- Docs préexistantes corrigées (F10) : `page-settings.md` (route non consommée, deux chemins inexistants).
- **Résidus** : (1) AP+STA connecté, SSID lisible (fenêtre probablement courte, `main.cpp:1774-1778`, lu non mesuré) ; (2) MQTT `<base>/diagnostic` retenu (SSID + IP) et `<base>/logs` : protection = broker ; (3) oracle de jeton sans quota ni trace ; (4) critère reformulé en « bit AP ».
- **Pas d'ADR** : les 4 critères ont été passés un par un par l'architecte, aucun n'est rempli. Alternatives écartées : (a) réutiliser `wifiConfigAllowed()` (≠ mode AP, F7 : vrai aussi en STA non connecté, et ne tue pas la mutation) ; (b) omettre la clé `ssid` (crée une forme de réponse neuve alors que `WiFi.SSID()` rend déjà `""` en AP pur) ; (c) tester `WIFI_MODE_AP` strict (casserait le portail en AP+STA).
