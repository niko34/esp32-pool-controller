# Feature — Exécution de la migration core 3/IDF 5 : code bi-core + banc validé (bascule production = porte séparée)

## Métadonnées
- **ID** : feature-063
- **Slug** : execution-migration-core3
- **Type** : feature
- **Statut** : done
- **Priorité** : normale
- **Version cible** : 2.15.0 (code bi-core mergé, production TOUJOURS buildée core 2 — zéro changement de comportement livré)
- **Dernière mise à jour** : 2026-08-04
- **Spec(s) liée(s)** : feature-062 (étude — GO technique conditionnel, done/), docs/adr/0034-migration-core3-idf5.md (ex-DRAFT, numéroté/accepté par ce cycle), ADR-0031/0033 (défenses conservées — décision utilisateur du 2026-08-03 : le disjoncteur history N'EST PAS supprimé)

## Domaines touchés
- [ ] Capteurs
- [x] Régulation (pump_controller — LEDC) → **passage obligatoire par `pool-chemistry`**
- [ ] Filtration / Éclairage
- [ ] Web
- [ ] MQTT / HA
- [ ] UART / écran
- [x] OTA (mbedtls 3 — vérification d'intégrité)
- [ ] Authentification
- [ ] Historique / persistance
- [ ] Frontend

## Agents requis
- [x] spec-reader
- [x] architect
- [x] pool-chemistry — **obligatoire** : transposition LEDC des pompes doseuses (pré-validation du design + re-validation du diff)
- [ ] ux-ui-designer
- [x] embedded-developer
- [ ] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description
Rendre le firmware **bi-core** (compile à l'identique sur core 2.0.17 ET core 3.x via gardes `#if ESP_ARDUINO_VERSION_MAJOR >= 3`), ajouter l'environnement de build `esp32dev_core3` (pioarduino), re-dérouler le banc de compilation sur l'arbre ACTUEL avec notre fork ESP32Async épinglé, et valider. La production continue d'être buildée et déployée en core 2 (`default_envs` inchangé, deploy.sh inchangé) — **la bascule de production est HORS de ce cycle** (porte séparée, décision utilisateur, fenêtre hors saison recommandée).

## Contexte
Étude feature-062 : GO technique conditionnel. Diff attendu ~20-25 lignes / 3 familles (LEDC, WDT, mbedtls). Conditions à lever DANS CE CYCLE : (1) banc sur HEAD avec le vrai fork ESP32Async 3.4.10/3.11.2 (le banc d'étude avait tourné sur un arbre périmé pré-fork — compat déclarée, pas prouvée) ; (2) mesure de taille Flash réelle sur l'arbre actuel (~92 % d'app0 attendu — GO seulement si ≤ 95 % avec justification de marge) ; (3) validation pool-chemistry de la transposition LEDC. Conditions HORS cycle (porte de bascule) : banc matériel 2ᵉ ESP32 (checklist L3 de l'étude), fenêtre hors saison, 14 j d'observation, binaires 2.14.x archivés.

## Comportement attendu
1. **Code bi-core** : les 3 familles portées sous gardes de préprocesseur — LEDC (`ledcAttachChannel`/`ledcWriteChannel`, canaux 0/1 explicites, 20 kHz/8 bits STRICTEMENT identiques, retour bool testé → log CRITICAL + inhibition dosage si échec d'attache, à arbitrer pool-chemistry), WDT (`esp_task_wdt_config_t` + `esp_task_wdt_reconfigure` — le core 3 initialise déjà le TWDT), mbedtls sha256 (macros de compat sur `MBEDTLS_VERSION_NUMBER`). Réutiliser/consolider les prototypes du worktree d'étude (`.claude/worktrees/agent-af3208fa3eac7de23`) SANS reprendre son platformio.ini périmé.
2. **platformio.ini** : nouvel `[env:esp32dev_core3]` (platform pioarduino, MÊMES lib_deps épinglées ESP32Async 3.4.10/3.11.2 + alanswx git, mêmes build_flags utiles) ; `default_envs = esp32dev` INCHANGÉ ; retrait des `-DCONFIG_LWIP_MAX_SOCKETS`/`-DCONFIG_LWIP_TCP_MAXRTX` des DEUX envs (prouvés inopérants — correctif documentaire ADR-0011 associé).
3. **Validation de ce cycle** : `pio run` (core 2) SUCCESS 0 warning ET `pio run -e esp32dev_core3` SUCCESS (via le pio du penv, `pio pkg install` préalable — pièges documentés) ; tailles Flash/RAM des deux builds rapportées ; `pio test -e native` 415/415 ; `./build_fs.sh` inchangé.
4. **Rien ne change pour la production** : le binaire déployé par deploy.sh reste core 2, comportement identique octet pour octet hors gardes inactives.

### Cas limites
- Fork ESP32Async 3.4.10/3.11.2 incompatible core 3 à la compilation → tenter les versions immédiatement supérieures compatibles (noter le changement, il devra être re-épinglé et re-validé) ; si blocant dur → rapport et STOP (le cycle livre quand même le bi-core compilable core 2).
- Flash core 3 > 95 % d'app0 → signaler, le GO de bascule future en dépendra (pistes : -Os déjà actif, audit des composants).
- ESPAsyncWiFiManager (alanswx) : à re-prouver sur l'arbre actuel.

## Contraintes techniques
- Gardes bi-core LISIBLES et localisées (pas de #if dispersés — regrouper dans des helpers/en-têtes si l'architect le préfère).
- Le comportement core 2 doit être PROUVÉ inchangé (mêmes binaires hors nouvelles gardes : revue du préprocesseur + tailles quasi identiques).
- LEDC : pool-chemistry valide le design AVANT implémentation et le diff APRÈS (double passage, comme feature-060).
- Aucun flash sur le contrôleur de production dans ce cycle. Le flash d'un éventuel 2ᵉ ESP32 de banc est hors périmètre (checklist L3 remise à l'utilisateur).

## Fichiers potentiellement concernés
- `src/pump_controller.cpp` (LEDC bi-core), `src/main.cpp` (WDT bi-core), `src/ota_integrity.cpp` (mbedtls bi-core)
- `platformio.ini` (env core3, retrait flags lwIP)
- `docs/adr/` : numéroter le DRAFT en **ADR-0034** (accepté pour la phase code ; la bascule production y est décrite comme étape conditionnelle) ; note corrective **ADR-0011** (flags lwIP fantômes) ; notes ADR-0031/0033 (défenses conservées, déclencheur amont corrigé en IDF 5)
- `docs/BUILD.md` (les deux envs, pièges pioarduino), `CHANGELOG.md` [2.15.0]

## Livrables — check-list
- [x] A1 — Code bi-core LEDC (pump_controller) — design pré-validé pool-chemistry
- [x] A2 — Code bi-core WDT (main.cpp)
- [x] A3 — Code bi-core mbedtls sha256 (ota_integrity)
- [x] A4 — platformio.ini : env esp32dev_core3 (fork ESP32Async réel épinglé) + retrait flags lwIP inopérants des deux envs
- [x] P1 — pool-chemistry : pré-validation design LEDC (GO sous conditions — 4/4 implémentées)
- [x] P2 — pool-chemistry : re-validation du diff (GO)
- [x] D1 — Doubles builds SUCCESS (core 2 : binaire baseline à l'octet près ; core 3 : mêmes 6 warnings préexistants), tailles rapportées, 415/415 natifs, build_fs OK
- [x] C1 — ADR-0034 numéroté/accepté + correctif ADR-0011 + notes ADR-0031/0033 + BUILD.md + CHANGELOG [2.15.0]
- [x] C2 — Checklist de banc matériel (L3 de l'étude) remise en l'état à l'utilisateur avec la marche à suivre pour son 2ᵉ ESP32 (voir « Notes d'implémentation » ci-dessous)

## Sécurité chimique (obligatoire)
- Dosage impliqué : OUI — le PWM des deux pompes doseuses change d'API sous core 3. En core 2 (production), STRICTEMENT identique.
- Exigences : fréquence/résolution/canaux identiques, retour d'erreur d'attache traité (comportement à arbitrer par pool-chemistry : échec d'attache = pompe inutilisable → quelle mise en sécurité ?), aucune autre garde touchée.

## ADR attendu ?
- **Oui** : numérotation du DRAFT en ADR-0034 (décision de migration par étapes, bascule OTA-only, conditions de porte) + correctif documentaire ADR-0011.

## Critères d'acceptation
- [x] `pio run` (core 2) : binaire au comportement identique (0 warning nouveau, tailles ±1 %). *(Dépassé : binaire identique à la baseline à l'octet près, delta ±16 o = artefact `__TIME__`)*
- [x] `pio run -e esp32dev_core3` : SUCCESS avec le fork ESP32Async épinglé réel — ou blocant documenté avec version alternative épinglée et justifiée. *(SUCCESS avec le fork 3.4.10/3.11.2 tel quel ; un blocant imprévu levé : include `WiFiClient.h` explicite)*
- [x] Taille Flash core 3 rapportée et ≤ 95 % d'app0 (sinon signalée comme condition de bascule non remplie). *(92,27 % — marge 141 866 o)*
- [x] Double GO pool-chemistry (design + diff). *(P1 GO sous conditions 4/4 implémentées, P2 GO)*
- [x] 415/415 natifs ; production non flashée ; `default_envs` inchangé.
- [x] ADR-0034 en place ; l'utilisateur dispose de la checklist de banc matériel pour la suite.

## Plan de test manuel (ce cycle)
1. `pio run` puis `pio run -e esp32dev_core3` : les deux SUCCESS.
2. Vérifier `default_envs` et un `./deploy.sh firmware` À BLANC (build seul) → produit bien le binaire core 2.

## Hors périmètre
- La bascule du contrôleur de production (porte séparée : décision utilisateur, hors saison, pompes débranchées, 14 j d'observation).
- Le flash du 2ᵉ ESP32 de banc (checklist fournie, exécution par l'utilisateur ou cycle ultérieur).
- Toute optimisation de taille Flash au-delà du constat.

---

## Notes d'implémentation (clôture 2026-08-04)

### Résultats réels

| Mesure | core 2 (`esp32dev`, production) | core 3 (`esp32dev_core3`, banc) |
|---|---|---|
| Build | SUCCESS — binaire **identique à la baseline à l'octet près** (delta ±16 o = artefact `__TIME__`) | SUCCESS |
| Flash | inchangée (baseline 2.14.0) | **1 693 142 o = 92,27 % d'app0** (marge 141 866 o) |
| RAM | inchangée | **69 248 o** |
| Warnings | 0 nouveau | mêmes **6 warnings préexistants**, 0 nouveau |
| Tests natifs | 415/415 | 415/415 (env `native`, insensible) |

- **Versions résolues au banc (2026-08-04)** : pioarduino **55.3.37**, core arduino-esp32 **3.3.7**, IDF **5.5.0** (URL `stable` non épinglée — **épinglage sur release taguée requis avant bascule**, exigence de revue).
- **Fork ESP32Async 3.4.10/3.11.2 épinglé : compatibilité core 3 PROUVÉE** sur l'arbre actuel (levée du caveat feature-062) ; **ESPAsyncWiFiManager** : même sha résolu dans les deux envs, OK.
- **Verdicts** : P1 pool-chemistry **GO sous conditions** (20 kHz/8 bits/canaux identiques ; retour d'attache testé → CRITICAL + inhibition fail-closed ; garde en 1ʳᵉ ligne d'`applyPumpDuty` + `setManualPump` ; alerte MQTT recommandée avant prod) — **4/4 implémentées** ; P2 pool-chemistry **GO** sur le diff ; D1 double vérification builds OK ; revue **Approved**.
- **Blocant imprévu levé** : include transitif `WiFiClient.h` disparu en core 3 (`WiFiClient` = alias de `NetworkClient`, plus fourni via `HTTPClient.h`) → include explicite ajouté dans `src/web_routes_ota.cpp` (sans effet binaire core 2).
- **Mineure de revue acceptée** : le log throttlé de refus `setManualPump` sur pompe inutilisable utilise un flag `static` **partagé entre les deux pompes** (une seule trace par boot même si les deux pompes sont inutilisables) — acceptable : le CRITICAL de `begin()` est, lui, émis par pompe.
- **Fichiers modifiés** : `src/pump_controller.{h,cpp}` (LEDC bi-core + `pumpUsable`), `src/main.cpp` (WDT `esp_task_wdt_reconfigure`), `src/ota_integrity.cpp` (macros compat mbedtls), `src/web_routes_ota.cpp` (include `WiFiClient.h`), `platformio.ini` (env `esp32dev_core3`, retrait flags lwIP fantômes), `src/version.h` (2.15.0). Docs : ADR-0034 (nouveau, ex-DRAFT), notes ADR-0011/0031/0033, `docs/BUILD.md`, `docs/subsystems/pump-controller.md`, `CHANGELOG.md`.

### Checklist de banc matériel (2ᵉ ESP32) — marche à suivre pour l'utilisateur

> ⚠️ **Exécution HORS de ce cycle** (par l'utilisateur ou un cycle ultérieur), sur un **2ᵉ ESP32 hors piscine** (config AP/WiFi de test, **jamais de vraies pompes en eau** — PWM à vide ou LED témoin). C'est le **préalable à la porte de bascule production** ([ADR-0034](../../../docs/adr/0034-migration-core3-idf5.md)) : rien ne doit être flashé sur le contrôleur de production tant que cette checklist n'est pas 100 % verte. Reprise du plan L3 de [feature-062](../done/feature-062-etude-migration-core3-idf5.md).

1. **Préparation**
   - [ ] Builder le banc : `~/.platformio/penv/bin/pio pkg install -e esp32dev_core3` puis `pio run -e esp32dev_core3` (voir [BUILD.md](../../../docs/BUILD.md)).
   - [ ] Épingler la plateforme pioarduino sur une **release taguée** (référence : 55.3.37 / core 3.3.7 / IDF 5.5.0) et re-builder.
   - [ ] Flasher le 2ᵉ ESP32 en USB (firmware core 3 + FS `./build_fs.sh`), archiver les binaires + **sha256**.
2. **Sous-systèmes (tous à vérifier)**
   - [ ] Pompes **PWM à vide** : signal 20 kHz / duty 0-255 à l'**oscilloscope**, puis **mesure au verre doseur** (eau claire) — écart **< 5 %** vs core 2 (critère chimique pool-chemistry).
   - [ ] Sondes I2C (EZO pH/ORP), OneWire/DS18B20, RTC DS3231.
   - [ ] WiFi + **AP secours ESPAsyncWiFiManager testé explicitement** (oublier le réseau, vérifier le portail).
   - [ ] MQTT/HA (auto-discovery, commandes, alertes), WebSocket/UI, écran UART (si dispo).
   - [ ] LittleFS : partition `spiffs` ET partition `history` — **relire un FS écrit en core 2 puis l'inverse** (compat lfs 2.1 bi-directionnelle).
   - [ ] NVS : config, compteurs journaliers, `hist_meta` préservés après flash IDF 4.4 → 5.
   - [ ] Disjoncteur history, auto-guérison heap, watchdog connectivité, heap ring (RAM RTC noinit).
3. **OTA bi-directionnel (critique — garantie de retour arrière)**
   - [ ] OTA core 2 → core 3 (le véhicule de bascule).
   - [ ] OTA core 3 → binaire core 2 archivé (**le retour**) — FS/NVS intacts après retour.
4. **Avant la bascule production** (le moment venu)
   - [ ] Alerte MQTT « pompe inutilisable » implémentée (exigence P1).
   - [ ] Fenêtre hors saison, pompes débranchées, observation 14 j, procédure de retour archivée.

## Résultats des essais on-target en production (2026-08-05/06, appareil réel, v2.16.1)

Essais menés par l'utilisateur directement sur le contrôleur de production
(pas de 2ᵉ ESP32), pompes doseuses débranchées électriquement pendant les
premiers boots core 3, tout par OTA (aucun flash USB — bootloader IDF 4.4
conservé, retour arrière garanti).

- ✅ **Référence** : v2.16.1 core 2 en production > 24 h sans anomalie avant essai.
- ✅ **OTA bi-directionnel PROUVÉ** : core 2 → core 3 → core 2 → core 3 via la
  page web, config/historique intacts à chaque transition. Identification du
  core au sketch_size (Infos système) : ~1 481 Ko (82 %) = core 2,
  ~1 657 Ko (92 %) = core 3.
- ✅ **PWM au multimètre** (substitut oscilloscope, pompes débranchées) :
  GPIO25 et GPIO33 — 0 V repos, 3,3 V à 100 % (bouton Test), 1,65 V à 50 %
  (injection manuelle, puissance max configurée 50 %). Échelle du duty core 3
  conforme.
- ✅ **Verre doseur** (aspiration eau claire) : injection manuelle de 100 mL
  demandés → 100 mL mesurés. La calibration mL/min datant du core 2, l'écart
  ≈ 0 % vaut test A/B implicite core 2 vs core 3 (critère < 5 % largement tenu).
- ✅ Aucune alerte `pump_unusable` (feature-064) sur tous les boots core 3 —
  comportement attendu (attaches LEDC saines) ; le déclenchement volontaire de
  l'alerte (sabotage d'un canal en debug) reste non testé, non bloquant.
- ✅ **Périphériques vérifiés sous core 3** (2026-08-06) : sondes I2C Atlas EZO
  (pH/ORP), OneWire DS18B20 (température), RTC DS3231, MQTT (connexion +
  publications).
- ⚠️ **Anomalie ouverte (non bloquante, peut-être antérieure au core 3)** :
  pas de données d'historique visibles dans les écrans pH et ORP. À
  diagnostiquer après la bascule. Pistes : invalidation feature-061 (mesures
  exclues de l'historique quand l'eau ne circule pas — beaucoup de temps
  filtration coupée/pompes débranchées pendant les essais), reboots OTA
  multiples (buffer brut RAM non flushé), ou état de la partition history.
- ℹ️ Constat annexe (pas un bug) : la puissance max des pompes ne s'applique
  pas au bouton Test (toujours 100 %, voulu — commentaire « test bench ») ;
  elle s'applique à l'injection manuelle/auto/programmée. Piège UX identifié :
  les curseurs de cette carte n'ont PAS d'autosave (bouton Enregistrer requis).
- ⏳ **En cours** : fenêtre d'observation (14 j) démarrée le 2026-08-06,
  régulation en service sous core 3. Critères de retour : boot-loop, dérive
  de dosage, corruption FS/NVS, instabilité réseau. Retour = OTA du binaire
  `builds/essais-core3/firmware-2.16.1-core2-REFERENCE.bin`.

⚠️ Tant que l'appareil tourne core 3 : NE PAS utiliser `deploy.sh ota-firmware`
(il compilerait et enverrait l'env core 2 par défaut). La bascule de
l'OUTILLAGE (`default_envs`, deploy.sh) reste à décider après l'observation.
