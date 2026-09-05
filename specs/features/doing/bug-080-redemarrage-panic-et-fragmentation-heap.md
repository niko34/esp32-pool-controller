# bug-080 — Redémarrage sur PANIC et effondrement du plus gros bloc allouable

## Métadonnées

| Champ | Valeur |
|---|---|
| Type | **bug critique** — crash firmware en production |
| Statut | doing — **lot A livré (v2.33.0, 02/09)** ; **lot B-1 livré (v2.35.0, 05/09)** : bascule PSRAM > 4095 o + réserve du parachute en RAM interne explicite (cf. « Notes d'implémentation B-1 ») ; **B-2 déployé le 05/09 (v2.40.0)** ; **B-3 déployé le 05/09 (v2.41.0)** — les trois lots du B sont livrés ; reste le **lot C** (cause de la fragmentation), non ouvert ; compteur « 7 jours sans redémarrage » **toujours ouvert** (repart du déploiement de la 2.35.0) |
| Priorité | **Haute** — un crash non expliqué sur un appareil qui pilote des pompes doseuses |
| Découvert | 29/08/2026, incidemment, en diagnostiquant un affichage |
| Version en cours | **2.35.0** (lot B-1) — la mention « 2.26.0 » était périmée depuis le lot A (2.33.0, 02/09) |

## Domaines touchés

- Diagnostic mémoire — `src/heap_ring.*`, `src/oom_guard.*`
- Potentiellement `src/ws_manager.cpp` (fragmentation par les trames)
- `docs/subsystems/oom-guard.md`

## Sécurité chimique

**Indirecte mais réelle.** Un redémarrage inopiné interrompt une session de
dosage. Les compteurs journaliers survivent (NVS), mais l'intégrale pH **n'est
pas persistée** (choix délibéré, ADR-0052) : un crash remet le rattrapage à zéro
et rallonge d'autant la convergence. Un crash **en cours d'injection** laisse la
pompe s'arrêter par coupure d'alimentation du MOSFET — sans danger immédiat, mais
sans traçabilité du volume réellement injecté.

## Constat

Deux observations distinctes, faites le 29/08 sur l'appareil de production.

### 1. Le dernier redémarrage était un PANIC

`reset_reason = PANIC` relevé dans la trame WebSocket, uptime ~12,8 h à ce
moment. **La trace du crash n'a pas pu être récupérée** : le journal système
téléchargé ne remontait qu'à 16 h 10, soit bien après le redémarrage.

### 2. Le plus gros bloc allouable s'est effondré, et n'est jamais remonté

Journal du 29/08, métrique 8 bits (celle qui est juste — cf. la cause racine du
15/08, la métrique historique mesurait `MALLOC_CAP_INTERNAL`, IRAM comprise) :

```
18:10   libre8 = 69 328   maxBloc8 = 36 852   minBoot = 36 852
18:40   libre8 = 68 224   maxBloc8 = 21 492   minBoot = 21 492   ← −42 %
19:10   libre8 = 68 512   maxBloc8 = 21 492
...
21:40   libre8 = 68 496   maxBloc8 = 21 492   ← jamais remonté
```

**Le total libre ne bouge pas** (≈ 68 Ko) : ce n'est donc pas une fuite, c'est de
la **fragmentation**. Un événement survenu entre 18 h 10 et 18 h 40 a coupé en
deux le plus grand bloc contigu, définitivement.

## Ce qui rend ce constat sérieux

Le projet a un historique d'incidents mémoire documenté : deux crashs OOM en
août, dont un `bad_alloc` à l'accept HTTP, et un parachute OOM qui ne se réarmait
jamais parce que son seuil (32 Ko) dépassait le plus gros bloc réel (28-30 Ko).

**À 21 492 octets, le plus gros bloc est désormais SOUS ce seuil historique de
28-30 Ko.** La question à instruire en premier : les gardes heap et le parachute
OOM sont-ils encore opérants à ce niveau de fragmentation, ou sont-ils redevenus
aveugles comme en août ?

## Pistes d'investigation

1. **Récupérer la trace** : `esp-coredump` (procédure validée le 11/08, gdb de la
   toolchain xtensa-esp32) ; vérifier si une partition coredump existe et si elle
   contient l'événement.
2. **Le post-mortem RTC** livré en v2.18.8 (post-mortem d'allocation en mémoire
   RTC, survivant au reboot) : a-t-il capturé quelque chose ?
3. **Corréler 18 h 10 – 18 h 40** avec le journal complet si une copie antérieure
   existe, et avec l'activité (OTA ? client WS ? téléchargement de journal ?
   export d'historique ?).
4. **Le ring heap** (`heap_ring.cpp`) enregistre l'historique avec le
   `esp_reset_reason` brut — l'exploiter.
5. **Suspects structurels** : les trames WebSocket (2 552 o pire cas, allouées à
   chaque push toutes les 5 s), les réponses chunked de `/download-logs` et
   `/get-history`, la pile TLS de la vérification de mise à jour.

## Critères de clôture

- Cause du PANIC établie, ou démonstration qu'elle n'est pas reproductible avec
  les moyens disponibles (et alors : quelle instrumentation ajouter).
- Origine de la fragmentation identifiée et, si possible, corrigée.
- **Vérification que les gardes heap et le parachute OOM sont opérants à
  maxBloc8 ≈ 21 Ko** — c'est le point le plus important, indépendamment de la
  cause du crash.
- 7 jours sans redémarrage inopiné.

## Contexte

Flash à **97,2 %**, marge 51,0 Ko : toute correction devra être économe. La piste
de dégraissage identifiée en v2.24.0 (pile TLS) reste ouverte et pourrait servir
deux fois — libérer du flash **et** réduire la pression mémoire.
*(Mise à jour 31/08 : flash à 98,2 %, marge ~33 Ko après les v2.27-2.32 — la
contrainte s'est resserrée.)*

---

## DIAGNOSTIC — 31/08/2026 (debugger, coredump décodé)

Pièces archivées : `builds/2.26.0/esp32dev_core3/coredump_panic_20260829.bin`
et `..._decoded.txt` (trace complète), ELF concordant `firmware_20260829_065630.elf`
(concordance établie par le SHA embarqué dans le dump — OTA ~06h57, panic
2-3 min après, boot PANIC ~07h00, uptime 12,8 h au relevé du soir : cohérent).

### 1. Cause du PANIC — établie

Tâche `async_tcp`, `abort()` sur core 1 :

```
abort() ← lock_init_generic (newlib locks.c:77) ← __retarget_lock_init_recursive
← __sfp ← fopen ← VFSFileImpl ← LittleFS ← AsyncWebServerRequest::send(fs, "text/html")
```

Le service d'une **page HTML statique** a demandé à la libc de créer un verrou
interne (`xQueueCreateMutex`, ~90 o) ; le malloc a échoué (famine 8 bits totale
et transitoire — le `minBoot = 1268` du 31/08 prouve que ces creux quasi-zéro
existent en régime normal) ; `lock_init_generic` aborte sans recours.

**Découverte structurelle : ce crash passe par `malloc` (chemin C), pas
`operator new`. Le parachute OOM (`std::set_new_handler`) est INOPÉRANT par
construction sur toute cette classe de crashs.**

### 2. Fragmentation 36 852 → 21 492 — mécanisme cerné, coupable non désigné

`free8` quasi constant ⇒ un PETIT bloc persistant posé à l'offset 15 360 du
grand bloc, pendant la vie d'une allocation transitoire déterministe (~15,3 Ko
en bas de bloc), lors d'une session UI (29/08 : 18h10-18h40 ; 31/08 : le matin).
**Reproductible inter-versions à l'octet près (21 492 identique sur 2.26.0 et
2.32.0)** ⇒ structurel, pas événementiel. Aucun `xTaskCreate`/tampon de 15 360
dans `src/` : l'analyse statique ne suffit pas, instrumentation requise
(cf. correctif 3).

### 3. Verdict sur les gardes — LE point critique

- **Les 4 gardes heap sont AVEUGLES** : auto-heal (`kRestartMaxAllocHeapBytes`
  8192), WARN (`kLowHeapWarnBytes` 20000), skip push WS (`kWsPushMinAllocBytes`
  16384, `ws_manager.cpp:165`), garde `/get-logs` (`web_routes_data.cpp:584`)
  mesurent la métrique BRUTE (IRAM comprise, plancher ~27 636) — même à
  maxBloc8 = 1 268, la brute affiche 27 636 : **aucune ne déclenchera jamais**.
  Récidive de la cause racine du 15/08. Le PANIC a été totalement silencieux.
- **Le parachute ne se réarme plus nominalement** : `kOomRearmMinMaxAllocBytes`
  24 576 > maxBloc8 réel 21 492 ⇒ repli 1 024 o définitif après la première
  famine. Récidive exacte du défaut du 13/08 (constants.h:303).

### 4. Post-mortem RTC du 29/08 — capturé puis perdu

`oomFailedAllocHook` a bien capturé l'échec et l'a dumpé au boot de 07h00,
mais la **rotation du journal 8 Ko (~30 min)** l'a détruit avant le
téléchargement de 16h10. Irrécupérable — le coredump a suppléé.

### Correctifs proposés (NON appliqués — cycle v2.33.0 à lancer)

