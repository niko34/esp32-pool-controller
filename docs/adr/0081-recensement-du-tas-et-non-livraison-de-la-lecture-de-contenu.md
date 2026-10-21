# ADR-0081 — Le tas est recensé par `heap_caps_walk` à la demande depuis la loop ; la lecture de contenu (R3) n'existe dans aucun binaire de release

- **Statut** : Accepté
- **Date** : 2026-09-14
- **Décideurs** : utilisateur (arbitrage du régime R3), `architect` (design), `security-reviewer` (P1 sur le design, **P2** sur le diff — R1 GO, R2/R3 **GO sous conditions**, 13 conditions), `pool-chemistry` (GO sous 4 conditions), `embedded-developer`, `code-reviewer` (Request changes → 8 correctifs appliqués et vérifiés), `doc-writer`
- **Spec(s) liée(s)** : [`feature-128`](../../specs/features/done/feature-128-recensement-du-tas-et-identification-des-intrus.md) (R2/R3), [`feature-127`](../../specs/features/done/feature-127-publier-les-octets-alloues-du-tas.md) (R1, contexte), incident [`2026-09-10-recidive-fragmentation-v2500.md`](../incidents/2026-09-10-recidive-fragmentation-v2500.md)

## Contexte

La fenêtre de mesure du 12-13/09/2026 (voir la note d'incident ci-dessus) a établi **que** le tas se fragmente — le plus gros bloc contigu s'effondre en quatre marches discrètes entre 20,7 h et 28,2 h d'uptime (23 540 → 16 372 → 14 836 → 10 228 → 7 924 octets) — sans établir **qui** fait ces découpes. Les instruments existants (`heap_caps_get_info()`, planchers glissants multi-masques de `feature-122`, compteurs de blocs `fb`/`ab` et octets alloués `a` de `feature-127`) ne rendent que des **agrégats** : combien de trous, combien de blocs, combien d'octets. Jamais où, jamais quoi.

Le premier suspect est le `logger` (`std::vector<LogEntry>` avec `String message`, `logger.h:22,35`) — mais ce sont des allocations de taille **variable**, dépendant du texte du message : aucune constante à retrouver par `grep`, et identifier par la seule taille ne peut pas trancher. Seule une lecture du **contenu** d'un bloc suspect (une `String` de journal se lit en clair) permet de le désigner sans ambiguïté.

Lire le contenu du tas expose potentiellement tout ce qui vit en RAM à un instant donné : mot de passe WiFi, identifiants MQTT, jeton de session, corps de requêtes en vol. Le projet sert en HTTP clair sur le LAN et porte un audit de sécurité encore différé. L'utilisateur a arbitré le 2026-09-14 : **la route de lecture est acceptée, en build de diagnostic, non livrée en production** — « non livré » signifie **absente du binaire de release**, pas « jamais exécutée » : l'appareil exécutera ce build pendant la fenêtre d'observation suivante.

Cet ADR fige les quatre décisions structurantes qui rendent cet arbitrage réel plutôt que déclaratif : le mécanisme d'instrumentation, le protocole d'exécution, le régime de sécurité du recensement (R2), et le mécanisme de non-livraison de la lecture de contenu (R3).

## Décision

### D1 — Instrumentation par `heap_caps_walk`, pas par le traçage natif de l'IDF

Le recensement bloc par bloc (adresse, taille, occupé/libre) utilise **`heap_caps_walk()`**, disponible pour l'ESP32 dans l'IDF 5.5.0 embarqué par pioarduino `55.03.37`, plutôt que le traçage natif `CONFIG_HEAP_TRACING_*` / `CONFIG_HEAP_TASK_TRACKING` de l'IDF.

**Motif, mesuré, pas supposé** : `CONFIG_HEAP_TRACING_OFF=y` est actif et `CONFIG_HEAP_TASK_TRACKING` est **absent** dans le `sdkconfig` des bibliothèques Arduino **précompilées** distribuées par pioarduino `55.03.37`. Rallumer ce traçage imposerait de recompiler ces bibliothèques — donc d'abandonner le paquet **épinglé** qui stabilise ce projet depuis la migration core 3 ([ADR-0042](0042-core3-environnement-par-defaut.md), [ADR-0069](0069-retrait-repli-core-2.md)). Ce n'est pas un choix de préférence : c'est la seule voie qui ne remet pas en cause l'épinglage de plateforme.

`heap_caps_dump()` a été écartée pour un motif distinct : elle écrit sur l'**UART**, et il n'y a **aucun port série** dans l'environnement de développement de ce projet (conteneur LXC, OTA seul — voir `MEMORY.md`, section « Conteneur LXC Debian 13 »). Tout instrument doit donc passer par HTTP.

### D2 — Le parcours du tas n'a jamais lieu sur `async_tcp`

Protocole en **deux temps**, calqué sur l'idiome déjà en place dans ce projet (`POST /debug/sensor_filter_reset` → `requestFilterReset()` consommé en tête de `SensorManager::update()`) :

```
POST /debug/heap_census?used_from=&used_to=   → 202 {queued, seq} | 409 | 429 | 410
GET  /debug/heap_census?kind=free|used&…      → l'instantané GELÉ, paginé
POST /debug/heap_peek?addr=&len=              → 202 [R3, si compilée]
GET  /debug/heap_peek                         → hex + ASCII [R3, si compilée]
```

Machine à états lock-free (`std::atomic<uint8_t>`, transitions par CAS) : `async_tcp` fait `IDLE → REQUESTED` et `READY → READING → IDLE` ; **la loop, et elle seule**, fait `REQUESTED → READY` — c'est elle qui appelle `heap_caps_walk`. Établi par désassemblage indépendant (`nm libheap.a` : `multi_heap.c.o` référence `xPortEnterCriticalTimeout`, `vPortExitCritical`, `xPortInIsrContext`) : la chaîne d'appel `heap_caps_walk → multi_heap_walk → multi_heap_internal_lock → xPortEnterCriticalTimeout … tlsf_walk_pool (callback utilisateur ICI) … multi_heap_internal_unlock → vPortExitCritical` s'exécute **interruptions masquées pendant tout le parcours**, sous un **spinlock inter-cœurs** : une tâche de l'autre cœur qui tente d'allouer pendant le parcours attend elle aussi, ses propres interruptions masquées le temps de l'attente. Sans conséquence pour ce projet (aucun ISR, aucun bit-banging sur ce cœur), mais la propriété est écrite ici plutôt que supposée — exigence explicite de `pool-chemistry`.

Exécuter ce parcours directement sur `async_tcp` aurait masqué les interruptions de la tâche qui sert le WiFi et les requêtes HTTP pendant 1,0 à 1,5 ms par appel, au cœur même du régime de famine étudié. Poser le parcours sur la loop retombe sur une tâche qui masque déjà ~0,7 ms toutes les 2 s pour les planchers glissants de `feature-122` (`heap_metrics.cpp`), et présente un second bénéfice : l'instantané est **déjà gelé** quand la route sérialise, donc les allocations de la réponse HTTP (`JsonDocument`, `AsyncResponseStream`) ne perturbent plus la mesure.

**Chronométrage et auto-désactivation (C1, condition bloquante `pool-chemistry`)** : `esp_timer_get_time()` — jamais le compteur de cycles CCOUNT, qui n'a de sens que pour une mesure locale à un seul cœur sans migration à corriger, ce qui est déjà le cas ici. Le premier parcours dont la durée dépasse `kHeapCensusMaxDurationUs` (1 500 µs) désactive **définitivement** R2 et R3, sur le patron de `oom_guard.cpp:408-418` (`heapIntegrityAdmitNextRun → sProbeDisabled`). Motif : la base de calibrage est **mouvante** — `wmax` (parcours de référence `0x0804`) valait 516 µs au pire en v2.49.0 et 711 µs six jours plus tard, **+38 %**. L'estimation 1,0-1,1 ms du coût de ce recensement repose donc sur une base qui dérive ; l'instrument doit se couper lui-même plutôt que se fier à une estimation périmée. Ce coût est **apériodique**, comptabilisé à part de l'invariant « ≤ 1 parcours O(blocs)/s » des planchers glissants (`constants.h:545-585`) : plafond ajouté de 0,2 parcours/s, **partagé** entre R2 et R3 (même horodatage de dernier parcours), soit `0,75/s + 0,2/s < 1/s`.

**Mémoire des tampons (C2, condition bloquante `pool-chemistry`)** : les tampons statiques du recensement (~1,4 Ko) restent en `.bss` **interne** (RAM classique). `EXT_RAM_BSS_ATTR` est **interdit** ici en commentaire de code : écrire en PSRAM depuis une section à interruptions masquées gonflerait la durée du parcours d'un facteur décisif — c'est précisément le budget que C1 borne à 1,5 ms.

### D3 — Régime de R2 (recensement) : compilé inconditionnellement, `CRITICAL`, plafonné par l'OTA — pas par l'absence d'ASLR

R2 (adresses + tailles, **aucun octet de contenu**) est compilé **inconditionnellement** dans tout binaire, y compris de release, et protégé par `RouteProtection::CRITICAL` avec la garde à deux temps décrite en D4.

⚠️ **L'argument initialement avancé pour ce régime — « pas d'ASLR sur ESP32, l'ELF est publié comme asset de release, donc R2 n'apprend presque rien à un attaquant qui a déjà l'ELF » — est FAUX et écarté.** L'absence d'ASLR rend déterministes les adresses **statiques** (celles que l'ELF documente : fonctions, variables globales). Elle ne rend **pas** déterministes les adresses de **blocs de tas** : elles dépendent de l'historique d'allocation depuis le boot, qui ne figure dans aucun ELF.

