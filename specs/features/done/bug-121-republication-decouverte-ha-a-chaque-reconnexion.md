## Métadonnées
- **ID** : bug-121
- **Slug** : republication-decouverte-ha-a-chaque-reconnexion
- **Type** : bug — **republication redondante** (travail inutile au moment le plus fragile). ⚠️ L'hypothèse de « boucle de déconnexion » est **RÉFUTÉE** — voir le verdict du 08/09 ci-dessous
- **Statut** : **done** (2026-09-20) — revue `code-reviewer` **Approved** en 3e passe. Code + tests livrés le 2026-09-20 (A1, A2, A4, D2, T1, T2 ;
  A3 partiellement couvert, voir plus bas). Reste : revue `code-reviewer` avant
  clôture vers `done/`.
- **Priorité** : **normale** — abaissée le 08/09 : les 13 déconnexions MQTT ne relèvent PAS de cette spec, elles sont rattachées à `bug-080`
- **Version cible** : 2.53.1 (corrigée le 2026-09-20 — la 2.49.0 d'origine est dépassée, le firmware est en 2.53.0)
- **Dernière mise à jour** : 2026-09-20 (doc-writer — checklist livrables + section « Périmètre réduit »)
- **Spec(s) liée(s)** : `bug-080` lot C (fragmentation du tas — même signature d'allocation), ADR-0010 (stabilité MQTT), ADR-0011 (mqttTask dédiée), ADR-0033 (budget de journalisation)

## Domaines touchés
- [ ] Capteurs (pH / ORP / température)
- [ ] Régulation (pump_controller, PID)
- [ ] Filtration / Éclairage
- [ ] Web — routes HTTP / WebSocket
- [x] MQTT / Home Assistant
- [ ] UART / écran LVGL
- [ ] OTA
- [ ] Authentification
- [ ] Historique / persistance
- [ ] Frontend (`data/`)

## Agents requis
- [x] spec-reader
- [x] architect
- [ ] pool-chemistry — aucun chemin de dosage touché
- [ ] ux-ui-designer — aucune surface UI
- [x] debugger — **la causalité est à établir, pas à supposer**
- [ ] embedded-developer
- [x] web-ui-developer — `mqtt_manager.*` relève de son domaine
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description

À **chaque** reconnexion MQTT, l'appareil republie l'**intégralité** des 60
entités d'auto-discovery Home Assistant — alors que ces messages sont publiés
avec `retain = true` et sont donc **déjà conservés par le courtier**. La
republication est redondante dans le cas nominal, et elle est temporellement
suivie d'une nouvelle déconnexion selon un motif qui suggère une boucle.

## Contexte — l'observation

Relevé le **2026-09-08** sur ~21 h de journal continu (firmware 2.45.0), par la
capture forensique automatique de `bug-080`. Faits établis :

- **13 déconnexions MQTT**, toutes `état=-3` (*connection lost*) sauf une `-4`.
- **Zéro perte WiFi** sur la même période, RSSI −39 dBm. Motif de comptage
  vérifié contre `src/main.cpp:548`. **La piste réseau est écartée** : c'est la
  session TCP vers le courtier qui tombe, l'association 802.11 tenant bon.
- **4 déconnexions sur 13 entre 01 h et 03 h**, quand personne n'ouvre
  l'interface — donc sans rapport avec l'usage de l'application.

### Le motif qui a lancé cette spec

Cinq déconnexions en **65 secondes**, et le délai entre la fin de la rafale de
découverte et la déconnexion suivante **décroît de façon monotone** :

```
06:36:54  Découverte HA #10 : 54/54 entités, 0 échec, 270 ms
06:37:04  MQTT déconnecté — état=-3        ← 10 s après
06:37:14  Découverte HA #11 : 54/54 entités, 0 échec, 267 ms
06:37:22  MQTT déconnecté — état=-3        ←  8 s après
06:37:32  Découverte HA #12 : 54/54 entités, 0 échec, 199 ms
06:37:36  MQTT déconnecté — état=-3        ←  4 s après
06:37:47  Découverte HA #13 : 54/54 entités, 0 échec, 191 ms
06:37:48  MQTT déconnecté — état=-3        ←  1 s après
06:37:52  Découverte HA #14 : 54/54 entités, 0 échec, 166 ms
```

10 s → 8 s → 4 s → 1 s. Un déclencheur indépendant ne produirait pas une
décroissance monotone : quelque chose **s'accumule** d'une rafale à la suivante.
Le numéro de rafale `#14` dit par ailleurs que **quatorze** republications
complètes ont eu lieu depuis le démarrage.

⚠️ **Le keepalive est à 60 s** (`mqtt_manager.cpp:85`, tolérance Mosquitto 90 s).
Des déconnexions à 1–10 s d'intervalle ne peuvent donc **pas** être des délais
d'inactivité. Cette hypothèse est écartée d'emblée.

### Ce que la 2.48.1 change

Le nombre d'entités est passé de **54 à 60** (relevé après déploiement le
08/09 : `Découverte HA #1 : 60/60 entités`). Le coût de chaque rafale a donc
augmenté d'environ **11 %** — sans rapport avec ce défaut, mais il l'aggrave.

## ⚠️ VERDICT DU 2026-09-08 — LA CAUSALITÉ EST RÉFUTÉE

Livrable D1 rendu. **Mon hypothèse était fausse, et le sens de la flèche est
inversé.**

### La flèche va déconnexion → rafale, pas l'inverse

Dans le journal de 21 h, l'ordre est **strictement alternant : D R D R D R…** ×13.
**Toute rafale est PRÉCÉDÉE d'une déconnexion**, sans exception — et c'est
mécanique : `mqtt_manager.cpp:459` remet le drapeau à chaque connexion réussie.
Une rafale ne peut donc pas *causer* la déconnexion qui l'a déclenchée.

### Les deux décomptes, dont celui qui réfute

| | Sur 13 |
|---|---|
| Déconnexions précédées d'une rafale (< 30 s) | **4** |
| Rafales suivies d'une déconnexion (< 30 s) | **4** |
| **Rafales NON suivies d'une déconnexion** | **9** — délais de **15 min à 5 h 10**, et `#14` jamais (52 min jusqu'à la fin de trace) |