1. `kOomRearmMinMaxAllocBytes` 24 576 → 16 384 (réarmable à 21 492, reste
   > réserve 12 288) — une constante.
2. Basculer les 4 gardes aveugles sur `heapLargestFreeBlock8Bit()`, seuils
   recalibrés sur `minFen` — **gate `pool-chemistry` obligatoire** (l'auto-heal
   redémarre l'appareil).
3. **[SUPERSEDED par le lot B-2 — conservé, NON réfuté]** Instrumentation
   fragmentation : sur chute de `maxBloc8` > 8 Ko entre deux
   échantillons du ring (`heap_ring.cpp`), dumper `heap_caps_print_heap_info` /
   walker pour désigner le bloc fautif.
   → *Remplacé par une instrumentation plus directe : la sonde du plus grand
   bloc **dans** le hook d'échec (mesure au moment exact du refus, pas 2 s
   avant), bornée à un échantillon par épisode et jamais depuis une ISR. Le
   besoin reste entier, la forme change.*
4. **[RÉFUTÉ — conservé pour mémoire, NE PAS IMPLÉMENTER]** À étudier :
   pré-créer les verrous stdio newlib au setup (ferme
   définitivement le chemin d'abort du 29/08).
   → *Réfutation (04/09, revérifiée) : `fclose` **détruit** le mutex créé par
   `fopen` (`__retarget_lock_close_recursive` → `vSemaphoreDelete`), et `__sfp`
   **ré-initialise** le verrou à chaque attribution, y compris sur un `FILE`
   recyclé. Les ~90 o sont payés à **chaque** `fopen`, indéfiniment. Une chauffe
   ne pré-alloue que le bloc de glue de **428 o** (`_malloc_r(0x1AC)`) : gain
   marginal. Détail complet dans la section « Deux correctifs de la liste sont
   RÉFUTÉS » ci-dessous.*

### Critères de clôture — état

- [x] Cause du PANIC établie (coredump décodé, trace archivée) — **deux
      occurrences, même trace** (29/08 et 03/09)
- [x] **Déclencheur commun identifié et corrigé** : le tampon stdio de 4096 o
      attaché à chaque fichier LittleFS ouvert, laissé en RAM interne par un
      seuil IDF inclusif — **lot B-1 livré en v2.35.0** (bascule PSRAM à 4095).
      ⚠️ **Correction déployée sous condition de vérification** : la prédiction
      falsifiable ci-dessous doit être confirmée sur 48 h avant de considérer
      l'hypothèse comme établie.
- [ ] Origine de la **fragmentation** (36 852 → 21 492) : mécanisme cerné,
      coupable à désigner par l'instrumentation — **lot B-2** (sonde du plus
      grand bloc **dans** le hook d'échec ; le correctif 3 d'origine est
      superseded, voir ci-dessus)
- [x] Gardes opérantes à maxBloc8 ≈ 21 Ko : **correctifs 1-2 livrés en v2.33.0**
      (lot A, voir ci-dessous)
- [x] **Le parachute OOM est prouvé armé en RAM interne** — invariant observé
      (`esp_ptr_internal`), journalisé à l'armement et au réarmement, publié
      dans `/get-system-info`, et alerté s'il n'y a **aucune** réserve depuis
      5 min (lot B-1). *Ce critère n'existait pas au départ : il a été ouvert
      par la découverte du 04/09.*
- [ ] **Ceinture** : garde 503 dans le filtre `serveStatic`, **avant** le
      premier `fopen` — **lot B-3**, à calibrer sur le régime post-B-1
- [ ] **7 jours sans redémarrage inopiné** — **compteur toujours ouvert**, il
      repart du déploiement de la 2.35.0 (le 2ᵉ PANIC du 03/09 l'avait déjà
      remis à zéro)

---

## LOT A — livré le 02/09/2026 (v2.33.0)

**Verdicts** : revue `code-reviewer` **Approved** ; gate `pool-chemistry` **GO**
+ **contre-visa GO** (révocation du NO GO v2.20.2 / ADR-0040 — argument central :
l'immunité vient du TEMPS, débounce 3 mesures / famine continue ≥ 10-15 s, pas du
niveau du seuil) ; tests **1029/1029** verts, **15/15 mutants tués** (dont
l'arithmétique du réarmement). Décision consignée : **ADR-0058**.

### Valeurs finales (écarts vs correctifs proposés au diagnostic)

- **Réarmement parachute** : 24 576 → **18 432** (le 16 384 proposé a été
  **REFUSÉ par le gate** : allouer 12 288 depuis 16 384 laisse un reliquat
  < 4 096 — le parachute sèmerait son propre auto-heal).
  18 432 = 12 288 (réserve) + 4 096 (auto-heal) + 2 048 (repli), somme
  verrouillée par un test d'arithmétique sur les constantes.
- **Les 4 gardes en `heapLargestFreeBlock8Bit()`**, échelle unique :
  2 048 (repli) < **4 096** (auto-heal, `kRestartMaxAllocHeapBytes`) <
  **6 144** (skip WS, `kWsPushMinAllocBytes`) < **12 288** (WARN,
  `kLowHeapWarnBytes`) < 18 432 (réarmement nominal) < 21 492 (nominal terrain).
- Garde 503 `/get-logs` : mesure basculée, marge dynamique inchangée.
- Tous les logs santé affichent `maxBloc8=`.

### Deux corrections d'affichage post-contre-visa

Réserve du contre-visa : afficher la MÊME métrique que celle qui décide.

1. `main.cpp` — log d'armement du parachute au boot : `maxBloc=` (brute) →
   `maxBloc8=` (8 bits).
2. `ws_manager.cpp` — WARN « pushes WS sautés » : idem.

### Hors périmètre consigné (lot B ou spec dédiée)

`web_routes_debug.cpp:55` (503 trace capteurs) et `web_routes_ota.cpp`
(garde TLS `kCheckUpdateMin*`) restent en métrique brute.

Fichiers modifiés : `src/constants.h`, `src/main.cpp`, `src/self_heal_logic.h`,
`src/ws_manager.{h,cpp}`, `src/web_routes_data.cpp`,
`test/test_native_oom_guard/`, `test/test_native_self_heal/`.
Docs : ADR-0058 (nouveau), ADR-0040 → Superseded, `oom-guard.md`,
`system-health.md`, `ws-manager.md`, `API.md`, `CHANGELOG.md`.

## OBSERVATION — 03/09/2026 08:59 : 2e PANIC sous v2.33.0, post-mortem du lot A capturé

Redémarrage PANIC (`reset_reason=4`) à 08:59:47, après 22 h 57 de fonctionnement
depuis le déploiement du 02/09 10:02 (`uptime=17080 s mod 65536` = 82 616 s ✓).
Premier redémarrage inattendu depuis le lot A — **le compteur « 7 jours sans
redémarrage » repart de zéro** (→ ~10/09 au plus tôt).

**Ce que le post-mortem RTC (lot A) a rendu — première capture exploitable :**

```
Cinétique heap avant reset : 32 échantillons, pas ~2 s
  t-78s … t-30s : maxB8=20416 free8=45824 ws=1 skip=0   (plat, 48 s)
  t-28s … t-10s : maxB8=20416 free8=50944                (+5 Ko libérés)
  t-3s  : free8=45056 · t-0s : free8=42752 (maxB8 inchangé 20416)
Post-mortem OOM : échecs=48 sur 8362 s (1er 1622 o, dernier 92 o, caps=0x0804),
  tas t-2s free8=42752 maxB8=20416, réserve=intacte, handler=actif,
  intégrité=OK → verdict SUSPICION CORRUPTION
```

Lecture :
- Les échecs d'allocation ont commencé vers **06:40** (8362 s avant le reset),
  soit ~17 min avant le démarrage de la filtration (06:57) — pas pendant un
  pic de charge web.
- **Un `new` de 92 octets a échoué avec un plus grand bloc 8 bits de 20 416 o**
  et 42 Ko libres : même signature que l'incident du 13/08 (156 o refusés
  avec 81 Ko libres). Le verdict automatique « suspicion corruption » est
  celui que le lot A devait produire dans ce cas ; le contrôle d'intégrité du
  tas était pourtant OK à chaque passage (cadence 300 s).
- Les gardes du lot A n'ont **rien vu venir** : maxBloc8 = 20 416 est au-dessus
  du seuil WARN (12 288) et du seuil de réarmement du parachute (18 432) — le
  tas n'était ni fragmenté ni épuisé au sens de la métrique. Ce n'est donc PAS un
  problème de seuil : la défaillance est ailleurs (allocation dans une
  capacité plus étroite que « 8 bits » — cf. échecs `caps=0x080C` = DMA —,
  ou corruption d'un bloc que le contrôle d'intégrité ne visite pas).

**Après le redémarrage, dégradation anormalement rapide :**
- 09:14 (T+15 min) : `Alloc heap échouée x3 (1422 o, caps=0x080C, heap_caps_malloc)`
- 09:34 (T+35 min) : idem x2, puis **`Heap fragmenté sous seuil : maxBloc8=12276`**
  — le plateau de 25 588 des boots précédents n'a même pas été atteint.