**Le bon argument, retenu, est le plafond OTA.** Les mêmes identifiants qui autorisent `GET /debug/heap_census` (`RouteProtection::CRITICAL`) autorisent aussi `POST /update` (`src/web_routes_ota.cpp`, protection `CRITICAL`) — et [`feature-118`](../../specs/features/done/feature-118-authenticite-de-l-image-ota.md) (ouverte, non livrée) établit que l'authenticité de l'image flashée par cette route **n'est pas vérifiée**. Un attaquant qui possède déjà les identifiants CRITICAL a donc **déjà l'exécution de code arbitraire** sur l'appareil via `/update` : lui refuser R2 ne fermerait aucune surface d'attaque réelle, seulement une lecture d'adresses et de tailles que l'exécution de code rend de toute façon accessible par un chemin strictement plus puissant.

**Cette décision est explicitement suspendue à une dépendance, à écrire à chaque relecture de cet ADR : le régime de R2 est à réévaluer quand la vérification d'authenticité de l'image OTA sera livrée.** Le jour où `/update` exige une image signée, l'argument du plafond disparaît et le régime de R2 redevient une question ouverte.

> **Relecture du 2026-10-10 (`feature-118` lot A livré) : l'argument du plafond TIENT, rien ne change ici.** `feature-118` a livré une empreinte SHA-256 **envoyée systématiquement** par les deux appelants officiels (page web, `ota_update.sh`) mais **journalisée seulement** sur `POST /update` (lot B, non livré, transformera ce verdict en refus) — et quand bien même ce refus existerait, l'empreinte reste **fournie par l'appelant**, donc sans effet sur l'argument ci-dessus : un porteur des identifiants `CRITICAL` fournit lui-même l'empreinte qu'il faudrait vérifier, elle ne l'authentifie pas davantage. **Ni le lot A ni le lot B de `feature-118` ne lèvent cette dépendance.** Le déclencheur de réévaluation **se déplace** vers [`feature-151`](../../specs/features/doing/feature-151-signature-de-l-image-ota.md) (signature à clé publique) — c'est elle, et elle seule, qui change la nature de la preuve d'origine sur `/update`. Voir [ADR-0096](0096-empreinte-sha256-obligatoire-sur-post-update.md) pour le détail de ce que `feature-118` prouve et ne prouve pas.
>
> **Relecture du 2026-10-18 (`feature-151` lot D1 livré) : l'argument du plafond TIENT ENCORE, rien ne change ici non plus.** D1 fait **vérifier et journaliser** une signature RSA-3072 sur un manifeste (genre, version, empreinte), mais ⛔ **ne refuse rien** sur ce verdict (`security-reviewer` P1, Q0 : le chemin d'acceptation doit être prouvé sur le terrain — D2 — avant que le chemin de refus — D3 — n'existe, l'OTA étant le seul canal de déploiement du projet). Un porteur des identifiants `CRITICAL` peut donc toujours flasher une image non authentique via `/update` : D1 ne ferme **aucune** surface. Le déclencheur de réévaluation **reste** [`feature-151`](../../specs/features/doing/feature-151-signature-de-l-image-ota.md), mais déplacé plus précisément vers **son lot D3** (refus effectif) — pas D1. Voir [ADR-0120](0120-signature-rsa-du-manifeste-ota-verifiee-avant-refus.md).