La rafale n'est donc **ni nécessaire ni suffisante**.

### Ce qui tue définitivement l'hypothèse

Les **durées** de rafale **décroissent** au fil de la salve — 270, 267, 199, 191,
166 ms — avec **`0 échec`** à chaque fois. Si la rafale épuisait les tampons TX,
elle **ralentirait** et signalerait des échecs : c'est **anti-corrélé**. Et la
rafale `#14` fait exactement le même travail que `#10`–`#13` tout en **brisant la
chaîne**. Cela suffit à falsifier « la rafale cause la déconnexion ».

Le `code=-2` des premières tentatives de `#10`→`#13` est un échec du **`connect`
TCP**, donc **AVANT toute publication** : il ne peut pas être un effet de la
découverte.

### Mon contre-exemple était un artefact — double erreur

J'avais mis le `debugger` sur la piste de `journal_20260907_0818.txt`, « 3 rafales
pour 0 déconnexion ». Faux **deux fois** : le fichier duplique son contenu en deux
sections (`Historique persistant` puis `Session courante`), donc le grep comptait
3 là où il y en a **2** ; et les deux rafales portent le **même numéro `#1`** —
le compteur repart à 1 à chaque démarrage. Ce sont donc deux **premières**
connexions après boot, sans déconnexion possible en amont. Mon arithmétique
« 3 rafales = 1 boot + 2 reconnexions » supposait un compteur cumulatif : il ne
l'est pas.

### La vraie cause est ailleurs, et elle est déjà instruite

