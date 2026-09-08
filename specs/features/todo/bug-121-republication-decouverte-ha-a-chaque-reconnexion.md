## Métadonnées
- **ID** : bug-121
- **Slug** : republication-decouverte-ha-a-chaque-reconnexion
- **Type** : bug — republication redondante, suspectée d'entretenir une boucle de déconnexion
- **Statut** : todo
- **Priorité** : **haute** — 13 déconnexions MQTT en 21 h, et la piste réseau est déjà écartée
- **Version cible** : 2.49.0
- **Dernière mise à jour** : 2026-09-08
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

## Le problème

`src/mqtt_manager.cpp:459` remet `discoveryPublished = false` à chaque connexion
réussie, juste avant d'appeler `publishDiscovery()` :

```cpp
discoveryPublished = false;
esp_task_wdt_reset();
publishDiscovery();              // 54 publish — long si CPL lossy
```

Or `publishDiscovery()` publie chaque entité avec **`retain = true`**
(`mqtt_manager.cpp:1991`, `safePublish(configTopic, payload, true)`). Le courtier
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
  la lambda `publishConfig` `:1987`
- `src/mqtt_manager.h` — état de mémorisation
- `src/constants.h` — clé NVS éventuelle
- `docs/MQTT.md` — contrat de découverte et conditions de republication
- `docs/subsystems/mqtt-manager.md` — la nouvelle règle
- `docs/adr/` — probable

## Livrables — check-list
- [ ] D1 — **`debugger` d'abord** : établir ou réfuter la causalité rafale →
      déconnexion. Instrumenter si nécessaire, et rapporter le sens de la flèche
      avec sa preuve. **Aucun code avant ce verdict.**
- [ ] D2 — Mesurer le **coût réel** d'une rafale : octets totaux émis, durée,
      tampons TX consommés, `maxBloc8` avant/après. Les 211 ms journalisés sont
      une durée, pas un coût mémoire.
- [ ] A1 — Empreinte du jeu de découverte (version de firmware + nombre et
      identité des entités + préfixe), persistée pour survivre au redémarrage
- [ ] A2 — Republication conditionnée à l'empreinte, jamais à la reconnexion seule
- [ ] A3 — Chemin de récupération si les messages retenus sont perdus côté
      courtier, sans redémarrage de l'appareil
- [ ] A4 — Journalisation : dire **pourquoi** une rafale a lieu (empreinte
      changée / récupération / première connexion), et rester **muet** quand elle
      est évitée — le silence est le cas nominal
- [ ] C1 — `docs/MQTT.md`, `docs/subsystems/mqtt-manager.md`
- [ ] C2 — ADR si la décision verrouille le contrat de découverte
- [ ] C3 — `CHANGELOG.md`
- [ ] T1 — Tests : empreinte inchangée → aucune republication ; version bumpée →
      republication ; entité ajoutée → republication ; préfixe changé →
      republication ; reconnexions en rafale → une seule au plus
- [ ] T2 — **Mutation** : au moins un mutant sur la condition d'empreinte, et un
      sur la persistance (empreinte non sauvegardée → republication à chaque boot)

## Nouvelles signatures / API
- Endpoints HTTP nouveaux : aucun
- Champs WebSocket nouveaux : aucun
- Topics MQTT nouveaux : aucun — **le contrat de découverte ne change pas**, seule
  la *fréquence* de republication change
- Constantes : clé NVS d'empreinte

## Sécurité chimique
- Dosage impliqué : **non**
- Filtration requise : non
- Calibration touchée : non
- Risque identifié : **aucun** — mais attention, `mqtt_manager.cpp:470` republie à
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
- Aucun nouveau. La **fréquence** de republication des topics `.../config` change.

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