- Les échecs `caps=0x080C` (INTERNAL|DMA|8BIT) réclament de la mémoire **DMA**
  (WiFi/lwIP) : la métrique « plus grand bloc 8 bits » peut surestimer ce qui
  est réellement disponible pour eux (régions 8 bits non DMA : RTC fast RAM
  ajoutée au tas sous IDF 5). ~~**Piste prioritaire pour le lot B** : mesurer
  aussi `heap_caps_get_largest_free_block(MALLOC_CAP_DMA)`.~~
  **[RÉFUTÉ le 04/09 — conservé, NE PAS IMPLÉMENTER]** : `soc_memory_region_count = 44`
  et **aucune région RTCRAM n'est enregistrée**
  (`CONFIG_ESP_SYSTEM_ALLOW_RTC_FAST_MEM_AS_HEAP` absent du sdkconfig, vérifié).
  Sur ce binaire, toute région satisfaisant `INTERNAL|8BIT` satisfait aussi
  `DMA` : `largest_free_block(0x080C) ≡ (0x0804)`, **l'écart est
  structurellement nul**. Réfutation détaillée dans la section « DIAGNOSTIC
  LOT B » ci-dessous.

**Effets côté bassin** (documentés dans feature-084) : instantané intégrale
REJETÉ (« périmé », R3) → régulation repartie de zéro ; compteurs journaliers
restaurés (pH 130 mL, ORP 522 mL) ; anneau `ph_load7` intact → feedforward
réactivé à 09:15:54 avec la même dose de base 1,41 mL/min.

**Coredump décodé (10:44) — même cause exacte que le 29/08 :**

Pièces : `builds/2.33.0/esp32dev_core3/coredump_panic_20260903_0859.bin`,
`..._decoded.txt`, ELF concordant `firmware_20260902_094213.elf` (identique au
`firmware.elf` courant), journal `system_log_20260903_1040.txt`.

```
Tâche async_tcp, core 1, abort() (IllegalInstruction) :
abort() ← lock_init_generic (newlib locks.c:77) ← __retarget_lock_init_recursive
← __sfp ← _fopen_r ← fopen ← VFSFileImpl("r") vfs_api.cpp:245
← VFSImpl::exists("/app.js.gz") ← AsyncStaticWebHandler::_searchFile WebHandlers.cpp:150
← _getFile ← canHandle ← AsyncWebServer::_attachHandler
← AsyncWebServerRequest::_parseLine WebRequest.cpp:997 ← _onData ← AsyncClient::_recv
```

Lecture :
- Le dernier échec du post-mortem (**92 o, caps=0x0804**) EST la création du
  mutex newlib (`xQueueCreateMutex`, ~90 o) que `lock_init_generic` exige pour
  ouvrir `/app.js.gz` — troisième occurrence de la classe « `malloc` en chemin
  C, parachute `new_handler` inopérant » (29/08 page HTML, 13/08 fichier
  statique, 03/09 `app.js.gz`). Déclencheur : un chargement de l'UI à 08:59.
- Les caps de l'échec (0x0804) sont **exactement** ceux de la métrique
  (`heap_metrics.cpp` : `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT`) — l'hypothèse
  « la métrique et l'allocation ne regardent pas le même tas » est écartée pour
  cet échec-là (elle reste ouverte pour les échecs `0x080C` = DMA).
- **Le diagnostic du 31/08 (« famine 8 bits totale et transitoire ») n'est ni
  confirmé ni infirmé** : maxBloc8 est resté plat à 20 416 o sur les 78 s
  précédant l'abort et 48 allocations 0x0804 ont échoué en 2 h 20 avec toujours
  ≥ 20 Ko annoncés — mais la cinétique échantillonne toutes les ~2 s et ne peut
  pas exclure un creux total plus bref (rafale allouer-tout/libérer). Deux
  lectures restent en lice : (a) famine transitoire < 2 s provoquée par une
  rafale récurrente dès 06:40 (à identifier) ; (b) bloc libre annoncé mais
  inservable (liste libre TLSF corrompue — `heap_caps_check_integrity_all` OK
  n'exclut pas tout).
- **Instrumentation décisive pour le lot B** : mesurer le plus grand bloc 8 bits
  **à l'intérieur du hook d'échec d'allocation** (au moment exact du refus, pas
  2 s avant) et le journaliser dans le post-mortem RTC avec la taille et les
  caps demandés. ≈ 0 → (a), chercher la rafale ; ≈ 20 Ko → (b), corruption.
  Ajouter aussi le plus grand bloc `MALLOC_CAP_DMA` pour les échecs 0x080C. À
  confier au `debugger` avant tout correctif.

---

## DIAGNOSTIC LOT B — 04/09/2026 : le fragmenteur est identifié

> Analyse `debugger` en lecture seule, faits revérifiés à la main (sdkconfig,
> sources du core, journaux du 03/09). **Rien n'est implémenté** : cette section
> est la matière du cycle v2.35.

### La cause racine : `setvbuf(4096)` à chaque ouverture de fichier

Question ouverte depuis le 15/08 (« le demandeur des allocations de 4096 o n'est
pas identifié », `bug-famine-8bits`) : **c'est le tampon stdio de LittleFS.**

- `libraries/FS/src/vfs_api.cpp:22` → `DEFAULT_FILE_BUFFER_SIZE 4096` ;
  `:250` → `setvbuf(_f, NULL, _IOFBF, DEFAULT_FILE_BUFFER_SIZE)` à chaque `open()`.
- `sdkconfig` des libs précompilées (IDF 5.5.0) : `CONFIG_SPIRAM_USE_MALLOC=y`,
  **`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096`**.
- Le test de `heap_caps_malloc_default` (désassemblage `libheap.a`, `.iram1.2`,
  `bgeu a8, a2`) est **`taille <= limite ⇒ interne`**. Donc `malloc(4096)` avec
  limite 4096 reste **en RAM interne**. **Un octet de plus et il partait en PSRAM.**
- Aggravant : `VFSImpl::exists()` (`vfs_api.cpp:88-101`) **ouvre réellement le
  fichier** pour tester son existence, et `AsyncStaticWebHandler::_searchFile`
  (`WebHandlers.cpp:143-152`) fait **`exists()` PUIS `open()`** → **2 × (4096 o +
  ~90 o de mutex newlib) par fichier statique servi**.
- Un chargement d'UI ≈ 8 requêtes, jusqu'à 6 en parallèle (`async_tcp`) ⇒
  **jusqu'à ~49 Ko de demande interne 8 bits en rafale**, sur un `free8` de
  42-59 Ko. C'est le profil exact du PANIC du 03/09.

**Correctif candidat (B1)** : `heap_caps_malloc_extmem_enable(4095)` au setup,
après `initOomGuard()`. La PSRAM est là, à 4 Mo, **libre à 99,4 %**.
**Invariant à préserver** : la réserve du parachute doit rester allouée en RAM
**interne** explicite (`heap_caps_malloc(..., MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)`)
— libérer de la PSRAM pendant une famine interne ne sert à rien.
**Prédiction falsifiable** : après B1, `minFen` ne doit plus descendre sous
~30 000 o et `psram_free` doit chuter à chaque chargement d'UI. Sinon, hypothèse
fausse.

### Deux correctifs de la liste sont RÉFUTÉS — à ne pas implémenter

- **Correctif 4 (« pré-créer les verrous stdio au setup ») : INEFFICACE.**
  `_fclose_r` appelle `__retarget_lock_close_recursive` → `vSemaphoreDelete` :
  **le mutex créé par `fopen` est détruit par `fclose`**. Et `__sfp` ré-initialise
  le verrou à chaque attribution, y compris sur un `FILE` recyclé — les ~90 o
  sont donc payés à **chaque** `fopen`, indéfiniment. Une chauffe ne pré-alloue
  que le bloc de glue de 428 o (`_malloc_r(0x1AC)`), gain marginal. Les verrous
  nommés de la libc (`__lock___sfp_recursive_mutex`, etc.) sont déjà **statiques**
  en `.bss` sous IDF 5.5 : `_GLOBAL_REENT` n'est pas un problème.
- **Mesure `MALLOC_CAP_DMA` : CUL-DE-SAC.** `soc_memory_region_count = 44`, et
  **aucune région de type RTCRAM n'est enregistrée** (`CONFIG_ESP_SYSTEM_ALLOW_RTC_FAST_MEM_AS_HEAP`
  absent du sdkconfig — vérifié). Sur ce binaire, toute région satisfaisant
  `INTERNAL|8BIT` satisfait aussi `DMA` : `largest_free_block(0x080C) ≡ (0x0804)`.
  **L'écart est structurellement nul.** Les échecs `caps=0x080C` sont simplement
  les buffers WiFi dynamiques (1422/1478 o = MTU) qui échouent pendant la même
  famine transitoire.

### Un invariant de NOTRE documentation est faux, et il a coûté trois cycles

