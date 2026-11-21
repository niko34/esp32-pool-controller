## Métadonnées
- **ID** : feature-171
- **Slug** : bouton-physique-autorisation-temporaire
- **Type** : feature
- **Statut** : todo
- **Priorité** : haute
- **Version cible** : à définir — **après** `feature-151` D3-c (sans D3-c, cette feature est inerte : rien n'est refusé, donc rien n'est à autoriser)
- **Dernière mise à jour** : 2026-11-16
- **Spec(s) liée(s)** : [`feature-151`](../done/feature-151-signature-de-l-image-ota.md) (durcissement « bouton physique », listé dans ses items ouverts) ; amende [ADR-0125](../../../docs/adr/0125-l-expiration-de-l-interrupteur-de-signature-se-mesure-sur-un-horodatage-nvs.md)

## Domaines touchés
- [ ] Capteurs (pH / ORP / température)
- [ ] Régulation (pump_controller, PID)
- [ ] Filtration / Éclairage
- [x] Web — routes HTTP / WebSocket
- [x] MQTT / Home Assistant
- [ ] UART / écran LVGL
- [x] OTA
- [x] Authentification
- [ ] Historique / persistance
- [ ] Frontend (`data/`)

## Agents requis
- [x] spec-reader
- [x] architect
- [ ] pool-chemistry — non : aucun chemin vers `pump_controller`. ⚠️ **Mais** le geste est lu
      dans `loop()`, à côté de la remise à zéro d'usine : toute temporisation ajoutée là
      doit respecter le watchdog (cf. « Contraintes techniques »).
- [ ] ux-ui-designer — non : aucune surface `data/` touchée. Le seul retour utilisateur est
      la LED bleue, traitée ici comme une contrainte matérielle.
- [x] embedded-developer
- [ ] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] security-reviewer — **P1 sur le design avant code, P2 sur le diff** : la feature
      modifie qui peut lever l'exigence de signature OTA.
- [x] doc-writer

## Description

Permettre d'accorder **l'autorisation temporaire** d'installer une image non signée par un
**appui bref sur le bouton physique** de l'appareil, au lieu d'une commande réseau. Un mot de
passe volé ne suffit alors plus : il faut être **devant l'appareil**.

## Contexte

`feature-151` D3-c refusera toute image OTA non signée. Une protection qu'on ne peut jamais
lever est un piège (clé perdue, signature abîmée, appareil ancien chez un client portant une
clé publique périmée) : D3-b2 a donc livré une **autorisation temporaire**, aujourd'hui
accordée par une commande réseau protégée par le mot de passe administrateur.

**Le problème.** L'appareil parle en HTTP ordinaire : le mot de passe traverse le réseau sous
une forme lisible. Après D3-c, ce mot de passe seul convertira en « peut installer n'importe
quel firmware ». C'est le vecteur « jeton admin volé », consigné comme ouvert dans ADR-0125.

**Deux constats établis le 2026-11-16, qui rendent la feature réaliste et peu coûteuse :**

1. ⭐ **Le bouton existe déjà** — `kFactoryResetButtonPin = 35`, câblé sur la carte v2.
   **Aucune modification matérielle.** Les sources KiCad ne sont pas touchées.
2. ⭐ **L'appui bref est libre.** Aujourd'hui, relâcher avant 10 s ne fait **rien** : le code
   écrit « Bouton relâché - factory reset annulé » et s'arrête là (`src/main.cpp:654`).
3. ✅ **Le bouton est ACTIF HAUT** — au repos `LOW`, appuyé `HIGH`. Confirmé par l'utilisateur
   sur la carte le 2026-11-16 ; le commentaire de `constants.h` qui annonçait « actif bas » a
   été corrigé le même jour.

**Et le bouton couvre un cas de panne que la commande réseau ne couvre pas** : un appareil qui
a perdu son Wi-Fi est injoignable, quel que soit le mot de passe. Le bouton, lui, est lu en
permanence.

## Comportement attendu

### Le geste