### D4 — R3 (lecture de contenu) : non-livraison par macro absente de tout fichier versionné, garde d'authentification renforcée

**Mécanisme retenu, à trois niveaux.**

1. **R3 entier est sous compilation conditionnelle** : `#if defined(POOL_HEAP_PEEK) && POOL_HEAP_PEEK == 1` dans `src/heap_census.h`/`.cpp` et `src/web_routes_heap.cpp` — sans cette macro définie **au build**, le code n'existe pas dans l'image, la route `/debug/heap_peek` n'est jamais enregistrée et répond **404**.

2. **`POOL_HEAP_PEEK` n'est écrite dans aucun fichier versionné** — ni `platformio.ini`, ni `constants.h`, ni aucun `.h`/`.cpp`. Le build de diagnostic se fabrique exclusivement par variable d'environnement, sans modifier un seul fichier du dépôt :

   ```bash
   PLATFORMIO_BUILD_FLAGS=-DPOOL_HEAP_PEEK=1 pio run -e esp32dev_core3
   ```

   **Ceci est le seul endroit du dépôt où cette commande peut être écrite en clair** (le périmètre de `tools/check_no_peek_flag.sh`, ci-dessous, exclut explicitement `docs/`, `specs/` et tout `*.md`, précisément pour que la documentation puisse la porter). Env matériel unique préservé (`esp32dev_core3`, [ADR-0042](0042-core3-environnement-par-defaut.md)/[ADR-0069](0069-retrait-repli-core-2.md)) — aucun second environnement PlatformIO n'est créé, `platformio.ini` n'est jamais réécrit.