`src/heap_metrics.h:31-35` et `docs/subsystems/oom-guard.md:186-190` affirment
qu'un appel `heap_caps_*` depuis `oomFailedAllocHook` est un **deadlock**.
**C'est faux sur IDF 5.5** : `heap_caps_malloc` n'appelle `heap_caps_alloc_failed`
qu'**après** le retour de `heap_caps_malloc_base`, et le `portMUX` du tas est pris
**et rendu à l'intérieur** de `multi_heap_malloc_impl` (relocations
`multi_heap_internal_lock`/`_unlock` appariées). Aucun verrou de tas n'est détenu
quand le callback s'exécute. Les vrais risques sont le **coût** (parcours TLSF
interruptions coupées) et le **contexte ISR**, pas la réentrance.
⚠️ **À corriger dans le code ET la doc au cycle B-2, après contre-vérification
par `code-reviewer`** — c'est cet interdit qui a empêché l'instrumentation
décisive pendant trois cycles de diagnostic.

### Le creux dure moins de 2 s — prouvé par nos propres journaux

`system_log_20260903_1040.txt` : la fenêtre 09:30:41 → 10:00:47 rapporte
`minFen=23540` (minimum des échantillons de l'anneau **à 2 s**), alors que
`checkHeapHealth()` a mesuré **`maxBloc8=12276` à 09:34:41**, dans cette même
fenêtre, **avec la même fonction** (`heapLargestFreeBlock8Bit()`). **Un creux de
11 Ko a donc existé sans être vu par un seul échantillon à 2 s.** L'hypothèse (a)
« rafale sub-échantillon » est établie ; l'hypothèse (b) « corruption » n'a plus
d'appui — le verdict automatique « SUSPICION CORRUPTION » découle mécaniquement
de la règle `ringMaxB >= 4 × lastSize` appliquée à un `ringMaxB` **périmé de 2 s**,
il est donc **structurellement biaisé** dès que la rafale est courte. À corriger.

Même signature le 04/09 : `maxBloc8` instantané figé à 34 804 à chaque relevé,
`minFen` oscillant entre **16 372 et 31 732**.

### Recadrage de l'ADR-0058

Relevé vivant du 04/09 21:30 : `max_alloc_8bit = 34 804` après 1 j 13 h ;
`free_heap − free_heap_8bit = 82 308 − 54 316 = 27 992` = le plancher IRAM
32 bits, **confirmé au terrain**. Le « nominal fragmenté 21 492 » de l'ADR-0058
n'est **pas** un régime stable, c'est un plateau parmi d'autres : les valeurs
relevées sont presque toutes congrues à **1012 modulo 1024** (bloc de tête entamé
par pas de 1 Ko). Les 20 416 o du 03/09 (≡ 960 mod 1024) appartiennent à une
**autre classe** — un trou secondaire, pas le bloc de tête : l'appareil était
donc bien en état dégradé avant le PANIC, pas dans son régime normal.

### PREUVE AU DÉPLOIEMENT (05/09/2026 00:53) — la réserve ÉTAIT bien en PSRAM

v2.35.0 déployée en OTA (firmware + filesystem), appareil redémarré à 00:53.
Journal de boot :

```
Parachute OOM armé : réserve d'urgence de 12288 o (palier nominal)
                     pré-allouée en RAM interne, maxBloc8=110580 o
PSRAM : 4 Mo détectés, 4093 Ko libres — allocations de PLUS de 4095 o tentées
        en PSRAM d'abord (repli RAM interne si refus) ; 4095 o et moins : RAM interne
```

`/get-system-info` : `oom_reserve_bytes=12288`, `oom_reserve_internal=true`,
`oom_handler_registered=true`.

**Le chiffre décisif est la PSRAM libre**, et il va dans le sens CONTRAIRE de
l'intuition :

| | v2.33.0 (04/09 21:30, uptime 1 j 13 h) | v2.35.0 (05/09 00:53, uptime 12 s) | écart |
|---|---|---|---|
| `psram_free` | 4 169 840 | 4 182 164 | **+12 324 o** |

La PSRAM libre a **AUGMENTÉ de 12 324 octets** alors que ce lot y envoie
*davantage* d'allocations (tous les tampons stdio de 4096 o). Or **12 324 =
12 288 (réserve du parachute) + 36 o de métadonnées d'allocateur**.

⇒ **La réserve occupait bien la PSRAM sous la v2.33.0 ; elle est désormais en
RAM interne.** Le parachute anti-crash était donc DÉCORATIF depuis l'activation
de la PSRAM (feature-066) : il libérait de la mémoire externe pendant des
famines de mémoire *interne*. C'est cohérent avec les trois crashs OOM survenus
avec un filet réputé armé, et avec une ligne de journal qui annonçait « palier
nominal » — vraie sur la taille, fausse sur l'utilité.

**Réserve d'honnêteté** : la comparaison oppose un appareil fraîchement démarré
à un autre après 1 j 13 h de fonctionnement, donc `maxBloc8` (110 580 o au boot
contre 34 804 o la veille) et `free_heap_8bit` ne sont PAS comparables. Seul
l'écart de PSRAM l'est, parce qu'aucun autre consommateur PSRAM n'existe à ce
stade — et il tombe sur la valeur exacte de la réserve, à 36 octets près.

**Ce que cela ne prouve pas encore** : que la fragmentation cesse. C'est l'objet
de la prédiction falsifiable ci-dessus (`minFen` ≥ ~30 000 o sur 48 h), non
tranchée à cette date.

⚠️ **Contamination assumée du signal** : la v2.35.0 a été déployée sur un
appareil en v2.33.0, elle apporte donc AUSSI la v2.34.0 (purge adaptative
feature-091 + intégrale persistante feature-093). Décision utilisateur du
05/09. En cas de PANIC dans les 48 h, l'attribution entre les deux leviers sera
**ambiguë** — c'est le prix accepté pour ne pas retarder les deux correctifs.

### Découpage proposé (le flash n'est PAS la contrainte : ~1,1 Ko au total sur 26 Ko)

| Lot | Contenu | Flash | Gate | Pourquoi séparé |
|---|---|---|---|---|
| **B-1** (2.35.0) | `heap_caps_malloc_extmem_enable(4095)` + réserve du parachute capée en interne | ~200 o | **pool-chemistry** | Un seul levier, un seul effet à mesurer. Déployer seul, observer 48 h. |
| **B-2** (2.35.1) | Sonde du plus grand bloc **dans** le hook d'échec (bornée : 1/s, jamais en ISR) + verdict post-mortem révisé + bascule de `web_routes_debug.cpp:55` | ~400 o | **pool-chemistry** | La sonde sert à juger B-1 : ne pas mélanger les deux signaux. |
| **B-3** (2.36.0) | Garde 503 dans le filtre `serveStatic`, **avant** le premier `fopen` (le filtre est évalué avant `canHandle`, `WebServer.cpp:147`) | ~300 o | code-reviewer | Ceinture : ferme un chemin d'`abort()` sans recours. Calibrer sur le régime post-B-1. |
| Hors lot | Recalibrage des seuils OTA (bascule aveugle = **mise à jour en ligne condamnée**, `free8 = 54 316 < 65 536`) ; cache PSRAM des assets | — | spec dédiée | Décisions produit. |

**Gate `pool-chemistry` requis pour B-1 et B-2** : B-1 change la stratégie
d'allocation globale, donc l'assiette sur laquelle l'ADR-0058 a calibré
`kRestartMaxAllocHeapBytes` et le débounce d'auto-guérison ; B-2 introduit un
parcours de tas interruptions coupées dans un chemin qui peut s'exécuter pendant
une injection.

### Observation armée pour le 05/09

`scratchpad/record_heap_0509.sh` (pid 71692) : `/get-system-info` toutes les 20 s
de 06:20 à 07:20, puis `/get-logs` complet. Route JSON — elle ne passe **pas** par
le chemin `fopen`/`setvbuf` des fichiers statiques. **Ne pas ouvrir l'interface
web pendant la fenêtre** : le déclencheur des deux PANIC est un chargement d'UI.
Ce qu'on cherche : `free_heap_8bit` en chute corrélée à 06:40 (rafale), ou plat
(phénomène sub-échantillon, seule la sonde B-2 trancherait) ; `max_alloc_8bit`
quittant son plateau sans y revenir (fragmentation permanente) ; dans le journal,
`Watchdog connectivité` / `WiFi déconnecté` / `MQTT déconnecté` + republication
d'auto-discovery HA (~55 topics) — **hypothèse exogène, la plus crédible pour
06:40** : aucun candidat périodique interne ne tombe à cette heure (SNTP à 3 h
d'intervalle, pas de vérification de mise à jour automatique, historique aux 5 min).

---

## VERDICT DE LA PRÉDICTION FALSIFIABLE DE B-1 — mesuré le 05/09 après-midi

Relevés pris sur l'appareil en v2.39.0 (boot 11:36:57), entre 12:03 et 13:36, sans
attendre les 48 h : le régime observé suffit à trancher.

### Prédiction n°2 — CONFIRMÉE

`psram_free` **chute bien** à chaque chargement d'interface : creux mesuré de
**16 912 o = 4,13 tampons de 4 096**, puis retour au plateau. Le levier de B-1
fonctionne : les tampons stdio partent en PSRAM.

### Prédiction n°1 — RÉFUTÉE, et largement

Elle exigeait `minFen ≥ ~30 000 o`. Les quatre traces « Heap 8bits: » de la
journée :

| t | libre8 | maxBloc8 | minBoot | minFen |
|---|---|---|---|---|
| 61 s | 47 756 | 42 996 | 38 900 | 38 900 |
| 1 864 s | 59 384 | 26 612 | 23 540 | 23 540 |
| 3 669 s | 58 872 | 20 468 | **1 908** | **1 908** |
| 5 474 s | 58 424 | 15 860 | 1 908 | 15 348 |

**Le plus grand bloc 8 bits est descendu à 1 908 octets.** C'est deux ordres de
grandeur sous la prédiction, et bien en dessous des 16 372 o **d'avant** B-1. Le
creux tombe dans la fenêtre 12:08-12:38, qui est celle des rafales de chargement
d'UI faites pour ce test — donc **provoqué et reproductible**, pas accidentel.

Deux faits distincts s'y ajoutent :

1. **La fragmentation domine, mais elle N'EST PAS monotone.**
   ⚠️ **Correction du 05/09 16:00** — il a d'abord été écrit ici, et propagé dans
   le message de commit de la v2.41.0, que le plateau « ne remonte jamais sans
   redémarrage ». **C'est faux.** L'enregistreur de 2 h (échantillon aux 2 min)
   montre une remontée de **21 492 → 38 900 entre 14:11 et 14:13, dans la même
   session**, sans redémarrage. Le plateau **fluctue** : il s'effondre sous charge
   et se recompose au repos.
   Ce qui reste vrai et mesuré : la tendance est **descendante** sur la durée d'une
   session (42 996 → 26 612 → 20 468 → 15 860 → 13 300 sur la session du matin), et
   le premier chargement d'UI après un démarrage coûte à lui seul 6 144 o de
   plateau. Mais « permanent » et « monotone » sont des mots trop forts pour ce que
   les données montrent, et le lot C ne doit pas partir sur cette base.
2. **Le plateau est passé sous le seuil de réarmement du parachute.**
   `kOomRearmMinMaxAllocBytes = 18 432` ; à t+1 h 55 le plus grand bloc vaut
   **15 860 o**, et `minFen` de la dernière fenêtre **15 348 o**. Le palier
   nominal ne peut donc plus se réarmer, **en régime calme**, sans qu'aucune
   alerte ne le dise.

Conformément à l'encadré ci-dessous, la réfutation **n'autorise pas un correctif
de plus** : elle renvoie aux mesures, c'est-à-dire au lot B-2.

### ⚠️ LE FAIT NEUF — les échecs réels sont des allocations DMA

Le journal porte des échecs d'allocation **qui se produisent déjà**, sans PANIC :

    t=1236s  Alloc heap échouée x1 (dernière=1622 o caps=0x080C appelant=heap_caps_malloc, cumul=1)
    t=1266s  Alloc heap échouée x1 (…, cumul=2)
    t=2513s  Alloc heap échouée x4 (…, cumul=6)
    t=4313s  Alloc heap échouée x5 (…, cumul=11)

**Onze échecs en 1 h 12**, tous sur **1 622 octets** en `caps=0x080C`, soit
`MALLOC_CAP_8BIT | MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL`.

La contrainte **DMA est strictement plus restrictive** que `INTERNAL|8BIT`. Toute
sonde à caps fixes répondrait donc **à côté de la question** : elle annoncerait un
bloc libre confortable pendant qu'une allocation DMA échoue — et produirait
exactement le faux verdict « SUSPICION CORRUPTION » que le lot B-2 supprime.
C'est ce relevé qui a imposé la **double mesure** (caps de l'échec + référence
8 bits) retenue en B-2, et le verdict neuf **FAMINE DMA**.

### Ce que ces relevés changent pour le lot B-3

Le seuil de la garde 503 devient calibrable **sans attendre 48 h** :

- plancher de `maxBloc8` sous rafale d'UI : **14 324 o** (sonde manuelle à 500 ms,
  donc le vrai plancher est plus bas — le firmware a vu **1 908 o**) ;
