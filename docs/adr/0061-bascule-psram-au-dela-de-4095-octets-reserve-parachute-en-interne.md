# ADR-0061 — Les allocations de plus de 4095 octets partent en PSRAM ; la réserve du parachute reste en RAM interne explicite

- **Statut** : Accepté
- **Date** : 2026-09-05
- **Décideurs** : `debugger` (diagnostic lot B du 04/09), `architect`, `pool-chemistry` (**gate GO sous 5 conditions**), `code-reviewer` (Request changes puis correctifs), utilisateur
- **Spec(s) liée(s)** : `specs/features/done/bug-080-redemarrage-panic-et-fragmentation-heap.md` (lot **B-1**, v2.35.0)

## Contexte

### Deux PANIC, un seul déclencheur : un chargement d'interface web

Les deux redémarrages sur PANIC de l'appareil de production (**29/08/2026** et **03/09/2026**) ont la même trace décodée, au fichier près :

```
abort() ← lock_init_generic (newlib locks.c:77) ← __retarget_lock_init_recursive
← __sfp ← fopen ← VFSFileImpl ← LittleFS ← AsyncStaticWebHandler::_searchFile
```

Dans les deux cas, le service d'un **fichier statique** (page HTML le 29/08, `/app.js.gz` le 03/09) demande à la libc de créer un mutex interne (~90 o) ; le `malloc` échoue ; `lock_init_generic` aborte **sans recours** — ce chemin passe par `malloc` (C), donc le parachute OOM (`std::set_new_handler`) y est inopérant par construction.

### Le consommateur enfin identifié : le tampon stdio de chaque fichier ouvert

Question ouverte depuis le 15/08 (« qui demande ces allocations de 4096 o ? ») :

- `libraries/FS/src/vfs_api.cpp:22` définit `DEFAULT_FILE_BUFFER_SIZE 4096` ; la ligne `:250` fait `setvbuf(_f, NULL, _IOFBF, DEFAULT_FILE_BUFFER_SIZE)` à **chaque `open()`** ;
- `VFSImpl::exists()` (`vfs_api.cpp:88-101`) **ouvre réellement** le fichier pour tester son existence, et `AsyncStaticWebHandler::_searchFile` (`WebHandlers.cpp:143-152`) enchaîne `exists()` **PUIS** `open()` → **2 × (4096 o + ~90 o de mutex)** par fichier statique servi ;
- un chargement d'interface ≈ **8 requêtes**, jusqu'à **6 en parallèle** sur `async_tcp` ⇒ jusqu'à **~49 Ko** de demande de RAM interne 8 bits **en rafale**, sur un `free8` mesuré à **42-59 Ko**.

### Pourquoi ce tampon restait en RAM interne alors que 4 Mo de PSRAM dorment à côté

Le `sdkconfig` des bibliothèques précompilées (IDF 5.5) porte `CONFIG_SPIRAM_USE_MALLOC=y` et **`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096`**. Le test de `heap_caps_malloc_default` est **inclusif** — `taille <= limite ⇒ RAM INTERNE` (désassemblage de `libheap.a`, section `.iram1.2`, `bgeu a8, a2`).

Avec une limite à 4096, une allocation de **exactement 4096 o** tombe donc du côté **interne** : il s'en fallait d'**un seul octet**. Le message de boot du firmware disait « les allocations > 4 Ko y sont placées automatiquement » — un arrondi en kilo-octets **faux**, qui a masqué le défaut pendant trois cycles de diagnostic.

### Un second défaut, révélé par le premier

`kOomReserveBytes` vaut **12 288 o**, soit **plus** que le seuil de 4096 : depuis l'activation de la PSRAM (feature-066), un `malloc(12288)` ordinaire est déjà tenté **en PSRAM d'abord**. La réserve du parachute OOM part donc **probablement en PSRAM depuis cette date**, sans attendre le présent lot.

> ⚠️ **Hypothèse forte, non encore prouvée.** Le coredump du 03/09 ne capture aucune variable globale (seules les piles de tâches) : il ne permet pas de lire l'adresse de la réserve. C'est la **ligne d'armement du prochain boot** (qui affiche désormais l'emplacement réel) qui tranchera. Ce lot **ne crée pas** ce défaut — il le **révèle** et le corrige.