Le moteur des 13 déconnexions est la **fragmentation du tas 8 bits** : `maxBloc8`
décroît de 51 188 à 6 388 sur les 21 h, et les déconnexions suivent ce **niveau**
— zéro pendant les 6 h 30 à ≥ 28 660, **9 sur 13** à 7 924, avec des sondes
d'échec à 1 012–1 524 o, sous les 1 622 o du tampon TX. Après redémarrage
(`maxBloc8 = 55 284`) : **zéro déconnexion**.

⇒ **Les 13 déconnexions sortent du périmètre de cette spec** et sont rattachées à
`bug-080`, où le relevé est consigné.

> ⚠️ **RÉSERVE AJOUTÉE LE 2026-09-08 (relevé instrumenté de 21:33) — la
> corrélation au niveau N'EST PAS REPRODUITE.**
> Le relevé de **8 h 49 / 12 673 échantillons** pris à travers l'instrument de
> `feature-122` donne l'inverse du motif ci-dessus : les **5** déconnexions sont
> survenues aux niveaux **hauts** de `maxBloc8` (**21 492–24 564**), et il y en a
> eu **zéro** pendant la période la **plus basse** (**12 276**).
>
> **Ce que cela change** — et rien de plus :
> - le rattachement des déconnexions à `bug-080` reste une **hypothèse de
>   travail**, **pas un fait établi** ;
> - l'effet de seuil décrit autour de **7 924** n'est **ni confirmé ni
>   infirmé** : 12 276 reste peut-être un niveau « sûr », et le relevé n'est pas
>   descendu jusqu'aux régimes de la veille ;
> - **ne pas trancher** dans un sens ni dans l'autre, et **ne pas rapatrier** les
>   déconnexions dans le périmètre de cette spec sur cette seule base.
>
> Ce qui reste **inchangé** : la republication de 60 entités `retain = true` est
> du **travail redondant** et se justifie sur son seul mérite. Le piège de la
> « fausse défaite » décrit ci-dessous en devient d'autant plus actuel — si le
> lien au niveau n'est pas établi, il n'y a aucune raison d'attendre que ce lot
> fasse disparaître les déconnexions.
> Relevé complet : `bug-080` § « RELEVÉ INSTRUMENTÉ DU 08/09/2026 (21:33) ».

### Ce que ce lot garde, et pourquoi il vaut toujours d'être fait

La republication reste **du travail redondant** : 60 entités publiées avec
`retain = true`, donc **déjà conservées par le courtier**, réémises au moment le
plus fragile — juste après une reconnexion, sur un tas déjà dégradé. Ça se
justifie **sur son seul mérite**, sans prétendre guérir les déconnexions.

⚠️ **Et c'est le piège à éviter en le livrant** : si ce lot part seul et que les
déconnexions persistent — ce qui est **attendu** —, on pourrait conclure que le
correctif a échoué. Il n'aura pas échoué : il ne visait pas ça. La sonde
`maxBloc8` avant/après rafale (livrable D2) sert précisément à **empêcher cette
fausse défaite**, en montrant que la rafale ne consomme pas la ressource rare.

## Le problème

`src/mqtt_manager.cpp:459` remet `discoveryPublished = false` à chaque connexion
réussie, juste avant d'appeler `publishDiscovery()` :

```cpp
discoveryPublished = false;
esp_task_wdt_reset();
publishDiscovery();              // 54 publish — long si CPL lossy
```

Or `publishDiscovery()` publie chaque entité avec **`retain = true`**
(`safePublish(configTopic, payload, true)` — **ligne 1995** sur HEAD au 2026-09-20 ; la rédaction citait 1991). Le courtier
conserve donc ces messages : Home Assistant les relit **de lui-même** à son
propre redémarrage, sans que l'appareil ait à les réémettre.

**La republication n'est nécessaire que dans trois cas** :

1. Les messages retenus ont été **perdus** (courtier redémarré sans persistance).
2. Le **contenu** de la découverte a changé (mise à jour de firmware ajoutant des
   entités — précisément le passage de 54 à 60).
3. Le **préfixe de topic** a changé (reconfiguration).