- plancher de `free8` sous rafale : **22 092 o** ;
- plateau en régime calme après 2 h : **15 860 o**.

⚠️ Toute valeur de seuil choisie entre 15 000 et 20 000 rendrait l'interface
**indisponible en régime normal**, puisque le plateau calme y est déjà.

---

## LOT B-1 — livré le 05/09/2026 (v2.35.0)

### ⚠️ PRÉDICTION FALSIFIABLE ET SON CRITÈRE DE RÉFUTATION

**À lire AVANT tout nouveau correctif.** Le lot B-1 est déployé **seul**,
précisément pour être jugé seul.

> **Prédiction** — après B-1 déployé seul, sur 48 h d'observation :
> 1. `minFen` (trace « Heap 8bits: », cadence 30 min) **ne doit plus descendre
>    sous ~30 000 o** ;
> 2. `psram=` (même ligne) doit **chuter de plusieurs dizaines de Ko à chaque
>    chargement d'interface**, puis remonter.
>
> **Critère de RÉFUTATION** — **si `minFen` replonge à 16-20 Ko, l'hypothèse du
> tampon stdio est FAUSSE.** Dans ce cas le cycle repart aux **MESURES**
> (sonde B-2 : plus grand bloc mesuré **dans** le hook d'échec, au moment exact
> du refus), **PAS à un correctif de plus**. Trois cycles de diagnostic ont
> déjà été perdus à empiler des correctifs sur une instrumentation muette.

Les valeurs de référence d'avant B-1 : `minFen` oscillant entre **16 372 et
31 732** (relevés du 04/09), `maxBloc8` instantané figé à 34 804 ; PSRAM libre
à **99,4 %**, quasi constante.

### Notes d'implémentation B-1

**Le levier** : `heap_caps_malloc_extmem_enable(kPsramExtmemThresholdBytes)`
(4095) au `setup()`, **un seul site d'appel**, juste après `initOomGuard()`,
sous garde `psramFound()`. La contrepartie obligatoire : les **trois** sites
d'allocation de la réserve du parachute (nominal, repli, **réarmement**) passent
par l'entonnoir `heap_caps_malloc(..., MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)`,
sous `#pragma GCC poison malloc calloc realloc strdup`.

| Fichier | Lignes clés | Contenu |
|---|---|---|
| `src/constants.h` | 421-438 | `kVfsStdioBufferBytes` (4096), `kPsramExtmemThresholdBytes` (4095), `kOomNoReserveCriticalDelayMs` (300 000) + **2 `static_assert`** (`seuil + 1 == tampon`, `kOomReserveBytes > seuil`) |
| `src/main.cpp` | 8, 128, 150, 1043 | `#include <esp_heap_caps.h>` ; site d'appel unique de la bascule ; emplacement sur la ligne d'armement ; champ `psram=` de la trace « Heap 8bits: » ; message PSRAM du boot réécrit **en octets** |
| `src/oom_guard.cpp` | 39, 47, 269/272/681, 522-582 | `#pragma GCC poison` ; `oomAllocReserveInternal()` ; les 3 sites d'allocation ; WARN de déclassement + CRITICAL « sans réserve » + drapeau `sNoReserveAlertPending` |
| `src/oom_guard.h` | +39 l. | `oomGuardReserveIsInternalRam()`, `oomGuardConsumeNoReserveAlert()` (contrats documentés) |
| `src/mqtt_manager.cpp` | 299 | Drain du drapeau depuis **sa** tâche → `publishAlert("oom_reserve_absent", …)` |
| `src/web_routes_config.cpp` | 1221-1222 | `oom_reserve_bytes` + `oom_reserve_internal` (contrat à 4 états) |
| `test/test_native_oom_guard/` | +62 l. | `test_psram_extmem_threshold_arithmetic_v2350` |

**Verdicts des agents** :

- `pool-chemistry` : **GO sous 5 conditions** — C1 (emplacement de la réserve
  **journalisé**, jamais supposé), C2 (aucun déclassement ni absence de réserve
  **silencieux** → WARN + CRITICAL + alerte MQTT), C3 (interdiction de `malloc`
  dans `oom_guard.cpp`, matérialisée par le `#pragma GCC poison`), C4 (protocole
  on-target, voir ci-dessous), C5 (seuils de l'ADR-0058 **non retouchés** dans
  ce lot).