Un parachute qui libère 12 Ko de **PSRAM** pendant une famine de **RAM interne** ne sauve rien, tout en journalisant « palier nominal » de façon parfaitement crédible.

**Incohérence adjacente** : le réarmement mesure **déjà** `heapLargestFreeBlock8Bit()` (RAM **interne**) et exige 18 432 o **avant** d'appeler `malloc(12288)` — la garde vérifie l'interne, puis alloue peut-être de l'externe. Le correctif rend l'allocation **conforme à la garde qui la gouverne**.

## Décision

**1. Au `setup()`, immédiatement après `initOomGuard()` et sous garde `psramFound()`, le firmware appelle `heap_caps_malloc_extmem_enable(kPsramExtmemThresholdBytes)` avec `kPsramExtmemThresholdBytes = 4095`.** Toute allocation **par défaut** (`malloc`, `new`, `calloc`, `realloc`, `strdup`) de **plus de 4095 octets** est désormais tentée **en PSRAM d'abord**.

Le choix de 4095 est **chirurgical** : la seule classe d'allocations qui change de camp est « exactement 4096 o » — le tampon stdio. Tout ce qui est strictement plus grand partait **déjà** en PSRAM, tout ce qui fait 4095 o ou moins reste en RAM interne comme avant. On ne déplace pas une politique d'allocation : **on ferme une inégalité non stricte**.

**2. En contrepartie obligatoire, les trois allocations de la réserve du parachute — nominale (12 288 o), de repli (1 024 o) et de réarmement — passent par un entonnoir unique** :

```cpp
static void* oomAllocReserveInternal(size_t bytes) {
  return heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}
```

L'unité de compilation `oom_guard.cpp` est protégée par `#pragma GCC poison malloc calloc realloc strdup` : toute réintroduction d'une allocation par défaut y **casse la compilation**. `free` n'est **pas** empoisonné — la réserve est libérée par `free()` (équivalent à `heap_caps_free` sur un pointeur issu de `heap_caps_malloc`), y compris depuis le `new_handler` où aucun appel non trivial n'est souhaitable.

**3. L'invariant « la réserve vit en RAM interne » est OBSERVÉ, jamais supposé** : `oomGuardReserveIsInternalRam()` lit `esp_ptr_internal()` sur le pointeur publié. Il alimente la ligne d'armement du boot, la ligne de réarmement et le champ `oom_reserve_internal` de `/get-system-info` — avec la mention **`PSRAM (ANOMALIE)`** si l'invariant tombe.

**4. Deux `static_assert` verrouillent le couple de constantes** ([`src/constants.h`](../../src/constants.h)) :

```
kPsramExtmemThresholdBytes + 1 == kVfsStdioBufferBytes   // 4095 + 1 == 4096
kOomReserveBytes > kPsramExtmemThresholdBytes            // ⇒ entonnoir INTERNAL obligatoire
```

Le second est une **sentinelle** : il rappelle à la compilation que la réserve dépasse le seuil, donc qu'elle **doit** être allouée en capacités explicites.

### La bascule est FAIL-SAFE : aucun échec nouveau n'est créé

`heap_caps_malloc_extmem_enable` installe une **préférence**, pas une contrainte. L'allocateur tente d'abord les capacités choisies, **se replie sur `MALLOC_CAP_DEFAULT`** si la PSRAM refuse, et n'appelle le hook `heap_caps_alloc_failed` qu'**après** ce repli. Conséquences :

- une PSRAM saturée ou absente ne provoque **aucun échec supplémentaire** — le comportement revient exactement à celui d'avant ;
- les **compteurs OOM ne sont pas pollués** : une tentative PSRAM infructueuse suivie d'un succès interne n'incrémente rien, n'écrit pas le post-mortem RTC et ne déclenche pas le WARN d'échec.

### Appel UNIQUE au setup, jamais à l'exécution

`heap_caps_malloc_extmem_enable` écrit une **variable globale de l'allocateur**, lue par **chaque** `malloc()` du système. L'appeler depuis un chemin d'exécution (une route HTTP, un tick, un handler) changerait la politique d'allocation de **toutes les tâches** de façon non atomique, sans moyen de raisonner sur l'état des allocations en vol. **Un seul site d'appel, dans `setup()`, après `initOomGuard()`** — placement voulu : la réserve du parachute est prise **avant** la bascule (bloc contigu interne, pris avant toute fragmentation), et tout le reste du `setup()` profite déjà de la nouvelle politique.