3. **Deux sentinelles indépendantes, à deux niveaux distincts**, parce qu'une seule ne suffit à rien prouver seule :

   - `tools/check_no_peek_flag.sh` (4ᵉ job d'`hygiene-specs.yml`, sur **tous les commits**) : échoue si `-DPOOL_HEAP_PEEK`, `#define POOL_HEAP_PEEK` ou `POOL_HEAP_PEEK=` apparaît dans un fichier versionné hors `docs/`, `specs/`, `*.md`. Cette sentinelle vérifie l'**arbre**.
   - `tools/check_release_artefacts.sh` (appelé par `.claude/commands/release.md`, entre la compilation et la publication) : `grep -a -c '/debug/heap_peek'` sur le `.bin` **et** l'`.elf` fraîchement compilés doit rendre `0`. Cette sentinelle vérifie l'**artefact**.

**Pourquoi les deux sont nécessaires — le mécanisme change de nature, pas seulement de sévérité.** `tools/check_no_peek_flag.sh` ne prouve **rien** sur le binaire réellement publié : `/release` compile **localement**, dans l'environnement de l'appelant (`pio run` sans `-e` explicite, `default_envs = esp32dev_core3`) — une variable `PLATFORMIO_BUILD_FLAGS` résiduelle du shell qui lance la commande publierait R3 sans qu'aucun fichier du dépôt n'ait changé, donc sans que la CI ne voie rien. Seule l'inspection de l'artefact (`check_release_artefacts.sh`) constitue une garantie sur ce qui sera réellement attaché à la release ; le contrôle d'arbre reste une **défense en profondeur** contre l'inadvertance (quelqu'un qui commit la macro par erreur), pas une preuve de non-livraison.

**Pourquoi `grep -a -c` sur la chaîne de chemin, et pas `nm` sur la table de symboles.** Sous `-Os`, un lambda de handler s'inline et son nom devient manglé et potentiellement absent de la table de symboles : `nm firmware.elf | grep heap_peek` rendant un résultat **vide peut être un FAUX NÉGATIF** — le pire cas possible pour une preuve de non-livraison, puisqu'il donne une fausse assurance. La chaîne de chemin `"/debug/heap_peek"`, elle, **doit** exister en `.rodata` pour que `server->on("/debug/heap_peek", …)` enregistre la route auprès d'`AsyncWebServer` : ni l'inlining ni le renommage du lambda ne la font disparaître de l'image. Grepper cette chaîne teste donc une propriété que le compilateur ne peut pas masquer, contrairement à un nom de symbole.

**Ce que ce mécanisme ne prouve PAS — à énoncer explicitement, pas à laisser croire couvert :**

- **Il ne prouve pas l'intention.** Quiconque a les droits de commit peut modifier `tools/check_no_peek_flag.sh` ou `tools/check_release_artefacts.sh` dans le **même commit** qui introduit ce qu'ils sont censés bloquer. Ces sentinelles protègent de l'**inadvertance** (un `-D` oublié, une variable d'environnement qui traîne dans un shell), jamais d'un contournement délibéré — même discipline assumée que `tools/check_fopen_guard.sh` ([ADR-0070](0070-fopen-interne-helper-unique-et-classes-de-contention.md)).
- **`tools/check_no_peek_flag.sh` seul ne prouve pas l'artefact** : voir ci-dessus, c'est tout le motif de l'existence de `check_release_artefacts.sh` comme second contrôle distinct.
- **Le budget `kHeapPeekMaxBytesPerBoot` (8 192 o cumulés) ne borne pas l'exfiltration d'un secret isolé.** Un mot de passe WiFi, des identifiants MQTT ou un jeton d'API tiennent en moins de 200 octets — très en dessous du budget. Ce budget borne un **vidage en masse**, rien de plus. Et `POST /reboot` (qui réarmerait le budget) est protégé par les **mêmes** identifiants `CRITICAL` que R3 : si l'authentification tombe, le budget tombe avec elle. La phrase « borne l'exfiltration même si l'authentification tombe », présente dans une version antérieure du design, est **fausse** et a été retirée — la constante reste utile contre un script qui boucle, pas contre un attaquant déterminé.

**Authentification renforcée, exigée pour R3 (S3, condition security-reviewer non négociable).** Chaque handler de `src/web_routes_heap.cpp` — R2 comme R3, par uniformité — commence, **avant toute autre instruction**, par :

```cpp
if (!authManager.isEnabled() || authManager.isFirstBootDetected()) {
  sendJsonError(req, 403, "forbidden");
  return;
}
REQUIRE_AUTH(req, RouteProtection::CRITICAL);
```

Motif : `RouteProtection::CRITICAL` est **décoratif** dans ce projet — `auth.cpp:204` (`checkAuth`) ne teste `level` que contre `RouteProtection::NONE`, donc `CRITICAL` se comporte exactement comme `WRITE`. Et `auth.cpp:199` fait passer **toutes** les routes en clair dès que `authManager.isEnabled()` est faux (mode `auth_enabled = false`, écrit par `POST /save-config`) ou au premier démarrage (`authManager.isFirstBootDetected()`, avant que l'assistant de configuration n'ait tourné). Sans ce refus 403 explicite et antérieur, une route qui lit la mémoire du processus serait **ouverte à quiconque** dès que l'authentification est désactivée — ce qui est vrai de toute route `CRITICAL` de ce projet aujourd'hui, mais que ce lot refuse d'hériter pour R2/R3. Ce garde-fou n'existant nulle part ailleurs dans le projet, une spec de durcissement séparée est ouverte pour l'étendre à `/update`, `/reboot` et `/save-config` (voir Références).

La forme du handler est elle-même contrainte : `server->on(chemin, méthode, UN SEUL lambda)` à **trois arguments** — aucun `onBody`, aucun `onUpload`. Un handler de corps exécuté avant la garde a déjà été un défaut réel de ce projet (07/09/2026) ; sous cette forme, ce défaut devient **inexprimable**, vérifié par `tools/check_heap_routes_guard.sh`.

## Alternatives considérées

### Pour le mécanisme de non-livraison de R3

- **Second environnement PlatformIO** (`[env:esp32dev_core3_diag]`) — **rejetée**. Viole l'environnement matériel unique posé par [ADR-0042](0042-core3-environnement-par-defaut.md)/[ADR-0069](0069-retrait-repli-core-2.md), et tout `pio pkg install`/`uninstall` nécessaire à sa maintenance réécrirait `platformio.ini` en supprimant ses commentaires.
- **Drapeau de configuration au runtime** (une clé NVS ou un paramètre `/save-config` qui active/désactive R3) — **rejetée, et ce n'est pas une nuance mineure**. Avec un drapeau, le code de R3 reste **dans le binaire** de production : l'interrupteur lui-même devient une surface d'attaque (le contourner, le forcer, l'oublier activé). Avec une macro absente de l'arbre, le code de R3 **n'existe pas** dans l'image compilée ; le seul chemin pour l'y faire exister est de compromettre le **poste de build**, pas l'appareil déployé. La garde change de machine. C'est exactement la distinction que la spec `feature-128` exige (« une garde qui se contourne par un drapeau de configuration n'est pas une garantie de non-livraison »).
- **`#ifdef DEBUG`** — **rejetée**. `DEBUG` est un nom de macro à sémantique partagée avec d'innombrables bibliothèques et outils ; un tiers (ou un futur mainteneur pressé) peut la redéfinir globalement sans avoir l'intention d'activer R3 spécifiquement. `POOL_HEAP_PEEK` est un nom de macro **dédié**, dont l'apparition dans un `grep` ne peut avoir qu'une seule origine.

### Pour l'instrumentation (D1)

- **Rallumer `CONFIG_HEAP_TRACING_*`/`CONFIG_HEAP_TASK_TRACKING`** — **rejetée**, motif mesuré : ces options sont désactivées dans le `sdkconfig` des bibliothèques Arduino précompilées de pioarduino `55.03.37` ; les rallumer imposerait de recompiler ces bibliothèques, donc d'abandonner le paquet épinglé.
- **`heap_caps_dump()`** — **rejetée** : écrit sur l'UART, absent de l'environnement de développement (LXC, OTA seul).
- **`heap_caps_walk()`** — **retenue** : disponible sans modification de `sdkconfig`, callback par bloc suffisant pour adresses/tailles/contenu.

### Pour le régime de R2 (D3)

- **« Pas d'ASLR + ELF publié en asset de release »** — **rejetée, explicitement, car fausse** : confond adresses statiques (déterministes sans ASLR, documentées par l'ELF) et adresses de blocs de tas (jamais dans aucun ELF, dépendantes de l'historique d'allocation).
- **Compilation conditionnelle de R2, même mécanisme que R3** — envisagée comme repli si `security-reviewer` avait refusé la compilation inconditionnelle. **Non retenue** au verdict final (GO sous conditions sur R2 compilée inconditionnellement), au motif du plafond OTA ci-dessus. Coût du repli documenté au cas où cet arbitrage serait révisé : un reflash de plus pour observer R2, et ~1,4 Ko de `.bss` en moins dans le binaire de production.

