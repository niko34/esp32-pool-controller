# Bug — La métrique heap du projet inclut l'IRAM 32 bits : toutes les gardes sont aveugles

- **Type** : bug (format minimal `specs/modele.md`) — **cause racine des 3 crashs OOM**
- **Statut** : **en cours** — phase 1 (**MESURE**) livrée en v2.19.0 ; phase 2 (**BASCULE + recalibrage**) en attente des relevés on-target → v2.19.1
- **Version cible** : 2.19.0 (mineure : recalibrage de seuils de sécurité + nouveau champ diagnostic)
- **Créée** : 2026-08-15 (3ᵉ crash en production, v2.18.9 — le post-mortem livré en 2.18.8 a permis le diagnostic)
- **Domaine** : Firmware transverse (`src/heap_ring.*`, `src/oom_guard.*`, `src/main.cpp`,
  `src/ws_manager.cpp`, `src/web_routes_*`, `src/mqtt_manager.cpp`, `src/constants.h`)
  — aucun chemin de dosage modifié, mais **les seuils de sécurité mémoire changent**
- **Specs liées** : bug-oom-accept-http-durcissement (2.18.5), bug-garde-tls-trop-permissive-sans-psram
  (2.18.6), bug-parachute-oom-jamais-rearme-et-postmortem (2.18.8), ADR-0029, ADR-0035, ADR-0036, ADR-0037

## Symptôme (3ᵉ crash, 15/08/2026, uptime 10 h)

`abort()` dans `async_tcp`, chaîne complète décodée grâce au post-mortem :
`esp_littlefs` ne peut pas allouer un descripteur (« Unable to allocate FD ») →
`ESP_LOGE` → `vprintf` → écriture console → **création PARESSEUSE du verrou
UART** (`lock_init_generic`, newlib `locks.c:77`) → `xSemaphoreCreateRecursiveMutex`
échoue → `abort()` direct. C'est le message d'erreur qui tue le contrôleur, pas
la panne qu'il signale. Le parachute OOM est hors-jeu : il ne couvre que les
`new` C++, pas les malloc de newlib/FreeRTOS.

Post-mortem : `échecs=767 sur 36023 s (1er 1622 o, dernier 92 o caps=0x0804),
tas comptabilisé t-2s free=67584 maxB=27584 → verdict SUSPICION CORRUPTION`.
Les 92 o en caps `0x0804` (= `MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT`) sont très
probablement le mutex fatal lui-même.

## Cause racine — et le verdict « corruption » est FAUX

`ESP.getFreeHeap()` = `heap_caps_get_free_size(MALLOC_CAP_INTERNAL)` et
`ESP.getMaxAllocHeap()` = `heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)`
(`framework-arduinoespressif32/cores/esp32/Esp.cpp:163-173`).

**`MALLOC_CAP_INTERNAL` inclut l'IRAM accessible en 32 bits SEULEMENT**, donc
inutilisable pour un `malloc`/`new` ordinaire (qui exige `MALLOC_CAP_8BIT`).
Preuves convergentes :
- le « plus gros bloc » est figé à **27 584 / 27 636 o depuis des jours et à
  travers les redémarrages** — signature d'un reliquat d'IRAM constant, pas d'un
  bloc DRAM vivant ;
- des allocations de 92 à 2 872 o échouent « alors qu'il reste 27 Ko » : il n'y a
  aucune contradiction, **la famine 8 bits est réelle** ;
- l'intégrité du tas est vérifiée OK toutes les 5 min (2.18.8) — pas de corruption.

**Toutes les gardes mémoire du projet mesurent donc le mauvais pool** :
`kRestartMaxAllocHeapBytes` (auto-guérison, 8192 — ne peut PAS se déclencher
puisque l'indicateur ne descend jamais sous ~27 Ko : **ce filet n'a jamais
fonctionné**), `kWsPushMinAllocBytes` (16384), `kLowHeapWarnBytes` (20000),
`kCheckUpdateMinMaxAllocBytes`/`kCheckUpdateMinFreeHeapBytes` (garde TLS),
`kOomRearmMinMaxAllocBytes`/`kOomFallbackRearmMinMaxAllocBytes` (parachute),
l'anneau de cinétique, le post-mortem et son verdict.

## Correctif attendu

1. **Métrique unique et correcte**, centralisée (helper dédié, pas 24 appels
   dispersés) : `heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)`
   et `heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)`.
   Recenser les **24 sites d'appel** de `ESP.getFreeHeap()`/`getMaxAllocHeap()`
   et décider pour chacun (les affichages d'information peuvent garder la valeur
   Arduino, les GARDES doivent toutes basculer).