- `code-reviewer` : **Request changes**, puis correctifs appliqués — dont
  l'ambiguïté d'`oom_reserve_internal` seul (→ ajout d'`oom_reserve_bytes`,
  contrat à 4 états), la publication MQTT sortie d'`oom_guard` (drapeau drainé
  par `mqtt_manager`), les tampons de pile des deux nouvelles lignes portés à
  224 o (un `[176]` **tronquait silencieusement** un WARN de diagnostic — soit
  exactement ce que ce lot existe pour supprimer) et la lecture du pointeur
  **local** (et non de l'atomique) dans le log de réarmement, pour ne pas
  annoncer « PSRAM (ANOMALIE) » sur une course avec `oomNewHandler`.

**Empreinte flash** : **1 810 361 / 1 835 008 o = 98,7 %**, marge **24 647 o**.
Le levier lui-même vaut ~**200 o** ; tout le reste est le **prix de
l'observabilité exigée par le gate** (emplacement journalisé, WARN de
déclassement, CRITICAL + alerte sans réserve, deux champs d'API). Ce coût est
assumé : un correctif de mémoire qu'on ne peut pas **observer** ne se juge pas.

**Tests** : **29 suites / 1 090 cas** natifs verts, zéro régression.
**Mutations tuées à la compilation** : `seuil 4095 → 4096` et le retrait de
`MALLOC_CAP_INTERNAL` de l'entonnoir. Le test neuf couvre en plus la mutation
que les `static_assert` **ne voient pas** — une **translation** des deux
constantes (8192 / 8191 compile sans un mot et ramènerait le tampon réel de
4096 o en RAM interne).

**Décision consignée** : **ADR-0061** ; **ADR-0058** amendé d'une note
(assiette de calibrage changée, seuils **inchangés**, réévaluation après 48 h).

### Branche non couverte par le build

La garde `#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3`
autour de `esp_memory_utils.h` / `soc_memory_layout.h` (en-tête d'où provient
`esp_ptr_internal`) est **correcte sur le papier** mais **l'environnement core 2
n'a pas été compilé** dans ce cycle — c'est interdit ici, les deux plateformes
s'écrasant mutuellement dans `.pio/`. À vérifier si le repli core 2 devait un
jour être réactivé.

### Entrée du lot B-2 — l'interdit `heap_caps` de `heap_metrics.h:31-35` doit être REQUALIFIÉ, pas supprimé

Avis du `code-reviewer`, à traiter en B-2 :

- **Le mot « deadlock » est factuellement FAUX.** Désassemblage de `libheap.a` :
  `heap_caps.c.obj` ne contient **aucune relocation** vers
  `multi_heap_internal_lock` / `_unlock` — le verrou vit **entièrement** dans
  `multi_heap.c.obj` —, et `heap_caps_alloc_failed` n'est atteint qu'**après le
  retour complet** de `heap_caps_malloc_base`. Aucun verrou de tas n'est détenu
  quand le callback s'exécute.
- **MAIS l'interdit reste pleinement valable sous une autre qualification** :
  c'est un interdit de **LATENCE**, pas de réentrance. Un parcours TLSF
  s'exécute **interruptions coupées** ; le hook peut être déclenché par
  **rafales de centaines par seconde** ; et il est **appelable depuis une ISR**.
- **Ce que B-2 devra faire** :
  1. reformuler le commentaire de `src/heap_metrics.h:31-35` **et** le
     paragraphe correspondant de `docs/subsystems/oom-guard.md` en « **coût
     interruptions coupées** », avec la **référence au désassemblage** ;
  2. **borner la sonde** : **un échantillon par épisode**, **jamais depuis une
     ISR** (test `xPortInIsrContext()`) ;
  3. **mesurer et journaliser la durée réelle** du parcours, sur le modèle de
     `kHeapIntegrityWarnDurationUs` ;
  4. **couvrir TOUS les points d'appel du hook** : `realloc`, `calloc`,
     `aligned_alloc`, `*_prefer`, `*_default` — pas seulement `malloc`.
- ⚠️ **Ne PAS toucher au paragraphe « aucun `heap_caps_*` depuis le chemin
  d'échec » de `docs/subsystems/oom-guard.md` avant le lot B-2** : sa
  requalification appartient à ce lot, avec le code qui la rend vraie.

C'est cet interdit — juste dans sa conclusion, faux dans sa justification — qui
a empêché l'instrumentation décisive pendant **trois cycles** de diagnostic.

### Protocole de test ON-TARGET (test-engineer) — 7 points

À exécuter au déploiement de la 2.35.0, dans l'ordre.

1. **Ligne d'armement du 1ᵉʳ boot** — c'est **le** verdict de l'hypothèse
   « la réserve partait en PSRAM depuis feature-066 ». Attendu :
   `Parachute OOM armé : réserve d'urgence de 12288 o (palier nominal)
   pré-allouée en RAM interne, maxBloc8=… o`. Toute mention
   `PSRAM (ANOMALIE)` est un **échec bloquant**.
2. **Message PSRAM du boot** : doit énoncer le seuil **en octets avec sa
   direction** (« allocations de PLUS de 4095 o tentées en PSRAM d'abord… ») —
   plus aucun arrondi en kilo-octets.
3. **Mutation n°1** — **retirer `MALLOC_CAP_INTERNAL` de l'entonnoir**
   `oomAllocReserveInternal()`, flasher, redémarrer : la ligne d'armement
   **doit** afficher `PSRAM (ANOMALIE)`. Si elle affiche encore
   `RAM interne`, la sonde d'emplacement ne prouve rien et tout le point 1 est
   sans valeur. **Restaurer le code immédiatement après.**
4. **Prédiction falsifiable** (48 h) : relever les traces « Heap 8bits: »
   (30 min) — `minFen ≥ ~30 000 o` et `psram=` en **chute** corrélée aux
   chargements d'interface. Critère de réfutation : voir l'encadré en tête de
   section.
5. **Rafale d'interface** : recharger l'UI complète plusieurs fois de suite
   (le **déclencheur exact** des deux PANIC), y compris depuis plusieurs
   navigateurs — **aucun PANIC**, aucune ligne `Alloc heap échouée` en rafale.
6. **`/get-system-info`** : vérifier le contrat à 4 états —
   `oom_reserve_bytes = 12288` **et** `oom_reserve_internal = true` en régime
   sain ; vérifier aussi que `psram_free` **varie** avec la charge (il ne doit
   plus être quasi constant).
7. **Condition C4 du gate `pool-chemistry`** — l'intégrité des données ne se
   déduit pas de l'absence de crash :
   - **relire le CONTENU de `/history.json`** (pas seulement vérifier qu'il se
     télécharge) : structure valide, points cohérents, pas de troncature ;
   - **round-trip NVS après reboot** : compteurs `pool-daily` **et** instantané
     de l'intégrale pH correctement restaurés ;
   - **rafale d'UI PENDANT une injection pH active** : les compteurs de volume
     doivent rester **monotones**, l'injection aller à son terme, aucune
     interruption ni double comptage.


---

## LOT B-2 — livré le 05/09/2026 (à déployer)

### Ce que le lot change

Le hook d'échec d'allocation mesure désormais le tas **au moment exact du refus**,
et ce couple de mesures remplace l'anneau périmé de 2 s comme fondement du verdict
post-mortem. Deux mesures, pas une : avec les **`caps` réellement demandées** par
l'allocation refusée, et une **référence `INTERNAL|8BIT`**.

C'est le relevé terrain du jour qui a imposé ce choix. Les onze échecs observés
portent tous sur `caps=0x080C`, donc **DMA** — une contrainte strictement plus
restrictive que `INTERNAL|8BIT`. Une sonde à caps fixes aurait annoncé un bloc
libre confortable pendant qu'une allocation DMA échouait, c'est-à-dire reproduit
**exactement** le faux verdict que ce lot existe pour supprimer.

Verdict neuf : **`FAMINE DMA (pool restreint)`**, en WARN — ce n'est pas une
corruption. Et **`SUSPICION CORRUPTION` n'est plus atteignable sans mesure
contemporaine** : sans sonde, la règle historique est conservée mais **amputée de
sa conclusion**, et rend `INDÉTERMINÉ`. L'indétermination est **attribuable**
(`sonde=oui / absente / ISR / désactivée`).

### Gate `pool-chemistry` — GO sous 6 conditions, toutes tenues

La plus structurante est la **n°2** : le gate a **refusé la protection post-hoc
seule** sur le pire cas PSRAM. La sonde ne peut pas être *sautée* (l'obligation
n°4 interdit de filtrer), mais pour un masque qui peut atteindre la PSRAM, le
parcours O(blocs) est remplacé par des agrégats **O(régions)**, champ « plus grand
bloc » marqué non renseigné. Le pire cas est ainsi borné **avant** coup.

Les autres : seuils **300 µs** (WARN) et **1 500 µs** (désactivation définitive)
sur l'assiette du **couple** ; **plancher temporel absolu de 1 000 ms** entre deux
sondes, en plus du jeton ; observabilité sans rien de silencieux ; aucune sonde
avant le premier `oomGuardLoopTick()` ; protocole on-target.

Validé explicitement par le gate : **pas de dérive de dosage** — `millis()` vient
d'`esp_timer` (64 bits matériel) et ne perd rien interruptions coupées, le PWM des
pompes est LEDC matériel, et le volume est crédité **au temps**. Un retard ≤ 5 ms
est *compté*, non perdu : 0,002 ml. Et **pas d'`esp_task_wdt_reset()`** dans ce
chemin (divergence assumée du modèle `oom_guard.cpp:448-451`, commentée en clair).

### Les deux défauts rattrapés en revue — tous deux auraient rendu la sonde inopérante

**B1 — la borne PSRAM ne couvrait pas la classe d'échec principale.** Le prédicat
basculait en O(régions) sur la présence du bit `MALLOC_CAP_SPIRAM`. Or le
désassemblage de `libheap.a` (`heap_caps.c.obj`, `.iram1.2`, `.literal+0x8 =
0x00001000`) montre que **tout échec de `malloc()`/`operator new` remonte
`caps = MALLOC_CAP_DEFAULT`**, jamais le bit SPIRAM — c'est-à-dire les incidents
du 11/08 et du 13/08 qui fondent ce chantier. La sonde serait donc partie
parcourir les 4 Mo de PSRAM interruptions coupées, et se serait **désactivée
définitivement sur son cas d'usage principal**.

Correctif **fail-safe** : on ne prend O(blocs) que si le masque **garantit** la
RAM interne (`caps & MALLOC_CAP_INTERNAL`). Le critère cesse d'être un test de
*présence* — muet sur tout masque non énuméré — pour devenir un test de *garantie*.

**B2 — la mesure était écrite puis silencieusement jetée.** Le mot portant l'octet
`flags` n'est écrit que par `oomPostMortemStore()`, appelée **avant** la sonde. Les
mots de mesure atteignaient la RAM RTC, mais **le bit `ProbeTaken` n'y arrivait
jamais**. Au boot suivant, la sonde était donc jugée absente et le verdict
retombait à `INDÉTERMINÉ` — dans le scénario « refus → abort → reboot », c'est-à-dire
**précisément quand le post-mortem sert**. Le cas des onze refus « aurait marché »
par accident au deuxième refus ; le cas mono-refus-puis-panic, jamais.

Deux défauts qui vivaient l'un et l'autre dans la **coquille impure** : les 14
mutants du premier passage ne les voyaient pas. L'extraction ADR-0017 couvre les
prédicats, pas l'ordre des écritures ni le choix réel des `caps`.

### Décision consignée — dépassement du budget flash assumé

**+2 748 o** contre une cible de 900 et un seuil d'alerte de 1 300. Marge restante
**10 463 o (0,57 %)**.

Les leviers de réduction prévus par le plan **n'ont PAS été activés**, et c'est
délibéré : L1 ne vaut que 55 o, et L2/L3 économiseraient ~215 o en supprimant
respectivement la protection contre la troncature et la ligne de journal **qui
contient la mesure**. Payer de la place en supprimant le signal que le lot produit
serait absurde. L'essentiel du coût est du `.rodata` — les libellés français du
diagnostic — et 688 o avaient déjà été récupérés sans perte.

⚠️ **Le vrai sujet n'est pas ce lot, c'est le suivant.** À 99,4 % de flash, une
spec « réduction flash » devrait être ouverte **avant** le prochain lot fonctionnel,
plutôt que de rogner celui-ci.

### Tests

32 suites, **1 261 cas**, zéro régression. Suite `oom_guard` : 85 → 114 cas.
**26 mutants appliqués, 26 tués** (dont 2 à la compilation par `static_assert`).

Le test qui justifie le lot est `test_field_20260905_*` : il rejoue le relevé réel
(`failCount=6, lastSize=1622, lastCaps=0x080C, ringMaxAllocBytes=26612`) et prouve
que le verdict passe de « SUSPICION CORRUPTION » à `INDÉTERMINÉ` sans sonde, et à
**FAMINE DMA** avec.

⚠️ **Leçon de méthode** : un test existant *verrouillait* le défaut B1 — il passait,
et prouvait l'inverse de ce qu'on croyait. Non pas par une assertion fausse (celle
sur `0xFBFF` reste vraie, ce masque contient `INTERNAL`), mais par **l'absence de
couverture de `0x1000` seul**, doublée d'un `static_assert` épinglant le mauvais
invariant — la *valeur du bit* SPIRAM plutôt que la question « ce masque peut-il
atteindre la PSRAM ». Une assertion juste sur un invariant hors sujet produit de la
confiance sans produire de garantie.

### Constat versé au dossier, non corrigé

**Le relevé du 13/08/2026 portait `caps=0x0404`**, soit `MALLOC_CAP_8BIT |
MALLOC_CAP_SPIRAM` — le bit **SPIRAM**, pas `INTERNAL`. L'allocation refusée ce
jour-là demandait donc de la mémoire **en PSRAM**. Son verdict « SUSPICION
CORRUPTION » est très probablement **un second faux verdict**, d'une autre famille
que celui du 05/09 : pool inexistant ou vide plutôt que pool épuisé.

Conséquence de conception à connaître : sur cette classe de caps, la sonde bascule
en O(régions) et **ne pourra jamais conclure « corruption »**. Le verdict de
corruption n'est atteignable que sur les classes **internes**. À confirmer par
`debugger` / `pool-chemistry` si le 13/08 doit être requalifié.

### Reste à faire — protocole ON-TARGET (condition 6 du gate)

À exécuter au déploiement, dans cet ordre :

1. **Famine `caps=0x080C`** (le cas des onze échecs) → `sonde=oui`, verdict
   **`FAMINE DMA` en WARN** (jamais CRITICAL), durée **< 300 µs**.
2. **Famine `caps=0x1000`** (un `new` ordinaire refusé) → branche **O(régions)**
   prise, et la sonde **ne se désactive pas**. C'était le défaut B1 : sans ce
   point, il n'est pas prouvé corrigé sur cible.

   ⚠️ **Le volet « famine PSRAM » du protocole initial est devenu vide de sens** et
   a été retiré : depuis le fail-safe de B1, un masque PSRAM emprunte la branche
   O(régions), dont la durée est **structurellement négligeable**
   (`multi_heap_free_size_impl` ne fait qu'une lecture de champ — vérifié au
   désassemblage : aucune relocation externe, ni verrou ni parcours). Le mesurer
   ne prouverait rien. **La mesure qui compte est la branche O(blocs) sur le tas
   interne au pire de sa fragmentation**, c'est-à-dire `caps=0x080C` avec `minFen`
   au plancher — soit le point 1 ci-dessus.
3. **Scénario refus → abort → reboot** → la **ligne 2 est émise** au boot suivant.
   Sans ce point, B2 n'est pas prouvé sur cible.
4. **`/get-system-info`** → les 4 champs `oom_probe_*` cohérents avec le journal.
5. **Zéro impact sur un dosage en cours** : volume journalier attendu vs relevé
   pendant un épisode de famine.
6. Mutants non couvrables en natif, à provoquer sur cible : suppression de
   `xPortInIsrContext()`, suppression du jeton, **suppression des sites d'appel du
   flush** (la forme exacte qu'avait B2), `kOomProbeMaxDurationUs = 1`.
7. Non-troncature des lignes enrichies (`[384]`, `[320]`, `[224]`, `[256]`) avec un
   nom d'appelant long — le calcul UTF-8 est manuel et non vérifiable hors cible.
8. Perte **assumée** du post-mortem au boot de bascule (magic bumpé) : vérifier
   qu'**aucune** ligne n'est émise, plutôt qu'une ligne fausse.


---

## LOT B-3 — livré et déployé le 05/09/2026 (v2.41.0)

### Ce que le lot ferme

Servir une page statique demande à la libc un verrou récursif d'environ 88 octets.
Quand ce `malloc` échoue, `lock_init_generic` appelle **`abort()`** : aucun code de
retour à tester, aucun repli. C'est le PANIC du 29/08. Et comme ce chemin passe par
`malloc` (C) et non `operator new`, **le parachute OOM est inopérant par
construction** dessus — B-3 en est la seule défense.

Un **handler prioritaire** répond désormais **503** (avec `Retry-After: 5`) quand le
plus grand bloc 8 bits passe sous `kStaticFileMinAllocBytes = 8192`.

### La spec se trompait d'endroit — périmètre corrigé

Elle désignait le filtre de `serveStatic`. Deux raisons le rendent inopérant :

1. ce filtre **rejette les `.html`** : il n'aurait jamais couvert les 7 routes HTML
   explicites, c'est-à-dire **précisément le chemin du PANIC** ;
2. `AsyncStaticWebHandler::canHandle` touche le système de fichiers **dès
   `canHandle`** : un filtre posé là serait arrivé **après** le `fopen`.

Un handler enregistré **avant** les autres est le seul point réellement antérieur.
Et comme `_attachHandler` s'arrête au premier handler qui accepte, **l'ordre
d'enregistrement devient la politique d'exemption** — plus sûr qu'une liste
d'exceptions, qui aurait dérivé. Les 19 routes GET de diagnostic, de configuration
et d'OTA sont déclarées avant, donc hors d'atteinte.

Les commandes de dosage sont protégées par autre chose : **toutes en POST**, la
garde ne matche que GET. Vérifié : 0 route GET dans `web_routes_control.cpp`, 19 en
POST, plus un double test de méthode dans le routeur dynamique.

### ⚠️ LA TROUVAILLE DU LOT — le logger pouvait se tuer lui-même

`logger.cpp:71-73` flushe **en synchrone, dans la tâche appelante**, sur `ERROR` et
`CRITICAL`. Cette écriture ouvre `/system.log` — donc **`fopen`**, donc exactement
l'`abort()` que ce lot ferme.

> **Journaliser une erreur depuis un chemin anti-`fopen` déclencherait le crash que
> ce chemin prévient.** La garde se serait tuée en signalant qu'elle travaillait.

D'où la contrainte, **générale et pas propre à B-3** : `WARN`/`INFO` uniquement, et
jamais depuis `async_tcp`. La logique pure vit dans un header sans Arduino ni
FreeRTOS : elle est **structurellement incapable** d'émettre un log. La contrainte
est garantie par la forme, pas par la discipline.

⚠️ **Reste ouvert** : `Logger::flushToDisk()` (`logger.cpp:276-277`) est un **second
`fopen` susceptible d'aborter**, qu'aucun 503 ne peut couvrir. Lot ultérieur.

### ⚠️ RÉSULTAT ON-TARGET — la garde a mordu au premier essai, et c'était justifié

Test du 05/09 après déploiement, appareil en régime sain (`maxBloc8 = 34 804`) :

| Concurrence | Refus |
|---|---|
| séquentiel | **0** |
| 2 en parallèle | **0** |
| 3 en parallèle | **0** |
| **6 en parallèle** | **1** (`/uPlot.min.css`), reproductible sur 2 vagues |

Ligne unique émise, conforme au bornage :
`WARN Service fichiers suspendu (503) — maxBloc8=2932 o < 8192 o`

**Le chiffre à retenir est 2 932 octets.** Sous 6 requêtes strictement simultanées,
le plus grand bloc s'effondre à cette valeur — alors qu'il valait 34 804 juste
avant, et que la meilleure mesure échantillonnée de la journée n'était descendue
qu'à 14 324 (sonde à 500 ms) et 1 908 (minimum firmware sur 30 min).

Deux conséquences :

1. **Le seuil de 8 192 est validé par l'expérience** : il faut qu'il soit
   nettement au-dessus de 2 932 pour mordre **avant** l'échec, et c'est le cas.
2. **Le PANIC était bien plus proche qu'on ne le croyait en usage normal.** Un
   simple chargement de page à 6 connexions parallèles — ce que fait un navigateur —
   amène le tas à moins de 3 Ko de plus grand bloc. Le crash du 29/08 n'était pas
   un concours de circonstances exotique.

**Conséquence utilisateur assumée** : au pire cas de concurrence, un asset peut être
refusé et la page s'afficher incomplète ; un rechargement suffit. C'est infiniment
préférable à un redémarrage en pleine régulation, et c'est **exactement ce que le
guide utilisateur explique** désormais (chapitre 4, encadré « Votre piscine continue
d'être régulée », avec la consigne de ne pas couper le courant).

### Ce qu'on accepte, dit honnêtement

- En famine, `/wifi.html` et `/` sont refusés : **le portail de secours et la page
  OTA deviennent inaccessibles au navigateur**. Les vrais chemins de secours
  survivent : `/update` est en POST, `./deploy.sh ota-firmware` fonctionne, et les
  routes de diagnostic (`/get-logs`, `/get-system-info`, `/data`, WebSocket, MQTT)
  répondent — ce sont elles qui permettent de comprendre l'épisode.
- ⚠️ **La formulation « impossible de se verrouiller dehors » du plan est FAUSSE.**
  L'auto-guérison mord à 4 096. La bande **4 096 ≤ maxBloc8 < 8 192** est donc un
  régime **stable** où l'interface est morte **sans qu'aucun redémarrage ne
  survienne**. Vu la tendance descendante constatée, cette bande est atteignable.
  La conclusion opérationnelle ne change pas — mieux vaut un 503 qu'un `abort()` —
  mais l'énoncé devait être corrigé.
  *(Correction du 05/09 16:00 : « décroissance monotone » remplacé par « tendance
  descendante » — l'enregistreur de 2 h a mesuré une remontée de 21 492 à 38 900
  en cours de session, sans redémarrage. Le plateau fluctue.)*

### Coût du parcours de tas par requête — arbitré, pas ignoré

Le filtre appelle `heap_caps_get_largest_free_block()` sur `async_tcp` à chaque GET
atteignant la garde : **70-250 µs par appel**, 1-3 ms par chargement de page, soit
≲ 10 % du coût d'un service de fichier qui existe déjà.

Les deux garde-fous imposés au lot B-2 **ne s'appliquent pas ici** :
- la **borne a priori** est acquise **par construction** — le masque est figé à
  `INTERNAL|8BIT`, le pool est fini (plafond pathologique ~800 µs) ;
- l'**autolimitation** est **sans objet** : elle existait parce que le hook d'échec
  est déclenchable par rafales depuis une ISR ; ici la fréquence est bornée par
  l'arrivée de requêtes TCP, dont le coût propre domine d'un ordre de grandeur.

**Un cache a été refusé fermement** : le faux négatif d'une garde anti-crash est un
`abort()`, et `heap_metrics.h` documente des oscillations de ~45 Ko entre deux
échantillons de 2 s.

⚠️ **Caveat consigné** : le coût du parcours **croît avec la fragmentation**, donc
il est maximal exactement dans le régime pour lequel la garde existe.

### Tests

33 suites, **1 286 cas**, **7 mutants sur 7 tués**.

Un mutant a corrigé un **raisonnement faux du plan** : « seuil → 12 288 doit tuer le
test *plateau 13 300 servi* » est arithmétiquement impossible (13 300 > 12 288). Le
test qui contraint réellement le seuil dit : *il doit rester de quoi servir un
fichier sans retransmission (~3 850 o) avant que la garde ne morde*, ce qui impose
un seuil ≤ 9 450. **La grandeur qui compte est la marge, pas le seuil.**

### Budget

Flash **1 824 525 → 1 825 717 = +1 192 o** (cible 600), marge **9 291 o**, soit
**99,5 %** occupés. `Retry-After` conservé contre 56 octets : sans lui, un
navigateur en famine rejoue immédiatement, chaque rejeu déclenche un parcours de
tas, et **la garde alimenterait la pression qu'elle est censée laisser retomber**.

⚠️ **Le prochain lot devra arriver avec son propre poste de récupération de flash.**
À 99,5 %, la partition devient la contrainte dominante avant la fragmentation.

Décision consignée : **ADR-0067**. Échelle ADR-0058 amendée d'un 6ᵉ barreau, **les 5
seuils existants intacts** (condition C5 du gate B-1, reconduite en B-2 et ici).

### Reste à vérifier on-target

- Famine forcée sous 8 192 → **exactement 2 lignes** (le front descendant n'a pas
  encore été observé : le test du 05/09 n'a produit que le WARN d'ouverture).
- Tas oscillant autour du seuil pendant plusieurs minutes → pas de paire de lignes
  par oscillation (`kHttpLowHeapEpisodeQuietMs = 30000`).
- POST pompes pendant famine → traité normalement (matcher GET-only).
- Mesure réelle du parcours (`esp_cpu_get_cycle_count` en instrumentation jetable),
  à reporter dans le commentaire de `kStaticFileMinAllocBytes` : c'est **la seule
  dette de mesure** que laisse ce lot.


---

## ⚠️ CRASH DU 05/09 SOUS LA v2.40.0 — un chemin `fopen` que B-3 ne couvre pas

Relevé le 05/09 à 15:35 via `/coredump/info`, **après** le déploiement de la
v2.41.0. Archivé : `builds/2.40.0/esp32dev_core3/coredump_panic_20260905_history.bin`
et `..._decoded.txt`.

```
abort() ← lock_init_generic (newlib locks.c:77) ← __retarget_lock_init_recursive
← __sfp ← _fopen_r ← fopen ← VFSFileImpl ← VFSImpl::open
← fs::FS::open(historyFs, "/history.json", "r", create=false)
```

Tâche **`loopTask`**, core 1, hors contexte d'interruption.

**Datation prouvée, pas inférée** : `esp-coredump` vérifie le SHA256 de l'image.
L'ELF de la **v2.40.0** décode ; celui de la v2.38.0 est **rejeté**
(`coredump SHA256(2895ab0e4) != app SHA256()`). Le crash a donc eu lieu **entre
14:05 et 14:59 le 05/09**, pendant la session de développement.
Corroboration : l'enregistreur de plateau montre **deux discontinuités d'`uptime`
non expliquées**, vers **14:16** et **14:20**, hors de tout déploiement.

**C'est le mécanisme du PANIC fondateur du 29/08, sur un chemin frère :**

| | 29/08 | 05/09 |
|---|---|---|
| Appelant | `AsyncWebServerRequest::send(fs, "text/html")` | `fs::FS::open(historyFs, "/history.json", "r")` |
| Tâche | `async_tcp` | **`loopTask`** |
| Déclencheur | chargement d'interface | **sauvegarde périodique de l'historique** |
| Couvert par B-3 | **oui** | **NON** |

**Le lot B-3 garde les requêtes HTTP GET. Il ne garde rien de ce que le firmware
ouvre pour son propre compte.** Le `code-reviewer` l'avait anticipé pour
`Logger::flushToDisk()` ; le chemin de l'historique n'avait pas été vu, et c'est
lui qui a crashé.

→ Spec de suivi ouverte : **`bug-100`** (recenser TOUS les `fopen` internes et
adopter une stratégie unique, plutôt qu'une rustine par site).

**Ce que ce crash dit du lot B-3** : il ne l'invalide pas — le chemin HTTP est bien
fermé, et la garde a été vue mordre au bon moment le jour même. Mais il **borne sa
portée** : B-3 ferme **un** chemin sur au moins **trois**. Le critère de clôture
« 7 jours sans redémarrage inopiné » du bug-080 reste donc **ouvert**, et il ne
pourra pas être atteint avant `bug-100`.

**Leçon de méthode** : les deux redémarrages sont passés inaperçus pendant deux
heures alors que l'appareil était sous surveillance active. `uptime` seul est un
**mauvais détecteur de redémarrage** quand on n'en lit que les minutes modulo 60.
**Vérifier `/coredump/info` fait désormais partie de tout relevé de diagnostic.**