### Pour l'exécution du parcours (D2)

- **Exécution directe sur `async_tcp`** — **rejetée** : masquerait les interruptions de la tâche HTTP/WiFi 1,0 à 1,5 ms par appel, précisément la tâche dont la famine est étudiée.
- **Protocole en deux temps, exécution sur la loop** — **retenue** : même idiome que `/debug/sensor_filter_reset` déjà en place, aucun verrou nouveau, instantané gelé avant sérialisation.

## Conséquences

### Positives

- L'instrument qui manquait après la fenêtre du 12-13/09 existe : un recensement bloc par bloc, comparable à `fb`/`ab` pour la concordance, et une lecture de contenu ciblée pour désigner nommément une allocation suspecte.
- La non-livraison de R3 est vérifiée à **deux niveaux indépendants** (arbre en CI sur tous les commits, artefact à chaque release), avec un motif écrit pour chacun et l'un explicitement conçu pour couvrir l'angle mort de l'autre (variable d'environnement résiduelle du shell qui lance `/release`).
- Le régime de verrouillage (spinlock inter-cœurs, interruptions masquées pendant tout le parcours) est **établi par désassemblage indépendant**, pas supposé — vérifiable par un futur relecteur sans reprendre l'analyse.
- L'auto-désactivation définitive (C1) protège contre la dérive de la base de calibrage elle-même : l'instrument ne s'appuie jamais sur une estimation de coût qu'il n'a pas mesurée lui-même.