Aucun log n'est émis à ce point (`systemLogger.begin()` n'a pas encore eu lieu) ; l'état est journalisé quelques lignes plus bas.

## Alternatives considérées

- **Modifier le `sdkconfig` (`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL`)** (rejetée) — **impossible** : les bibliothèques IDF du core Arduino sont **précompilées**. La valeur est figée dans `libheap.a`, aucune option `platformio.ini` ne la reconstruit.
- **Patcher `DEFAULT_FILE_BUFFER_SIZE` dans `vfs_api.cpp`** (rejetée) — fichier **du core Arduino**, hors du dépôt : le patch ne serait pas versionné et serait **réécrasé à chaque mise à jour** de plateforme. Un correctif qui disparaît silencieusement est pire que pas de correctif.
- **`setvbuf(_f, NULL, _IONBF, 0)` sur les fichiers servis** (rejetée) — le `FILE*` est **encapsulé** dans `VFSFileImpl` (membre privé, aucun accesseur) : inaccessible depuis le firmware.
- **Seuil à 0 (« tout en PSRAM »)** (rejetée) — casserait les allocations **DMA** et les chemins **ISR**, qui exigent de la RAM interne : la PSRAM n'est ni adressable par le DMA de tous les périphériques, ni accessible pendant certaines opérations flash. Un seuil aussi large déplacerait une politique, quand il ne s'agit que de corriger une inégalité.
- **Cache PSRAM des assets statiques** (rejetée **pour ce lot**, retenue comme correctif de fond) — c'est la vraie solution structurelle : plus aucun `fopen` sur le chemin de service. Mais elle exige de trancher l'**invalidation** (flash FS, OTA), le **budget PSRAM** et la **cohérence gzip** ; elle est **reportée en spec dédiée**. B-1 doit rester **un seul levier, un seul effet à mesurer**.
- **Ne rien faire et se contenter de la garde 503 du lot B-3** (rejetée) — la garde `serveStatic` refuse la requête **avant** le premier `fopen` : elle ferme un chemin d'`abort()`, mais traite le **symptôme**. Elle reste au programme du lot B-3, en **ceinture**, pas en remplacement.
- **Retenue : bascule à 4095 + entonnoir interne explicite pour la réserve** — le seul couple qui supprime la rafale de ~49 Ko sans changer la nature des autres allocations, et qui rend l'invariant du parachute **observable**.

## Conséquences

### Positives

