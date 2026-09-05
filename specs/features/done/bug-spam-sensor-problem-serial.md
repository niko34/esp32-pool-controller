# Bug — Spam `MQTT ph/orp_sensor_problem → ON` (~250 lignes/s) + écritures Serial entrelacées

- **Type** : bug (format minimal `specs/modele.md`) — **investigation requise avant correctif** (cause racine non confirmée)
- **Statut** : done
- **Version cible** : 2.10.0 (livré avec feature-059)
- **Créée** : 2026-07-22 (observation on-target, v2.9.2, ESP32 hors PCB sur USB)
- **Domaine** : MQTT (dédup d'état), logger (concurrence Serial) — aucun chemin de dosage touché
- **Spec liée** : feature-059-disjoncteur-bootloop-history (la corruption de la partition history pourrait être une CONSÉQUENCE de ce spam si `flushToDisk` a martelé `/system.log`)

## Symptôme observé

Capture série de 25 s juste après le premier boot post-effacement de la partition history (2026-07-22, v2.9.2, bench, sondes EZO absentes, RSSI WiFi -88 dBm) :

- **6 286 occurrences** de lignes se terminant par `em → ON` en 25 s (~250 lignes/s, ~70 Ko de flux série). Les deux seules chaînes du firmware finissant ainsi sont `MQTT ph_sensor_problem → ` et `MQTT orp_sensor_problem → ` (mqtt_manager.cpp:865/870). Aucune occurrence `→ OFF` significative.
- Lignes entrelacées/tronquées simultanées : `[WARN] EZO pH : Wire.endTransmis` répété 29× **collé dans une seule ligne** puis `sion err=2 (cmd=RT,25.0)`, et de nombreuses lignes réduites au fragment `err=2 (cmd=R)` — signature d'**écritures Serial concurrentes non sérialisées** entre au moins deux tâches (mqttTask core 0 / loopTask core 1).
- Le phénomène ne s'est **pas reproduit** au boot suivant (60 s de capture propre : 2 logs `sensor_problem` exactement, dédup normal).

## Pourquoi c'est grave

- Le dédup (mqtt_manager.cpp:863-872, membres `_lastPhSensorProblem`/`_lastOrpSensorProblem`, seuls points d'écriture, jamais remis à -1 ailleurs) est censé rendre ce log **impossible à répéter** sans alternance ON/OFF. 6 286 ON sans OFF ⇒ soit corruption mémoire des membres, soit appel concurrent avec course, soit chemin d'appel insoupçonné.
- Si ce spam alimente aussi `_persistBuffer` du logger, `flushToDisk()` écrit dans `/system.log` à très haute cadence → usure/martèlement de la partition history 64 Ko → **suspect n°1 de la corruption littlefs** qui a rendu l'appareil injoignable pendant une semaine (cf. feature-059).
- L'entrelacement Serial révèle que le chemin d'émission console du logger n'est pas sérialisé entre tâches (le mutex protège les buffers, pas nécessairement l'écriture Serial elle-même).

## Hypothèses à investiguer (debugger)

1. **Course inter-cœurs** : la fonction contenant le bloc dédup est-elle appelée à la fois par `mqttTask` et par la loop principale ? Deux exécutions entrelacées avec `_last… = -1` initial ne donnent que quelques doublons, pas 6 286 — chercher un appel en boucle serrée (reconnexion MQTT ? boucle de publication ?).
2. **Flapping réel de l'état** : `phStale`/`phFrozen`/`orpStale`/`orpFrozen` peuvent-ils osciller à chaque itération quand les EZO sont absents (NaN) et que l'heure vient d'être synchronisée (bascule des horodatages) ? — mais il faudrait alors autant de OFF que de ON.
3. **Corruption mémoire** : débordement d'un buffer voisin dans `MQTTManager` écrasant `_last…` en continu (contexte : ADR-0029 auto-guérison heap né de problèmes mémoire réels). Vérifier les membres adjacents dans mqtt_manager.h (:160-161) et les écritures de tableaux/String proches.
4. **Émission Serial non atomique** : identifier le chemin `systemLogger` → Serial (print du préfixe puis du message ?) et le(s) appel(s) de log hors mutex ; expliquer les fragments répétés.
5. **Conditions du bench comme déclencheur** : WiFi à -88 dBm + flapping MQTT connect/disconnect → chemins de republication forcée à la reconnexion (discovery, états retained) appelés en boucle ?

## Correctif attendu (à préciser après investigation)

- Rendre la répétition impossible par construction (dédup sous protection adéquate, et/ou rate-limit de secours sur ce log).
- Sérialiser les écritures console du logger entre tâches (mutex autour de l'émission Serial complète d'une ligne) si l'hypothèse 4 est confirmée.
- Si le spam peut alimenter `/system.log` : garde-fou de débit sur `flushToDisk` (rate-limit ou coalescing) pour protéger la partition 64 Ko — à arbitrer avec l'architect si ça dépasse le périmètre bug.

## Critères d'acceptation

- [x] Cause racine identifiée et documentée dans cette spec (mécanisme expliquant 6 286 ON sans OFF) → rejeu d'une fenêtre d'octets du FIFO UART0, voir Investigation.
- [x] Après correctif : impossible de logguer `sensor_problem → …` plus d'une fois par transition d'état, même sous flapping MQTT/WiFi et EZO absents → démontré DÉJÀ garanti par le code (dédup correct + plafond structurel 10 s) ; aucun changement requis.
- [x] Aucune ligne série entrelacée/tronquée sous logs concurrents → correctif appliqué (émission en un seul `Serial.write`, voir Résolution). L'entrelacement à granularité ligne était réel ; les troncatures intra-chaîne relevaient du rejeu matériel.
- [x] Aucun chemin de dosage touché.
- [x] `pio run` sans warning nouveau, suites natives vertes. *(validé le 2026-07-22 avec feature-059 : SUCCESS 0 warning, 289/289 tests natifs)*

## Résolution (2026-07-22)

- **`src/logger.cpp` (log(), fin de fonction)** : la ligne console est désormais composée localement (`"[NIVEAU] message\r\n"`) puis émise en **un seul** `Serial.write()` atomique au niveau HAL — supprime l'entrelacement inter-tâches à granularité ligne. Aucun autre changement.
- **`src/mqtt_manager.*` : intentionnellement inchangé** — le dédup est démontré correct, le « spam » n'était pas une émission logicielle.
- **Bug latent `refreshTopics()`** (UAF inter-tâches, découvert pendant l'investigation) : spec dédiée ouverte → `specs/features/todo/bug-race-refreshtopics-uaf.md`.
- **Discriminateurs on-target** (ci-dessus) : restent à exécuter à l'occasion pour clore à 100 % (lecture de `/system.log`, historique HA du 2026-07-22).

## Investigation (2026-07-22, agent debugger — analyse statique)

### Verdict : le "spam" n'est PAS une émission logicielle — artefact de rejeu d'octets sur la voie UART0 TX

**Preuve arithmétique décisive** : le fragment `em → ON\r\n` fait exactement **11 octets** en UTF-8
(`→` = 3 octets). 6 286 × 11 o = **69,1 Ko ≈ les "~70 Ko" mesurés**. Le flux était donc le fragment
de 11 octets répété dos-à-dos, PAS des lignes de log complètes (une ligne réelle
`[INFO] MQTT ph_sensor_problem → ON\r\n` ≈ 39 o → 6 286 × 39 = 245 Ko, incompatible avec la capture).
Aucun chemin de code n'émet le fragment isolé `em → ON` : c'est une **fenêtre d'octets du FIFO/flux TX
rejouée en boucle** au niveau matériel/driver, pas 6 286 appels à `systemLogger.info`.

**Plafond de débit prouvé par le code** (chemins d'appel exhaustifs) :
- Les logs de mqtt_manager.cpp:865/870 vivent dans `publishCalibrationStatusInternal()`, appelée
  UNIQUEMENT depuis `publishAllStatesInternal()` (mqtt_manager.cpp:694), elle-même appelée depuis
  2 sites seulement, tous deux dans mqttTask (core 0) : taskLoop:249 (flag `publishStatesRequested`
  posé par loopTask toutes les **10 s**, main.cpp:236 + kMqttPublishIntervalMs) et connectInTask:363
  (reconnexion, rate-limitée **≥ 5 s** par `lastAttempt`/`_reconnectDelay`, mqtt_manager.cpp:275).
  → Plafond théorique ≈ 0,3 appel/s soit **0,6 log/s même avec un dédup totalement cassé** :
  les 251 lignes/s observées dépassent le plafond d'un facteur **> 400**. Impossible logiciellement.
- Même démonstration pour le 2ᵉ symptôme : `EZO pH : Wire.endTransmission err=…` (atlas_ezo.cpp:45)
  est borné à 1 warn / capteur / 5 s (sensors.cpp:199, kPhOrpSensorIntervalMs) → 29 copies collées
  du préfixe tronqué en < 25 s = **impossible en temps réel**. Même mécanisme de rejeu : une fenêtre
  de 33 octets (`[WARN] EZO pH : Wire.endTransmis`) rejouée 29×, puis récupération des pointeurs et
  émission de la suite réelle (`sion err=2 (cmd=RT,25.0)`) une fois — signature typique d'une
  désynchronisation transitoire des pointeurs FIFO TX.

**Hypothèses de la spec, statuées** :
1. *Course inter-cœurs sur le dédup* : ÉCARTÉE. `_lastPhSensorProblem`/`_lastOrpSensorProblem` sont
   lus/écrits exclusivement depuis mqttTask (un seul consommateur), aucun autre point d'écriture.
2. *Flapping stale/frozen* : ÉCARTÉE. EZO absents → `getPh()` = NaN constant → `phProblem = 1`
   constant. Un flapping donnerait de toute façon une alternance ON/OFF, pas 6 286 ON sans OFF.
3. *Corruption mémoire des membres* : ÉCARTÉE comme cause du débit. Voisins mémoire = scalaires
   (`_lastSensorStale/Frozen`, 3 floats) ; `_lastFilterPub[16]` est borné (mqtt_manager.cpp:710-712,
   indices littéraux 0-15) ; `OutboundMsg` utilise `strlcpy` borné. Et même corrompu en continu,
   le dédup ne peut produire que ~0,6 log/s (plafond ci-dessus).
4. *Émission Serial non atomique* : PARTIELLEMENT CONFIRMÉE mais insuffisante. logger.cpp:77-80
   émet chaque ligne en **4 appels `Serial.print` séparés, hors mutex** → deux tâches peuvent
   entrelacer préfixe/message (granularité ligne). En revanche chaque `print` individuel est
   atomique (HAL `UART_MUTEX_LOCK` + écriture bloquante, esp32-hal-uart.c) : les troncatures
   INTRA-chaîne et les répétitions ne peuvent PAS venir de là → rejeu matériel/driver.
5. *Republication forcée à la reconnexion* : ÉCARTÉE comme cause. Reconnexion ≥ 5 s (backoff 5→120 s),
   et le dédup survit aux reconnexions (membres jamais remis à -1). NOTE : bug latent distinct repéré —
   `refreshTopics()` (mqtt_manager.cpp:279→101) réassigne ~70 String depuis mqttTask à CHAQUE tentative
   de connexion pendant que loopTask lit `topics.*` (publishFiltrationState/publishAlert/…) →
   lecture use-after-free possible. Sans rapport avec ce spam ; à traiter dans une spec dédiée.

**Mécanisme "ON sans OFF"** : l'état réel était ON (EZO absents), publié UNE fois par capteur au
1ᵉʳ passage (dédup -1 → 1, fonctionnement NORMAL). La fenêtre rejouée contenait la fin de cette
ligne unique. Aucun OFF n'a jamais été émis car l'état n'a jamais transitionné. Le dédup est correct.

**Déclencheur probable du rejeu** (non tranché, transitoire, bench) : désynchronisation des pointeurs
du FIFO TX UART0 — conditions favorisantes : ESP32 hors PCB (masse/EMI, câblage volant), écrivains
UART0 multi-cœurs (loopTask core 1 + mqttTask core 0 + éventuels ets_printf ROM directs), 1ᵉʳ boot
post-effacement. Non reproductible au boot suivant, cohérent avec un glitch matériel/driver.

**pool_logs.txt** : 9 octets, contenu littéral `Not Found` (réponse HTTP 404 sauvegardée le
2026-07-15 pendant la semaine de panne). Aucun log exploitable.

**Lien avec la corruption history (feature-059)** : IMPROBABLE. Les lignes spammées sont des INFO/WARN
→ jamais de flush immédiat (logger.cpp:72 : flush immédiat sur ERROR/CRITICAL uniquement) ; flush
périodique 10 min ; `_persistBuffer` plafonné à 100 entrées (drop du plus ancien) → au pire ~6 Ko
écrits / 10 min même sous flood réel. Et sous le verdict H1 (rejeu UART), le logger n'a reçu que les
~2 lignes réelles → aucun martèlement disque. Seul scénario d'usure crédible : ERROR répétés
(`MQTT échec, code=…`, mqtt_manager.cpp:370, min 1/5 s en flapping broker) déclenchant chacun un
flush + rotation 12 Ko au-delà de 16 Ko → à vérifier via les discriminateurs ci-dessous.

**Discriminateurs pour clore définitivement** (à faire on-target quand disponible) :
1. Lire `/system.log` sur la partition history : s'il ne contient qu'1-2 lignes `sensor_problem` →
   H1 confirmée (le buffer RAM n'a jamais vu le spam).
2. Historique HA/broker du binary_sensor `ph_sensor_problem` au 2026-07-22 : un seul ON retenu → le
   publish n'a eu lieu qu'une fois → H1 confirmée.

**Correctif minimal recommandé** (sans implémentation ici) :
- `src/logger.cpp` (log(), lignes 76-80) : composer la ligne complète (`"[" + level + "] " + message + "\n"`)
  dans un buffer/String local puis l'émettre en UN SEUL `Serial.write()` — chaque write étant atomique
  au niveau HAL, cela supprime l'entrelacement inter-tâches à granularité ligne (critère d'acceptation #3)
  sans mutex supplémentaire ni risque de blocage.
- `src/mqtt_manager.*` : AUCUN changement au dédup (démontré correct). Optionnel : rate-limit de secours
  jugé inutile (le plafond structurel de 10 s existe déjà).
- Bug latent `refreshTopics()` (UAF topics inter-tâches) : ouvrir une spec dédiée (hors périmètre).
- Cosmétique : commentaire mqtt_manager.cpp:450 dit « kMaxPayloadLen=384 » alors que la constante vaut 128.

## Notes

- Captures de référence : session Claude du 2026-07-22 (spam ~250 lignes/s au 1ᵉʳ boot post-erase, plus jamais reproduit ensuite). Le caractère non-reproductible impose une analyse **statique** du code (chemins d'appel, concurrence) plutôt qu'une reproduction on-target.
- `pool_logs.txt` (racine du repo, non versionné) contient peut-être des logs de la semaine de panne — à examiner pendant l'investigation.
