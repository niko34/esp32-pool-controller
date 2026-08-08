# ADR-0034 — Migration de plateforme : core Arduino 3.x / ESP-IDF 5.x via pioarduino

- **Statut** : Accepté (phase **code bi-core** exécutée — feature-063). La **bascule production** reste une étape **CONDITIONNELLE** — voir « Porte de bascule production » ci-dessous.
- **Date** : 2026-08-04 (projet DRAFT rédigé le 2026-08-03, étude feature-062)
- **Décideurs** : Nicolas (décision d'exécution de la phase code) — étude feature-062, exécution feature-063
- **Spec(s) liée(s)** : feature-062 (étude, done), feature-063 (exécution code bi-core, v2.15.0)

## Contexte

La plateforme de build actuelle (`espressif32@6.9.0` = core Arduino **2.0.17** / ESP-IDF **4.4.7**) est une lignée **abandonnée** : Espressif ne supporte plus PlatformIO, plus aucun correctif n'arrivera. Trois problèmes concrets en découlent :

1. le **wedge lwIP** (gels réseau — confiné par le watchdog connectivité [ADR-0030](0030-watchdog-connectivite.md), jamais corrigé à la racine) ;
2. le **bug littlefs div/0 amont** sur FS plein ([ADR-0033](0033-budget-espace-partition-history.md)) — la version littlefs est figée par la plateforme ;
3. l'absence de correctifs de sécurité à long terme.

L'étude feature-062 (2026-08-03) a démontré la faisabilité : diff firmware ~20-25 lignes / 3 fichiers (LEDC, watchdog, mbedtls sha256), banc de compilation SUCCESS, 4 questions critiques favorables (littlefs 2.11.2 corrige le div/0 ; format FS lfs 2.1 compatible bi-directionnel ; NVS préservé ; retour arrière sûr en OTA-only). Caveat connu : le banc d'étude avait tourné sur un arbre périmé (pré-fork ESP32Async) — la re-passe sur HEAD était la première tâche du cycle d'exécution feature-063, qui l'a levée (voir Résultats).

## Décision

Le firmware migre vers le core Arduino 3.x / ESP-IDF 5.x en utilisant **pioarduino** comme véhicule (empaquetage communautaire du core officiel Espressif — le code embarqué reste 100 % Espressif ; déjà en production sur le projet écran), **en deux phases** :

1. **Phase code bi-core (EXÉCUTÉE — feature-063, v2.15.0)** : le même code compile sur core 2.0.17 ET core 3.x via des gardes `#if ESP_ARDUINO_VERSION_MAJOR >= 3` localisées (3 familles : LEDC, WDT, mbedtls). Un environnement de banc `[env:esp32dev_core3]` (pioarduino) est ajouté à `platformio.ini`, **BUILD-ONLY** — la production reste buildée et déployée en core 2 (`default_envs = esp32dev`, `deploy.sh` inchangés).
2. **Phase bascule production (CONDITIONNELLE — hors feature-063)** : décision utilisateur séparée, subordonnée à la check-list de porte ci-dessous.

La bascule se fera en **OTA-only** : le bootloader IDF 4.4 existant est conservé, ce qui garantit un retour arrière sûr par simple OTA vers un binaire 2.14.x/2.15.x archivé.

**Plan B documenté** (non retenu en premier, payable plus tard sans surcoût) : « Arduino comme composant ESP-IDF » — voie officielle Espressif, donne l'accès complet au sdkconfig (lwIP notamment). La dette d'API core 2 → 3 est identique dans les deux voies : migrer via pioarduino ne ferme pas ce chemin.

## Résultats réels de la phase code bi-core (feature-063, 2026-08-04)

### Builds

| Mesure | core 2 (`esp32dev`, production) | core 3 (`esp32dev_core3`, banc) |
|---|---|---|
| Verdict | SUCCESS — binaire **identique à la baseline à l'octet près** (delta ±16 octets = artefact `__TIME__`) | SUCCESS |
| Flash | inchangée (baseline 2.14.0) | **1 693 142 o = 92,27 % d'app0** (marge restante **141 866 o**) |
| RAM | inchangée | **69 248 o** (+3,3 Ko vs core 2, conforme à l'étude) |
| Warnings | 0 nouveau | mêmes **6 warnings préexistants**, aucun nouveau |
| Tests natifs | **415/415** (insensibles à la plateforme) | idem |

### Versions résolues au banc (2026-08-04)

- **pioarduino** `55.3.37` (URL `stable` non épinglée au moment du banc — épinglée le 2026-08-04 sur la release taguée `55.03.37`, voir porte de bascule) ;
- **arduino-esp32 core** `3.3.7` ;
- **ESP-IDF** `5.5.0` (littlefs **2.11.2** embarqué — le fix div/0 amont est présent, confirmant Q1 de l'étude).

### Compatibilités prouvées (levée du caveat de l'étude)

- **Fork ESP32Async épinglé** (AsyncTCP `3.4.10` / ESPAsyncWebServer `3.11.2`) : compile en core 3 sur l'arbre ACTUEL — la compatibilité n'est plus déclarative, elle est **prouvée au banc**.
- **ESPAsyncWiFiManager** (alanswx, dépendance Git) : **même sha** résolu dans les deux envs, compile OK.

### Blocant imprévu levé

- **Include transitif `WiFiClient.h` disparu en core 3** : sur core 2, `WiFiClient` arrivait transitivement via `HTTPClient.h` ; sur core 3 (où `WiFiClient` est un alias de `NetworkClient`), la chaîne d'includes ne le fournit plus → include **explicite** ajouté dans [`src/web_routes_ota.cpp`](../../src/web_routes_ota.cpp) (sans effet binaire en core 2). Aucune autre rupture au-delà des 3 familles inventoriées par l'étude.

### Sécurité chimique (double GO pool-chemistry)

- **P1 (pré-design) : GO sous conditions** — 4 conditions, toutes implémentées : (1) fréquence 20 kHz / résolution 8 bits / canaux 0-1 STRICTEMENT identiques ; (2) retour de `ledcAttachChannel()` testé → échec = log **CRITICAL** + pompe déclarée **inutilisable jusqu'au reboot** (`pumpUsable[i] = false`, fail-closed, GPIO tenu LOW) ; (3) garde `pumpUsable` en **première ligne** d'`applyPumpDuty()` + refus dans `setManualPump()` (défense en profondeur) ; (4) recommandation : **alerte MQTT dédiée** sur pompe inutilisable **avant toute bascule production core 3** (reprise dans la porte).
- **P2 (re-validation du diff) : GO.**
- Voir [subsystems/pump-controller.md](../subsystems/pump-controller.md).

## Alternatives considérées

- **Rester sur `espressif32@6.9.0` / core 2.0.17** (rejetée) — lignée abandonnée : le wedge lwIP et le div/0 littlefs ne seront jamais corrigés, aucune mise à jour de sécurité. Le confinement (ADR-0030, ADR-0031/0033) reste des pansements sur une cause racine gelée.
- **Sortir de PlatformIO** (idf.py / Arduino IDE / CLI Arduino) (rejetée) — casse tout l'outillage du projet (build_fs.sh, deploy.sh, tests natifs `pio test -e native`, CI) pour un bénéfice nul par rapport à pioarduino.
- **ESP-IDF pur, sans Arduino** (rejetée) — réécriture massive (toutes les libs Arduino : ESPAsyncWebServer, PubSubClient, DallasTemperature, RTClib…) sans proportion avec le besoin.
- **pioarduino** (retenue) — diff minuscule, outillage PlatformIO conservé, déjà éprouvé sur `esp32-pool-controller-screen`, et plan B « Arduino composant IDF » reste ouvert.

## Conséquences

### Positives

- lwIP nettement plus récent → traitement à la racine du wedge réseau (ADR-0030 devient une deuxième ligne de défense) — effectif à la bascule.
- littlefs 2.11.2 : div/0 amont corrigé → les défenses [ADR-0031](0031-disjoncteur-bootloop-history.md) (disjoncteur boot-loop) et [ADR-0033](0033-budget-espace-partition-history.md) (budget d'espace) deviennent des **défenses dormantes** après bascule (elles restent en place — décision utilisateur du 2026-08-03, notes posées dans les deux ADR).
- Retour des correctifs de sécurité (core maintenu).
- **Acquis dès maintenant** : le code bi-core est mergé et vérifié — le même arbre compile sur les deux plateformes, la bascule ne demandera plus aucun diff firmware.

### Négatives / dette assumée

- **Marge Flash réduite** : **92,27 % d'app0 mesurés** (1 693 142 o, marge 141 866 o) — sous le plafond de 95 % fixé par la spec, mais à surveiller à chaque feature après bascule.
- **ADR-0011 corrigé** (découverte collatérale de l'étude, correctif appliqué par feature-063) : les `-DCONFIG_LWIP_*` (feature-014) n'ont jamais été appliqués — lib lwIP précompilée (16 sockets / 12 retransmissions effectifs, avant comme après). Flags retirés des deux envs, note « effet fantôme » posée sur ADR-0011.
- RAM +3,3 Ko (mesuré : 69 248 o en core 3).
- Directives `#if` bi-core dans 3 fichiers (+1 include explicite) tant que le core 2 reste supporté.
- La plateforme pioarduino du banc pointe l'URL `stable` **non épinglée** : dette acceptée pour un env BUILD-ONLY, **interdite pour la production** (voir porte). **Épinglée le 2026-08-04** sur la release taguée `55.03.37` (item de la porte livré — `stable` avait déjà dérivé vers 55.03.311).

> **Note additive 2026-08-08 — régression TLS sortant sous core 3 sur ESP32 classique (limitation ACCEPTÉE)**
>
> Constatée en fenêtre d'observation core 3 sur l'appareil de production (spec `bug-checkupdate-tls-core3`, v2.16.2) : **`/check-update` et « Installer depuis GitHub » — tout TLS sortant — sont inopérants sous core 3**, y compris sur heap fraîche après reboot, alors que le même code réussissait en core 2.
>
> - **Cause racine démontrée** : l'empreinte statique d'IDF 5 sur ESP32 classique (sans PSRAM) réduit le plus gros bloc heap contigu à **~43-47 Ko dès le boot** — insuffisant pour une session TLS sortante (~50 Ko en pointe : 2×16 Ko de buffers symétriques + structures + X509). Les allocations refusées en plein handshake remontent en erreurs mbedtls déguisées (`X509 - Allocation of memory failed`, voire `Certificate verification failed`). GitHub, les certificats embarqués et la crypto matérielle sont **hors de cause** (validation X509 rejouée sur l'ESP32 : rc=0, flags=0). Le remède upstream, les **buffers TLS dynamiques** (`CONFIG_MBEDTLS_DYNAMIC_BUFFER`), n'est activé que sur la branche master du lib-builder (futur core majeur) — **absent des 3.3.x**, pas de fix logiciel sans libs custom.
> - **Décision (utilisateur, 2026-08-08)** : régression **acceptée**. L'échec est **propre** (500 + log heap/mbedtls détaillé, livrés en v2.16.2), aucun crash ; contournement opérationnel : OTA par **upload manuel** des binaires de release (`/update`, flux déjà utilisé). Le TLS **entrant** n'existe pas sur ce projet (serveur web en HTTP local) — seul le canal GitHub est touché.
> - **Levier documenté** : module **ESP32-DevKitC-VE (WROVER-E, +8 Mo PSRAM)** — la config core 3 place déjà les allocations > 4 Ko en PSRAM (`SPIRAM_USE_MALLOC=y`, seuil 4096) → session TLS restaurée + marge heap pour tout le système. **Prérequis** : libérer les GPIO 16/17 (consommés par la PSRAM sur WROVER) → retrait du code écran UART (feature-065 à venir).
> - Ce point s'ajoute à la pesée de la porte de bascule ci-dessous : tant que le matériel reste un WROOM sans PSRAM, basculer en core 3 = perdre la vérification/installation de mises à jour via GitHub.

> **Note additive 2026-08-08 — GPIO 16/17 libérés (feature-065, v2.17.0)**
>
> Le support écran externe LVGL/UART2 a été **entièrement retiré** (feature-065) : plus aucun périphérique sur les GPIO 16/17 — le **prérequis matériel du levier WROVER-E ci-dessus est levé**. Gain flash collatéral : marge app0 core 3 **92,3 % → 90,9 %** (1 667 566 o) ; core 2 : 80,7 % (1 481 445 o).

### Ce que ça verrouille

- pioarduino devient la plateforme de build cible du contrôleur (alignement avec le projet écran) ; l'accès au sdkconfig complet exige de payer le plan B.
- Bascule et retour arrière **OTA-only** : aucun reflash USB du bootloader — le bootloader IDF 4.4 est conservé, c'est la garantie même du retour arrière.
- Le contrat de compatibilité FS (lfs 2.1) et NVS entre les deux mondes doit être maintenu tant que le retour arrière est une option.
- Les gardes bi-core `#if ESP_ARDUINO_VERSION_MAJOR >= 3` sont le **seul** mécanisme de divergence core 2/core 3 autorisé : pas de fork de fichier, pas de branche longue durée.
- L'env `esp32dev_core3` reste **BUILD-ONLY** (jamais flashé sur la production) tant que la porte n'est pas franchie.

## Porte de bascule production (étape conditionnelle — check-list)

La bascule du contrôleur de production vers core 3 est une **décision utilisateur séparée**, subordonnée à **TOUTES** les conditions suivantes :

- [ ] **Banc matériel L3 100 % vert** sur un 2ᵉ ESP32 (checklist remise à l'utilisateur — voir feature-063 § Notes d'implémentation) : PWM à l'oscilloscope + mesure au verre doseur (écart < 5 %), OTA **bi-directionnel** (le véhicule ET le retour), FS/NVS dans les deux sens, AP secours ESPAsyncWiFiManager explicite, tous sous-systèmes.
- [x] **Épinglage de la plateforme pioarduino sur une release TAGUÉE** (exigence de revue feature-063) : remplacer l'URL `stable` par l'URL d'une release versionnée, et re-vérifier le build avec les versions figées (référence au 2026-08-04 : pioarduino 55.3.37 / core 3.3.7 / IDF 5.5.0). **LIVRÉ (2026-08-04)** : `platformio.ini` pointe la release taguée **`55.03.37`** (= pioarduino 55.3.37 / core 3.3.7 / IDF 5.5.0, versions exactes validées au banc feature-063) ; build de re-vérification SUCCESS (Flash 92,3 % — 1 693 898 o, légère hausse vs la référence car le build inclut feature-064). **Dérive constatée** au moment de l'épinglage : l'URL `stable` pointait déjà **55.03.311** (core 3.3.11) — la dérive n'était pas théorique ; toute montée de version est désormais un choix explicite à re-valider au banc.
- [x] **Alerte MQTT dédiée « pompe inutilisable » OBLIGATOIRE** (exigence pool-chemistry P1) : le CRITICAL local ne suffit pas en production — l'échec d'attache LEDC doit remonter en alerte MQTT avant tout déploiement core 3. **LIVRÉ (feature-064, v2.16.0)** : alerte `pump_unusable` par pompe, re-publiée à chaque (re)connexion MQTT (arbitrage Q1, GO pool-chemistry).
- [ ] **Fenêtre hors saison**, pompes débranchées à la bascule.
- [ ] **Observation 14 jours** post-bascule.
- [ ] **Binaires de retour arrière archivés + sha256** (dernière version core 2 déployée), procédure de retour OTA archivée.

## Références

- Specs : `specs/features/done/feature-062-etude-migration-core3-idf5.md` (étude, inventaire, checklist L3), `specs/features/doing/feature-063-execution-migration-core3.md` (exécution, résultats réels), `specs/features/done/bug-checkupdate-tls-core3.md` (régression TLS sortant — investigation, preuves, décision d'acceptation, note additive 2026-08-08)
- Code : [`src/pump_controller.cpp`](../../src/pump_controller.cpp) / [`.h`](../../src/pump_controller.h) (LEDC bi-core + `pumpUsable`), [`src/main.cpp`](../../src/main.cpp) (WDT `esp_task_wdt_reconfigure`), [`src/ota_integrity.cpp`](../../src/ota_integrity.cpp) (macros compat mbedtls), [`src/web_routes_ota.cpp`](../../src/web_routes_ota.cpp) (include `WiFiClient.h`), [`platformio.ini`](../../platformio.ini) (`[env:esp32dev_core3]`, retrait flags lwIP)
- ADR liés : [ADR-0011](0011-mqtt-task-dediee.md) (flags lwIP fantômes — note corrective), [ADR-0028](0028-migration-fork-esp32async.md) (fork ESP32Async), [ADR-0030](0030-watchdog-connectivite.md) (wedge lwIP), [ADR-0031](0031-disjoncteur-bootloop-history.md) / [ADR-0033](0033-budget-espace-partition-history.md) (défenses conservées — notes)
- Docs : [BUILD.md](../BUILD.md) (les deux envs, pièges pioarduino), [subsystems/pump-controller.md](../subsystems/pump-controller.md) (mise en sécurité `pumpUsable`)