| Geste | Effet |
|---|---|
| Appui **bref** (relâché entre `kAuthButtonMinMs` et `kAuthButtonMaxMs`) | **Accorde** l'autorisation temporaire |
| Relâché **entre `kAuthButtonMaxMs` et 10 s** | **Rien** (comportement actuel conservé) |
| Maintien **10 s** | Remise à zéro d'usine (inchangé) |

⚠️ **La zone morte entre l'appui bref et les 10 s n'est pas un détail, c'est la garde
principale.** Quelqu'un qui vise la remise à zéro et renonce à mi-course relâche vers 3-5 s :
il ne doit **surtout pas** accorder une autorisation sans le savoir. Valeurs à proposer par
`architect` ; point de départ suggéré : bref = 80 ms à 1 500 ms, zone morte au-delà.

⛔ **Pas de geste d'annulation.** Décision utilisateur du 2026-11-16 : *« il suffit de ne pas
gérer l'annulation avec le bouton »*. Un troisième sens rendrait le geste ambigu, et
**l'annulation immédiate existe déjà** — le bouton « Redémarrer » de l'onglet Système
(`POST /reboot`, `data/app.js:14867`) referme la porte, puisqu'un redémarrage la referme.

### Les sorties

L'autorisation se referme à **la première** de ces conditions :

1. **60 minutes** écoulées — *si l'appareil a une heure valide*
2. **un redémarrage** — **toujours**, avec ou sans heure
3. **une mise à jour réussie** — elle a servi (comportement actuel, conservé)
4. **la commande réseau d'annulation** — tant qu'elle existe (voir « Hors périmètre »)

### ⭐ Le changement de fond : la porte ne survit plus à un redémarrage

**C'est le cœur de la feature, et il renverse une décision actée.** ADR-0125 inscrit l'échéance
comme un **horodatage en NVS**, précisément pour survivre à un redémarrage sans allonger la
fenêtre. Cette exigence est ici **abandonnée** :