- La rafale de tampons stdio (~49 Ko par chargement d'UI) quitte la RAM interne : c'est le déclencheur **commun** des deux PANIC qui disparaît.
- **Le parachute redevient (ou devient) réellement utile** : sa réserve est prouvée interne à chaque armement, et l'anomalie est visible dans le journal **et** dans `/get-system-info`.
- L'**allocation** de la réserve devient conforme à la **garde** qui la gouverne (`heapLargestFreeBlock8Bit()` ≥ 18 432 ⇒ allocation interne).
- Le message PSRAM du boot énonce désormais le seuil **en octets, avec sa direction** — l'arrondi trompeur qui a coûté trois cycles ne peut plus se reproduire.
- La PSRAM, jusqu'ici libre à 99,4 %, devient un **pool de travail** : sa consommation est un **signal de charge exploitable** (champ `psram=` de la trace « Heap 8bits: »).

### Négatives / dette assumée

- **La latence de la PSRAM n'a pas été mesurée** sur le service des assets statiques. Un accès PSRAM est plus lent qu'un accès RAM interne ; l'effet sur le temps de chargement de l'interface est **inconnu et non quantifié** à ce jour.
- **Le rebond interne d'`esp_flash_read`** : lire du flash vers un tampon en PSRAM impose au driver un tampon de rebond en RAM interne. Cette pression 8 bits résiduelle est **non quantifiée** — elle pourrait réintroduire une partie du problème, à une taille très inférieure. À surveiller pendant les 48 h d'observation.
- L'assiette de calibrage de l'[ADR-0058](0058-gardes-heap-metrique-8-bits.md) change : le régime de RAM interne après B-1 n'est plus celui sur lequel les seuils ont été calibrés. Les seuils **ne sont pas retouchés** dans ce lot (**un seul levier**), ils seront réévalués après 48 h.
- La branche `ESP_ARDUINO_VERSION_MAJOR >= 3` autour de `esp_memory_utils.h` / `soc_memory_layout.h` est correcte sur le papier mais **non compilée** : l'environnement core 2 n'a pas été construit dans ce cycle (les deux plateformes s'écrasent mutuellement).
- **`psram_free` change de sens** pour tout superviseur : la valeur n'est plus quasi constante, elle **varie avec la charge web**. Une baisse est le comportement **attendu**, pas une anomalie.

### Ce que ça verrouille

- **Aucun `malloc`/`calloc`/`realloc`/`strdup` ordinaire dans `oom_guard.cpp`** — verrouillé par `#pragma GCC poison`. Toute nouvelle allocation de ce module passe par `oomAllocReserveInternal()`.
- **Toute structure devant vivre en RAM interne** (DMA, chemin ISR, réserve d'urgence, tampon de rebond) doit désormais l'exiger **explicitement** par `heap_caps_malloc(..., MALLOC_CAP_INTERNAL | ...)` : la politique **par défaut** n'est plus « interne » au-delà de 4095 o.
- **`heap_caps_malloc_extmem_enable` a un site d'appel UNIQUE**, dans `setup()`. Aucun appel à l'exécution, sous aucun prétexte.
- **Le couple `kPsramExtmemThresholdBytes` / `kVfsStdioBufferBytes` est solidaire** : si un futur core change `DEFAULT_FILE_BUFFER_SIZE`, le `static_assert` casse le build — c'est voulu, le seuil doit être revu **avec** lui.
- **L'emplacement de la réserve est un invariant publié**, pas un détail d'implémentation : la ligne d'armement, la ligne de réarmement et `/get-system-info` en portent l'état. Le supprimer exige de rouvrir le gate `pool-chemistry`.

## Références

- Code : [`src/main.cpp`](../../src/main.cpp) (site d'appel unique dans `setup()`, message PSRAM du boot, ligne d'armement, champ `psram=` de la trace « Heap 8bits: »), [`src/constants.h`](../../src/constants.h) (`kPsramExtmemThresholdBytes`, `kVfsStdioBufferBytes`, `kOomNoReserveCriticalDelayMs`, les deux `static_assert`), [`src/oom_guard.cpp`](../../src/oom_guard.cpp) (`#pragma GCC poison`, `oomAllocReserveInternal`, WARN de déclassement, CRITICAL « sans réserve »), [`src/oom_guard.h`](../../src/oom_guard.h) (`oomGuardReserveIsInternalRam`, `oomGuardConsumeNoReserveAlert`), [`src/mqtt_manager.cpp`](../../src/mqtt_manager.cpp) (drain de l'alerte `oom_reserve_absent`), [`src/web_routes_config.cpp`](../../src/web_routes_config.cpp) (`oom_reserve_bytes` / `oom_reserve_internal`)
- Spec : `specs/features/done/bug-080-redemarrage-panic-et-fragmentation-heap.md` (diagnostic lot B du 04/09, découpage B-1/B-2/B-3, prédiction falsifiable)
- Doc : [subsystems/oom-guard.md](../subsystems/oom-guard.md#politique-dallocation-par-défaut--bascule-psram-v2350), [subsystems/system-health.md](../subsystems/system-health.md#trace-de-calibrage-des-seuils-mémoire-v2190), [API.md](../API.md#get-get-system-info--write), [MQTT.md](../MQTT.md#alertes)
- ADR liés : [ADR-0058](0058-gardes-heap-metrique-8-bits.md) (**assiette de calibrage amendée**, seuils inchangés), [ADR-0035](0035-parachute-oom-new-handler.md) (parachute), [ADR-0038](0038-metrique-memoire-allouable-8-bits.md) (métrique 8 bits), [ADR-0036](0036-psram-requise-mise-a-jour-en-ligne.md) (PSRAM libre requise pour TLS sortant — la PSRAM devenant un pool de travail, son seuil de 1 Mo est à réévaluer hors lot), [ADR-0029](0029-auto-guerison-heap.md) (auto-guérison)