2. **Recalibrage de TOUS les seuils** sur les valeurs réellement mesurées après
   correction — les valeurs actuelles ont été fixées sur des chiffres faussés,
   y compris celles des cycles 2.18.5 à 2.18.8. Mesurer d'abord, décider ensuite.
3. **Exposer les deux mesures** dans `/get-system-info` (8 bits ET total interne)
   pour rendre l'écart visible et éviter que le piège se referme plus tard.
4. **Réexaminer le verdict du post-mortem** (ADR-0037) une fois la métrique juste :
   la règle « comptabilité saine ⇒ corruption » devient exploitable.
5. **Le crash lui-même** : une allocation de mutex qui échoue dans le chemin de
   log abortit sans recours. Étudier une réduction du risque (créer le verrou
   UART tôt, au boot, pour qu'il ne soit jamais créé sous pression mémoire) —
   à trancher par l'architect, périmètre à borner.

## Hors périmètre (phase 2, spec de suivi)

Identifier **ce qui consomme** la mémoire 8 bits (oscillations ~45 Ko dans
l'anneau, 767 échecs en 10 h). Pistes : chemin de service des fichiers statiques
(`AsyncStaticWebHandler`, présent dans 2 des 3 crashs), `LittleFS.begin(false)`
= **10 fichiers ouverts maximum par défaut**, fuite éventuelle de `File`.
La phase 2 n'a de sens qu'une fois la métrique juste.

## Critères d'acceptation

- [ ] Une seule fonction fournit la mesure « mémoire réellement allouable » ;
      aucune garde ne s'appuie plus sur `ESP.getMaxAllocHeap()`/`getFreeHeap()`
      — **partiel** : le helper unique `heap_metrics` existe et est utilisé par
      l'anneau / le miroir ; les **9 gardes** lisent encore la mesure brute
      (bascule en v2.19.1, cf. découpage ci-dessous)
- [ ] Les seuils sont recalibrés sur des mesures réelles, documentées dans la spec
      — **bloqué** tant que les relevés on-target ne sont pas disponibles
- [x] `/get-system-info` expose les deux valeurs et l'écart est visible dans l'UI
      ou la doc — 3 champs additifs (`free_heap_8bit`, `max_alloc_8bit`,
      `max_alloc_internal`), carte Système « Mémoire utilisable » + note sur la
      mesure brute, trace INFO 5 min donnant les deux familles
- [ ] L'auto-guérison redevient capable de se déclencher (démontré par le calcul
      des seuils vs mesures) — v2.19.1
- [ ] `pio run` core 2 + core 3 verts, `pio test -e native` sans régression (507)
- [ ] Mesures on-target relevées AVANT/APRÈS et consignées dans la spec
      — section « Mesures on-target à relever » ci-dessous, **à remplir par
      l'utilisateur** avant d'engager la v2.19.1

## Notes d'implémentation — v2.19.0 (phase 1 : MESURER)

**Principe directeur** : cette version **n'altère aucune décision de l'appareil**.
Elle rend la vraie grandeur observable, rien de plus. Risque de déploiement nul.

Livré :

1. **`src/heap_metrics.h` / `.cpp`** (nouveau) — source unique :
   `heapFree8Bit()`, `heapLargestFreeBlock8Bit()`, `heapMetricsSnapshot()` sur
   `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT`. Sans log, sans allocation, sans
   `String` (appelable depuis `WsManager::_heapAllowsPush()`, qui ne doit jamais
   journaliser sous peine de récursion). **Interdit** depuis `oomFailedAllocHook`
   et le `new_handler` (réentrance allocateur → deadlock).
2. **Anneau de cinétique + miroir du post-mortem** basculés sur la mesure 8 bits
   (`src/heap_ring.cpp`) — **purement observationnel**, aucune décision n'en
   dépend. Cadence de 2 s **inchangée**.
3. **Bump des deux magics RTC** : anneau `0xB10C4EA9` → `0xB10C4EAA`,
   post-mortem `0xF00DFA11` → `0xF00DFA12`. Une trame 2.18.x relue avec la
   nouvelle grille afficherait un chiffre faux **et crédible** ; perdre la trace
   du boot de bascule est le moindre mal. Budget RTC inchangé (1073 o / 8192).
4. **Libellés du journal suffixés** : `maxB8=` / `free8=` dans le dump de
   l'anneau et dans la ligne de post-mortem → logs auto-descriptifs, un `maxB=`
   signale un firmware ≤ 2.18.9 (métrique surestimée).
5. **Trace INFO de calibrage** toutes les `kHeapLogIntervalMs` (5 min), depuis
   `checkHeapHealth()` :
   `Heap 8bits: libre8=… maxBloc8=… minMaxBloc8=… | interne: libre=… maxBloc=… (octets)`.
   `minMaxBloc8` est mis à jour **à chaque tick** du watchdog heap (60 s, ou 5 s
   sous seuil d'alerte) et pas toutes les 5 min : un creux transitoire entre deux
   traces serait invisible — or c'est lui qui dimensionnera le seuil.
6. **`/get-system-info`** : 3 champs **additifs** (`free_heap_8bit`,
   `max_alloc_8bit`, `max_alloc_internal`). `free_heap` / `heap_size` gardent
   leur sémantique. Budget JSON mesuré (`measureJson`, pires cas) : **716 o sur
   1024** (628 avant).
7. **UI** : carte Système, ligne « Heap libre » → « **Mémoire utilisable** »
   (libre + plus gros bloc, 8 bits) + note discrète sur la mesure brute.
   Repli silencieux sur firmware antérieur (pas d'`undefined`).

**Non livré, volontairement** : aucun seuil de `constants.h` modifié, aucune des
**9 gardes** basculée (auto-guérison, pushes WS, réarmement nominal et de repli
du parachute, gardes HTTP, garde TLS). Basculer sans mesure obligerait à
**deviner** les seuils d'une garde qui **redémarre** un appareil pilotant des
pompes doseuses.

Décision formalisée : [ADR-0038](../../../docs/adr/0038-metrique-memoire-allouable-8-bits.md)
(+ notes additives datées dans ADR-0029, 0035, 0036, 0037).

## Mesures on-target à relever (prérequis à la v2.19.1)

⚠️ **La v2.19.1 ne doit pas être engagée avant que ce tableau soit rempli.**
Source : trace INFO `Heap 8bits: …` (journal système, 1 ligne / 5 min) et
`GET /get-system-info`.

### 1. Régime nominal — ≥ 24 h d'uptime continu

| Mesure | Où la lire | Valeur relevée |
|---|---|---|
| `minMaxBloc8` après ≥ 24 h | trace INFO (champ `minMaxBloc8`) | … |
| `maxBloc8` médian en régime établi | trace INFO (champ `maxBloc8`) | … |
| `libre8` médian | trace INFO (champ `libre8`) | … |
| Écart `maxBloc − maxBloc8` | trace INFO (les deux colonnes) | … |
| Uptime au moment du relevé | `/get-system-info` (`uptime_seconds`) | … |

> `minMaxBloc8` est **la** valeur qui dimensionne le seuil de redémarrage :
> `kRestartMaxAllocHeapBytes = clamp(0,40 × minMaxBloc8 ; 8192 ; 16384)`.

### 2. Régime chargé — pire cas à provoquer volontairement

Relever `maxBloc8` / `libre8` **pendant** chacune de ces sollicitations (la
trace de 5 min peut rater le creux : `minMaxBloc8` le retient, lui) :

| Sollicitation | `maxBloc8` observé | `minMaxBloc8` après |
|---|---|---|
| Chargement complet de l'interface web (cache vidé) | … | … |
| Export CSV de l'historique (`/get-history?range=all`) | … | … |
| Plusieurs clients WebSocket connectés simultanément | … | … |
| Session OTA (`/check-update` ou upload manuel) | … | … |
| Consultation du journal (`/get-logs`) | … | … |

### 3. Contrôles de cohérence avant recalibrage

- [ ] L'invariant d'ordre est satisfaisable avec les valeurs relevées :
      `restart < rearmOom ≤ kWsPushMinAllocBytes < kLowHeapWarnBytes < minMaxBloc8`
- [ ] `minMaxBloc8` **n'est pas** figé à la même valeur d'un boot à l'autre
      (un chiffre constant à travers les redémarrages = signature du piège :
      vérifier qu'on lit bien `maxBloc8` et non `maxBloc`)
- [ ] Aucun `Post-mortem OOM` ni `Allocation heap échouée` dans le journal sur
      la période — sinon consigner taille, `caps` et verdict ici

### 4. Ordre de bascule des gardes (v2.19.1, risque croissant)

1. Pushes WebSocket (`kWsPushMinAllocBytes`) — dégradation bénigne
2. WARN heap (`kLowHeapWarnBytes`) — journal seul
3. Gardes HTTP / TLS (`kCheckUpdateMin*`) — refus réessayable
4. Réarmement du parachute OOM (`kOomRearm*`) — protection
5. **Auto-guérison (`kRestartMaxAllocHeapBytes`) — EN DERNIER**, c'est la seule
   qui redémarre l'appareil

> Gate `pool-chemistry` : **report d'un redémarrage pendant une injection
> REFUSÉ**. Le PWM des pompes est matériel — une boucle figée laisserait la
> pompe **énergisée**. Différer transformerait une dose tronquée (bénigne,
> rattrapée au cycle suivant) en **surdosage potentiel**. Débounce **maintenu
> à 3**.