| | Aujourd'hui (ADR-0125) | Proposé |
|---|---|---|
| Sans horloge valide | ⛔ **autorisation impossible** (`409`) ⇒ appareil irréparable | ✅ accordée, fermée au redémarrage |
| Redémarrage | ne referme **pas** (l'échéance survit) | ✅ **referme** |
| Effacement NVS raté | l'échéance peut **ré-armer** au redémarrage (dette assumée, ADR-0125) | ✅ **cas inexistant** |

⇒ **Le mécanisme n'est pas corrigé, il est rendu impossible.** Une marque qui ne vit qu'en
**mémoire vive** est effacée par n'importe quelle remise à zéro du processeur.

⛔ **`RTC_NOINIT_ATTR` est EXCLU** : cette zone survit à un redémarrage logiciel, ce qui est
exactement l'inverse du but. Mémoire vive ordinaire (BSS), `std::atomic`.

⚠️ **Les 60 min restent** quand l'heure est valide — elles ne protègent pas du même risque :
elles ferment la porte quand l'opérateur l'a ouverte puis oubliée, sur un appareil qui tourne
des mois sans redémarrer.

### Qui peut appuyer

L'appui physique **vaut l'authentification** : il est accepté même si le mot de passe
administrateur est désactivé ou au premier démarrage — cas où la commande **réseau**, elle,
répond `403`. Être devant l'appareil est une preuve plus forte qu'un mot de passe.

### Ce que l'utilisateur voit

- **La LED bleue** confirme l'octroi par un motif **distinct** de celui de l'appui long
  (aujourd'hui : clignotement 200 ms pendant l'appui, puis 10 clignotements rapides à la
  remise à zéro). ⚠️ Un appui bref dure moins de 1,5 s : le motif actuel ne se verrait pas.
- **Le journal** : une ligne `WARNING` nommant la source (**bouton**, pas réseau) et la durée.
- **Home Assistant** : le témoin existant bascule, comme pour la commande réseau.

### Cas limites

- **Appui au démarrage**, avant que le serveur web soit prêt → accordé quand même (c'est de la
  mémoire vive, aucune dépendance au réseau), journalisé dès que le journal existe.
- **Appui alors qu'une autorisation est déjà en cours** → **prolonge-t-elle ?** ⚠️ Non :
  un second appui ne doit pas permettre d'étendre indéfiniment la fenêtre. À trancher par
  `architect`, mais le défaut proposé est « sans effet, journalisé ».
- **Appui pendant un envoi OTA en cours** → la décision de la session est déjà capturée
  (condition C8 de `feature-151`) : aucun effet sur l'envoi en cours.
- **Rebond mécanique** → anti-rebond obligatoire, sinon un appui unique en vaut plusieurs.
- **Appareil sans heure** → accordée, fermée au redémarrage seulement. Le journal doit le
  **dire** (« jusqu'au prochain redémarrage », pas « 60 min »).
- **MQTT déconnecté** → accordée ; le témoin n'est pas publié. Déjà le cas aujourd'hui.

## Comportement actuel

- Le bouton ne sert qu'à la remise à zéro d'usine (maintien 10 s). Relâché avant, rien.
- L'autorisation temporaire s'obtient **uniquement** par `POST /ota-signature-switch/arm`,
  protégée par le mot de passe administrateur, refusée si celui-ci est désactivé.
- Son échéance est un horodatage NVS : **elle survit au redémarrage**, et elle est
  **impossible sans heure valide**.

## Contraintes techniques

- ESP32, offline first
- Français dans toute l'UI utilisateur et les journaux
- AsyncWebServer : aucun handler bloquant > 50 ms
- ⚠️ **Le geste est lu dans `loop()`** (`src/main.cpp:621`) : ⛔ aucun `delay()` ajouté sur ce
  chemin, `esp_task_wdt_reset()` dans toute attente. Le code actuel fait un `delay(150)` ×10
  **uniquement** dans la branche remise à zéro, juste avant `ESP.restart()` — ne pas prendre
  ce précédent pour une autorisation générale.
- ⚠️ **Concurrence** : l'octroi vient de `loop()`, la lecture de `async_tcp`, le témoin MQTT de
  `mqttTask`. Marque = un **seul** `std::atomic`, comme le miroir actuel — ⛔ pas de mutex,
  ⛔ pas d'état exprimé par deux variables.
- ⛔ **Sources KiCad gelées** : aucune modification matérielle (CLAUDE.md § Règles 5).

## Fichiers potentiellement concernés

- `src/main.cpp` — reconnaissance du geste dans la boucle, motif LED, anti-rebond
- `src/ota_signature_switch_logic.{h,cpp}` — la décision doit accepter une **source** et une
  échéance **absente** (« jusqu'au redémarrage »), sans cesser d'être fail-closed
- `src/ota_signature_switch.{h,cpp}` — bascule de l'échéance NVS vers la mémoire vive ;
  ⚠️ **le retrait du stockage NVS supprime aussi tout le traitement du résidu** (`disarm`,
  `rearms_on_reboot`, `nvs_residual_until`) : c'est une **simplification**, à mener jusqu'au
  bout plutôt qu'à empiler
- `src/web_routes_ota_switch.cpp` — la réponse doit pouvoir dire « pas d'heure de fin »
- `src/mqtt_manager.cpp` — le champ d'échéance doit accepter « aucune »
- `src/constants.h` — `kAuthButtonMinMs`, `kAuthButtonMaxMs`, motif LED
- `tools/check_ota_signature_switch.sh` — les affirmations de forme vont rougir : à reprendre
- `test/test_native_ota_signature_switch*/` — idem
- `docs/API.md`, `docs/MQTT.md`, `docs/UPDATE_GUIDE.md`, `docs/subsystems/ota-manager.md`,
  `docs/user/src/guide-utilisation.html`, `CHANGELOG.md`

## Livrables — check-list

- [ ] A1 — Reconnaissance de l'appui bref dans `loop()`, avec anti-rebond et **zone morte**
      prouvée entre l'appui bref et les 10 s de la remise à zéro
- [ ] A2 — Motif LED distinct confirmant l'octroi
- [ ] A3 — Bascule de la marque : NVS → mémoire vive ; `RTC_NOINIT_ATTR` **exclu**
- [ ] A4 — Échéance **optionnelle** : 60 min si l'heure est valide, « jusqu'au redémarrage »
      sinon ; fail-closed conservé dans tous les cas
- [ ] A5 — Suppression du traitement du résidu NVS devenu sans objet (`rearms_on_reboot`,
      `nvs_residual_until`) et des champs de réponse associés
- [ ] A6 — Journal : ligne `WARNING` nommant la **source** (bouton / réseau) et la nature de
      l'échéance ; ⛔ ne jamais écrire « 60 min » quand il n'y a pas d'heure
- [ ] C1 — ADR **nouveau** disant pourquoi la porte ne survit plus à un redémarrage, et
      amendant ADR-0125 ⛔ **sans le modifier** (décision datée, immuable)
- [ ] C2 — Documentation transverse + guide utilisateur (le geste, et ce qui referme)
- [ ] D1 — Tests natifs : zone morte, anti-rebond, échéance absente, second appui
- [ ] D2 — **Validation par mutation** de chaque test neuf (CLAUDE.md) — en particulier :
      casser la borne haute de l'appui bref **doit** tuer un test
- [ ] D3 — Gardes de forme mises à jour (`check_ota_signature_switch.sh`)
- [ ] E1 — **Preuve de terrain** : appuyer sur le bouton de l'appareil, relever le journal,
      le témoin Home Assistant, puis vérifier qu'un redémarrage referme la porte

## Nouvelles signatures / API (optionnelle)

- Constantes : `constexpr unsigned long kAuthButtonMinMs`, `kAuthButtonMaxMs`
- Fonction publique : octroi depuis `loop()` — ⚠️ ADR-0125 verrouille « `Arm` n'a qu'un seul
  appelant » et `tools/check_ota_signature_switch.sh` le vérifie. **Ce verrou saute ici** : il
  y aura deux sources. L'ADR nouveau doit le dire, et la garde être adaptée — ⛔ pas désactivée.
- Champ de réponse : l'échéance devient **optionnelle** (`null` = « jusqu'au redémarrage »)
- Aucun endpoint nouveau, aucun topic MQTT nouveau

## ADR attendu ?

- **Probable** : **oui**
- **Critère(s) candidat(s)** : 1, 2, 3, 4
- **Sujet pressenti** : « L'autorisation temporaire de signature ne survit pas à un
  redémarrage » — renverse l'exigence centrale d'ADR-0125 et supprime sa dette de résidu NVS.
  Alternatives à tracer : garder l'horodatage NVS (rejetée : impasse sans heure) ;
  `RTC_NOINIT_ATTR` (rejetée : survit au redémarrage logiciel) ; `millis()` (rejetée pour les
  raisons déjà posées dans ADR-0125 — repliement, redémarrage qui prolonge).

## Éléments visibles (pour doc-writer)

### Endpoints HTTP concernés
- `GET /ota-signature-switch` — l'échéance peut désormais être **absente**
- `POST /ota-signature-switch` (annulation) — inchangé
- `POST /ota-signature-switch/arm` — inchangé (voir « Hors périmètre »)

### Champs WebSocket
- aucun

### Topics MQTT
- aucun **nouveau** — le témoin existant change seulement de sources possibles, et son champ
  d'échéance doit accepter « aucune »

### Documentation à produire / mettre à jour
#### Docs vivantes
- `docs/subsystems/ota-manager.md` — le geste, les sorties, la bascule mémoire vive
- aucune page UI touchée

#### Docs transverses
- `docs/API.md` — échéance optionnelle
- `docs/MQTT.md` — échéance optionnelle
- `docs/UPDATE_GUIDE.md` — comment accorder l'autorisation au bouton
- `docs/user/src/guide-utilisation.html` — ⚠️ **le geste doit y figurer**, au même endroit que
  la remise à zéro d'usine : c'est le même bouton, et c'est là qu'on le cherchera
- `CHANGELOG.md`

#### Docs immuables
- `docs/adr/NNNN-...md` — à créer (voir ci-dessus)

## Critères d'acceptation

- [ ] Un appui bref sur le bouton accorde l'autorisation, sur un appareil **sans heure valide**
- [ ] Un relâchement entre la borne haute de l'appui bref et 10 s n'accorde **rien** — vérifié
      sur l'appareil, pas seulement en test natif
- [ ] Un maintien de 10 s déclenche toujours la remise à zéro d'usine, inchangée
- [ ] Un **redémarrage** referme la porte, avec ou sans heure valide
- [ ] Une mise à jour réussie referme la porte (comportement conservé)
- [ ] Le journal nomme la source et ne parle jamais de « 60 min » sans heure valide
- [ ] Le témoin Home Assistant bascule à l'appui et retombe au redémarrage
- [ ] Tous les libellés utilisateur sont en français
- [ ] `pio run` compile sans nouveau warning
- [ ] Les tests neufs sont validés **par mutation**

## Plan de test manuel

1. **Nominal** : appui bref → journal, témoin HA, `GET /ota-signature-switch` dit « accordée ».
   Envoyer une image **non signée** → elle passe (une fois D3-c livré). Vérifier que la porte
   s'est refermée ensuite.
2. **Zone morte** : appuyer et relâcher à ~5 s. Attendu : **rien**, et le journal le dit.
3. **Remise à zéro** : maintenir 10 s. Attendu : comportement d'avant, strictement.
4. **Sans heure** : priver l'appareil d'heure valide, appuyer. Attendu : accordée, et le
   journal annonce « jusqu'au prochain redémarrage ».
5. **Redémarrage** : accorder, redémarrer, relire l'état. Attendu : refermée.
6. **Rebond** : dix appuis brefs successifs. Attendu : pas de comportement erratique.

## Notes pour agent

- ⛔ **Sans `feature-151` D3-c, cette feature est inerte** : rien n'est refusé, donc
  l'autorisation n'autorise rien. Ne pas la livrer en croyant durcir quoi que ce soit.
- ⚠️ **La simplification est le livrable, pas un effet de bord.** Retirer le stockage NVS
  supprime tout le traitement du résidu. Un lot qui ajouterait le bouton **en gardant** la
  NVS aurait deux mécanismes au lieu d'un, et serait pire que l'existant.
- ⚠️ **Deux gardes automatiques vont rougir** (`check_ota_signature_switch.sh` : « `Arm` n'a
  qu'un appelant », affirmations sur la forme des messages). C'est **attendu**. Les mettre à
  jour, ⛔ jamais les contourner par une liste blanche — ce travers a déjà été attrapé par la
  revue sur D3-b2.
- Vocabulaire : l'utilisateur a demandé le 2026-11-16 qu'on dise **« autorisation temporaire »**
  et non « interrupteur », y compris dans les textes que l'appareil affiche. Le renommage est
  inscrit dans `feature-151` pour D3-c — s'y aligner, ne pas réintroduire l'ancien mot.

## Hors périmètre

- ⛔ **Le retrait de la commande réseau d'octroi** (`POST /ota-signature-switch/arm`). C'est une
  **décision produit distincte**, à prendre une fois le bouton éprouvé sur le terrain : tant
  que les deux portes existent, la sécurité vaut celle de la plus faible, mais la retirer prive
  du dépannage à distance sans intervention humaine sur place. À trancher par l'utilisateur,
  dans une fiche de suivi.
- Le chiffrement des échanges sur le réseau local (HTTP en clair) — chantier séparé, plus gros.
- `bug-112` et la distinction `RouteProtection::CRITICAL` / `WRITE` — durcissements de
  `feature-151`, indépendants de celui-ci.