Dans tous les autres cas — la grande majorité des reconnexions — la rafale est
du travail pur, payé en tampons TX WiFi au moment le plus fragile : juste après
une reconnexion.

### Lien suspecté avec `bug-080`, à établir et non à supposer

La famine qui accompagne ces épisodes porte la signature déjà identifiée :
`caps=0x080C`, blocs de **1 622 o**, `heap_caps_malloc` — les **tampons TX
WiFi**. Une rafale de 60 publications retenues est exactement le genre de charge
qui les épuise, et un échec d'émission fait retourner `état=-3` à PubSubClient.

**MAIS la causalité n'est pas prouvée**, et cette spec ne doit pas partir du
principe qu'elle l'est. Deux réserves explicites :

- Aucun échec d'allocation n'est journalisé **pendant** la rafale de 06:36–06:37.
  Ce n'est pas une preuve d'absence : l'ADR-0033 throttle ces avertissements, et
  le journal ne montre donc pas tout.
- Le sens de la flèche reste ouvert. La rafale peut **causer** la déconnexion,
  ou les deux peuvent partager une cause amont. Le motif de décroissance
  monotone appuie la première lecture sans l'établir.

C'est le rôle du `debugger` dans ce cycle, avant toute écriture de code.

## Comportement attendu

- Une reconnexion MQTT **ne republie pas** la découverte quand rien n'a changé.
- La découverte **est** republiée quand son contenu a changé (nouvelle version de
  firmware, nouvelle entité, préfixe modifié) — sinon une mise à jour n'apparaît
  jamais dans Home Assistant, ce qui serait une régression pire que le défaut.
- La perte des messages retenus côté courtier reste **récupérable** sans
  redémarrer l'appareil.
- Home Assistant continue de voir les 60 entités après un redémarrage du
  courtier, de HA, ou de l'appareil.

### Cas limites
- **Courtier neuf ou purgé** : aucun message retenu → la découverte doit repartir.
  Comment le détecter sans republier « au cas où » ? C'est l'arbitrage central.
- **Première connexion après un flash** : la version de firmware a changé → rafale
  légitime, et c'est le cas qui a fait passer 54 à 60.
- **Reconnexions en rafale** (le cas observé) : la deuxième et les suivantes ne
  doivent rien republier.
- **Changement de préfixe de topic** : les anciens topics retenus deviennent
  orphelins — les purger ou les laisser ? À trancher.
- **`retain` refusé par le courtier** (ACL) : la republication redevient
  nécessaire à chaque fois. Détectable ?

## Comportement actuel

Republication systématique et inconditionnelle des 60 entités à chaque connexion
réussie. `discoveryPublished` ne sert qu'à éviter une **double** rafale pour une
**même** connexion (`publishDiscovery()` sort en no-op si le drapeau est déjà
levé) — il ne mémorise rien d'une connexion à l'autre.

## Contraintes techniques
- ESP32, offline first
- `mqttTask` dédiée (ADR-0011), cœur 0, priorité 2, pile 8192
- `kMqttPublishBufferBytes = 1536` — chaque trame doit y tenir, refus silencieux
  de PubSubClient au-delà (`bufferSize < 5 + 2 + topic + payload`)
- `setSocketTimeout(2)` : 2 s de gel maximum par tentative (ADR-0010)
- `esp_task_wdt_reset()` autour de la rafale — **ne pas le retirer**
- Toute mémoire d'état entre connexions doit survivre à un redémarrage si elle
  prétend éviter la republication → **NVS**, avec le coût d'écriture que cela
  implique
- Ne pas alourdir la trame : les 4 clés d'hygiène sont HTTP-seules à dessein

## Fichiers potentiellement concernés
- `src/mqtt_manager.cpp` — `:459` (le reset), `publishDiscovery()` `:1948`,
  la lambda `publishConfig` (**:1991** sur HEAD ; la rédaction citait 1987)