### Négatives / dette assumée

- Deux sentinelles à maintenir en parallèle (`check_no_peek_flag.sh`, `check_release_artefacts.sh`), chacune contournable par quiconque a les droits de commit dans le même commit qui introduirait le défaut — protection contre l'inadvertance, pas contre l'intention.
- `RouteProtection::CRITICAL` reste décoratif partout ailleurs dans le projet (identique à `WRITE`) ; la garde 403 posée ici est **locale à `src/web_routes_heap.cpp`**, pas une correction du mécanisme d'authentification lui-même — une spec de durcissement séparée est nécessaire pour l'étendre.
- Le régime de R2 dépend d'un fait externe non encore livré ([`feature-151`](../../specs/features/doing/feature-151-signature-de-l-image-ota.md), signature — `feature-118` lot A/B journalise puis refuse une empreinte **auto-fournie**, sans en changer la nature) : tant que cette dépendance n'est pas honorée à sa propre échéance, ce document doit être relu à chaque changement touchant `/update`.
- `kHeapPeekMaxBytesPerBoot` a une portée plus étroite qu'annoncée dans une version antérieure du design ; la phrase erronée a été retirée du code et du design mais aurait pu se propager si cet ADR ne l'avait pas consignée explicitement.

### Ce que ça verrouille