- `src/mqtt_manager.h` — état de mémorisation
- `src/constants.h` — clé NVS éventuelle
- `docs/MQTT.md` — contrat de découverte et conditions de republication
- `docs/subsystems/mqtt-manager.md` — la nouvelle règle
- `docs/adr/` — probable

## Périmètre réduit

Le plan initial de l'`architect` prévoyait un bouton « Forcer la republication »
dans l'UI Paramètres, plus un champ `mqtt_force_discovery` sur
`POST /save-config`, pour piloter explicitement `DiscoveryReason::Recovery`
(paramètre `forced` de `discoveryDecide()`). **Retiré par l'utilisateur le
2026-09-20** pour livrer ce lot en **firmware seul**, sans reconstruction du
filesystem LittleFS (`data/` inchangé, pas de rebuild `build_fs.sh`).

Conséquence : `DiscoveryReason::Recovery` et le paramètre `forced` existent
dans `discovery_fingerprint_logic.h`, sont couverts par les tests unitaires
(§4-5 de `test/test_native_discovery_fingerprint/test_discovery_fingerprint.cpp`),
mais **n'ont aucun appelant firmware** — `_discoveryForceRequested` n'est posé
à `true` que par le message de naissance HA (motif `HaRestart`, chemin séparé
qui court-circuite `discoveryDecide()` dans `taskLoop()`), jamais par une route
HTTP. C'est un **point d'extension tracé**, pas du code mort : trois
commentaires y renvoient explicitement — `src/mqtt_manager.cpp:520-521`,
`src/mqtt_manager.cpp:1467-1468`, `src/mqtt_manager.h:188-189` — et la
docstring de `discoveryDecide()` dans `src/discovery_fingerprint_logic.h:148-149`.

C'est directement la cause du livrable **A3 partiel** : sans ce bouton, il
n'existe **aucun** moyen déclenché par l'utilisateur de forcer une
récupération — seul le redémarrage de Home Assistant (détecté via
`homeassistant/status`) déclenche une rafale de rattrapage.

## Livrables — check-list
- [x] D1 — **`debugger`** : causalité **RÉFUTÉE** le 08/09. Verdict et preuve
      ci-dessous. Le reste du lot se justifie sur son seul mérite.
- [x] D2 — Coût réel d'une rafale mesuré et **journalisé à chaque rafale**
      (`bytesOffered`, `maxBloc8Before`/`maxBloc8After` — ADR-0038, jamais la
      mémoire libre). Voir `DiscoveryTally` (`src/discovery_report_logic.h`) et
      la ligne de synthèse enrichie.
- [x] A1 — Empreinte 32 bits FNV-1a (version firmware + préfixe HA + device ID +
      topic de base + serveur + port), persistée en NVS
      (`src/discovery_fingerprint_logic.h`, `DiscoveryRecord`,
      `constants.h:kDiscoveryNvsNamespace/kDiscoveryNvsRecordKey`).
- [x] A2 — Republication conditionnée à `discoveryDecide()` (empreinte,
      1re connexion, rafale précédente incomplète), jamais à la seule
      reconnexion — voir `mqtt_manager.cpp` (`connectInTask`).
- [ ] A3 — **Partiellement couvert.** Le seul chemin de récupération livré est
      le message de naissance HA (`homeassistant/status` == `online`, motif
      `HaRestart`). Un courtier purgé **sans** redémarrage de Home Assistant
      n'a **aucun rattrapage automatique** dans ce lot — le bouton de
      récupération manuelle prévu par l'architecte est hors périmètre, voir
      « Périmètre réduit » ci-dessous.
- [x] A4 — Journalisation : le motif (`DiscoveryReason`) figure dans la ligne de
      synthèse, et le cas nominal (aucune rafale) reste **muet** — aucun appel
      à `publishDiscovery()` n'est fait avec `DiscoveryReason::None`.
- [x] C1 — `docs/MQTT.md`, `docs/subsystems/mqtt-manager.md` mis à jour
      (2026-09-20, `doc-writer`) — condition de republication, abonnement
      `homeassistant/status`, geste manuel pour les orphelins, décompte
      d'entités corrigé (54 → 60).
- [x] C2 — [ADR-0085](../../docs/adr/0085-republication-decouverte-ha-sur-empreinte.md)
      créé (critères 1, 2, 3, 4).
- [x] C3 — `CHANGELOG.md` mis à jour, section `[Unreleased]` (cible 2.53.1) —
      **sans** affirmer que les déconnexions MQTT sont corrigées.
- [x] T1 — Tests : empreinte inchangée → aucune republication ; version bumpée →
      republication ; entité ajoutée → republication ; préfixe changé →
      republication ; reconnexions en rafale → une seule au plus
      (`test/test_native_discovery_fingerprint/`).
- [x] T2 — **Mutation** : mutants sur la condition d'empreinte (§5 du fichier de
      test, persistance absente → republication à chaque boot) couverts dans
      `test_discovery_fingerprint.cpp`.

## Nouvelles signatures / API
- Endpoints HTTP nouveaux : aucun
- Champs WebSocket nouveaux : aucun
- Topics MQTT nouveaux : aucune **publication** nouvelle — **le contrat de
  découverte ne change pas**, seule la *fréquence* de republication change.
  Un **abonnement** nouveau est ajouté côté appareil : `homeassistant/status`
  (message de naissance HA, `mqtt.subscribe(...)` dans `connectInTask()`,
  `src/mqtt_manager.cpp:522`) — observable côté courtier (ACL, logs
  d'abonnement), documenté dans `docs/MQTT.md`.
- Constantes : `kDiscoveryNvsNamespace` / `kDiscoveryNvsRecordKey` (clé NVS de
  l'empreinte), `kDiscoverySummaryLineMax` porté de 160 à 256 o
  (`src/constants.h`)

## Sécurité chimique
- Dosage impliqué : **non**
- Filtration requise : non
- Calibration touchée : non
- Risque identifié : **aucun** — mais attention, `mqtt_manager.cpp` (**:469-481** sur HEAD) republie à
  chaque reconnexion l'alerte `pump_unusable`, **non retenue** et prescrite par
  l'ADR-0034 (pool-chemistry P1). **Cette republication-là doit être préservée** :
  elle n'est pas de la découverte, et la confondre avec elle supprimerait une
  alerte de sécurité.

## ADR attendu ?
- **Probable** : oui
- **Critère(s) candidat(s)** : 1, 3, 4
- **Sujet pressenti** : « La découverte Home Assistant est republiée sur
  changement d'empreinte, pas sur reconnexion » — contrat vis-à-vis d'un système
  tiers, donc verrouillage long terme, avec l'alternative « republier toujours,
  c'est idempotent » à tracer explicitement puisque c'est le choix actuel.

## Éléments visibles (pour doc-writer)

### Endpoints HTTP concernés
- Aucun

### Champs WebSocket
- Aucun

### Topics MQTT
- Aucune publication nouvelle. La **fréquence** de republication des topics
  `.../config` change. Un abonnement nouveau est ajouté :
  `homeassistant/status` (message de naissance HA).

### Documentation à produire / mettre à jour
- `docs/MQTT.md`, `docs/subsystems/mqtt-manager.md`, `CHANGELOG.md`, ADR probable.
  **Pas** de documentation utilisateur : rien de visible à l'écran ne change.

## Notes

Trouvé en instruisant `bug-080` lot C, pas en cherchant un défaut MQTT. La chaîne
de raisonnement mérite d'être conservée, parce qu'elle a corrigé **deux** fausses
pistes en route : l'utilisateur soupçonnait des coupures WiFi (zéro perte
journalisée), puis un lien avec l'usage de l'application (la moitié des
déconnexions ont lieu la nuit). C'est la **décroissance monotone** du délai — 10,
8, 4, 1 seconde — qui a désigné la rafale de découverte, et elle n'était visible
que parce que `feature-109` numérote les rafales.