- Toute future route qui lirait la mémoire du processus (pas seulement le tas) devra suivre la **même** discipline de non-livraison : macro jamais écrite dans un fichier versionné, vérifiée à la fois sur l'arbre et sur l'artefact publié — pas seulement l'une des deux.
- Le recensement du tas reste une opération **strictement à la demande**, jamais périodique ; aucun futur code ne doit appeler `heap_caps_walk` (ou tout parcours O(blocs) équivalent) depuis `async_tcp` ou une ISR dans ce projet.
- Le régime de sécurité de R2 (compilation inconditionnelle) est **conditionné** à l'absence de vérification d'authenticité OTA — ce n'est pas un acquis permanent, c'est un arbitrage daté qui expire quand `feature-151` (signature) sera livrée, pas quand `feature-118` (empreinte auto-fournie) le sera.
- `kHeapCensusMaxDurationUs` et son auto-désactivation définitive (jamais réarmée) deviennent le patron de référence pour tout futur instrument qui parcourt une structure protégée par un verrou à interruptions masquées.

## Références

- Code : [`src/heap_census.h`](../../src/heap_census.h), [`src/heap_census.cpp`](../../src/heap_census.cpp), [`src/heap_census_logic.h`](../../src/heap_census_logic.h) (module pur, [ADR-0017](0017-logique-metier-pure-humble-object-testabilite.md)), [`src/web_routes_heap.h`](../../src/web_routes_heap.h), [`src/web_routes_heap.cpp`](../../src/web_routes_heap.cpp), [`src/constants.h`](../../src/constants.h) (`kHeapCensus*`, `kHeapPeek*`, `kHeapPoison*`), [`src/auth.cpp`](../../src/auth.cpp) (`checkAuth`, `isEnabled`, `isFirstBootDetected`)
- Sentinelles : [`tools/check_no_peek_flag.sh`](../../tools/check_no_peek_flag.sh), [`tools/check_release_artefacts.sh`](../../tools/check_release_artefacts.sh), [`tools/check_heap_routes_guard.sh`](../../tools/check_heap_routes_guard.sh), [`.github/workflows/hygiene-specs.yml`](../../.github/workflows/hygiene-specs.yml) (job 4 et job 5), [`.claude/commands/release.md`](../../.claude/commands/release.md) (étape 2bis)
- Spec : [`specs/features/done/feature-128-recensement-du-tas-et-identification-des-intrus.md`](../../specs/features/done/feature-128-recensement-du-tas-et-identification-des-intrus.md), [`specs/features/done/feature-127-publier-les-octets-alloues-du-tas.md`](../../specs/features/done/feature-127-publier-les-octets-alloues-du-tas.md), [`specs/features/done/feature-118-authenticite-de-l-image-ota.md`](../../specs/features/done/feature-118-authenticite-de-l-image-ota.md) (lot A livré, ne lève pas la dépendance de D3), [`specs/features/doing/feature-151-signature-de-l-image-ota.md`](../../specs/features/doing/feature-151-signature-de-l-image-ota.md) (dépendance réelle de D3, relecture du 2026-10-10)
- Doc externe : [`docs/API.md`](../API.md#recensement-du-tas--lecture-de-contenu-feature-128) (routes `/debug/heap_census`, `/debug/heap_peek`), [`docs/subsystems/system-health.md`](../subsystems/system-health.md), [`docs/incidents/2026-09-10-recidive-fragmentation-v2500.md`](../incidents/2026-09-10-recidive-fragmentation-v2500.md) (contexte de l'incident)
