# ADR-0038 — La mémoire se mesure sur le pool 8 bits réellement allouable, jamais sur `MALLOC_CAP_INTERNAL`

- **Statut** : Accepté
- **Date** : 2026-08-15
- **Décideurs** : Nicolas (verdict architect du cycle `bug-metrique-heap-inclut-iram-32bits`, gate `pool-chemistry` sur la règle de recalibrage et sur le refus de différer un redémarrage pendant une injection)
- **Spec(s) liée(s)** : `bug-metrique-heap-inclut-iram-32bits` (v2.19.0 = mesure, v2.19.1 = bascule des gardes)

## Contexte

**Trois crashs de production en cinq jours**, tous par échec d'une **petite** allocation, tous avec un tas annoncé confortable :

| Date | Version | Allocation refusée | Tas annoncé au moment de l'échec |
|---|---|---|---|
| 11/08/2026 | 2.18.4 | `new AsyncWebServerRequest` (356 o), accept HTTP | effondrement de ~60 Ko en ~5 s |
| 13/08/2026 | 2.18.7 | `new AsyncFileResponse` (156 o) puis objet exception (100 o) | `free = 81 408`, `maxBloc = 27 584`, **stables depuis 78 s** |
| 15/08/2026 | 2.18.9 | descripteur de fichier `esp_littlefs`, puis **verrou UART créé paresseusement** dans le chemin de log → `abort()` | `free = 67 584`, `maxBloc = 27 584` ; 767 échecs sur 36 023 s, du 1ᵉʳ (1 622 o) au dernier (**92 o**, `caps = 0x0804`) |

Les réponses successives (parachute OOM [ADR-0035](0035-parachute-oom-new-handler.md), garde TLS [ADR-0036](0036-psram-requise-mise-a-jour-en-ligne.md), post-mortem + contrôle d'intégrité [ADR-0037](0037-postmortem-oom-et-integrite-tas.md)) ont toutes été **calibrées sur le même indicateur**. Cet indicateur est faux.

`ESP.getFreeHeap()` vaut `heap_caps_get_free_size(MALLOC_CAP_INTERNAL)` et `ESP.getMaxAllocHeap()` vaut `heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)` (`framework-arduinoespressif32/cores/esp32/Esp.cpp:163-173`). Or **`MALLOC_CAP_INTERNAL` englobe l'IRAM accessible en accès 32 bits alignés SEULEMENT**. Cette mémoire ne peut **pas** servir à un `malloc()`/`new` ordinaire : l'allocateur exige alors `MALLOC_CAP_8BIT` (accès octet). L'indicateur du projet mesure donc un pool **strictement plus grand** que celui dans lequel les allocations sont réellement servies.

Trois preuves convergentes, toutes issues de l'instrumentation livrée en 2.18.8 :

1. **Le « plus gros bloc » est figé à 27 584 / 27 636 o depuis des jours, et à travers les redémarrages.** Un bloc DRAM vivant ne se comporte pas ainsi ; un **reliquat d'IRAM constant**, si.
2. **Des allocations de 92 à 2 872 o échouent réellement** pendant que l'indicateur annonce 27 Ko. Il n'y a aucune contradiction à expliquer : la **famine 8 bits est réelle**, l'indicateur ne la voit pas.
3. **L'intégrité du tas est vérifiée OK toutes les 5 min** (2.18.8) : il n'y a pas de corruption. Le verdict `SUSPICION CORRUPTION` du post-mortem, construit sur la règle « comptabilité saine ⇒ corruption », était **construit sur une comptabilité fausse**.

Conséquence directe et grave : **toutes les gardes mémoire du projet sont aveugles depuis leur création.** `kRestartMaxAllocHeapBytes` (8 192 o) en est l'exemple le plus coûteux — l'indicateur ne descendant jamais sous ~27 Ko, le **filet d'auto-guérison n'a jamais pu se déclencher une seule fois**. Sont dans le même cas : `kWsPushMinAllocBytes`, `kLowHeapWarnBytes`, `kCheckUpdateMinMaxAllocBytes` / `kCheckUpdateMinFreeHeapBytes`, `kOomRearmMinMaxAllocBytes` / `kOomFallbackRearmMinMaxAllocBytes`, l'anneau de cinétique, le post-mortem et son verdict.

Il faut trancher maintenant : chaque cycle supplémentaire calibré sur cet indicateur ajoute une garde inopérante de plus, et le piège est **invisible à la lecture du code** (`ESP.getFreeHeap()` a l'air d'être « la mémoire libre »).

## Décision

**La mémoire du projet se mesure sur le pool `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT`** — exactement ce qu'un `malloc()`/`new` peut obtenir. Trois volets :

1. **Un helper centralisé, unique source de la mesure** : [`src/heap_metrics.h`](../../src/heap_metrics.h) / [`.cpp`](../../src/heap_metrics.cpp) expose `heapFree8Bit()`, `heapLargestFreeBlock8Bit()` et `heapMetricsSnapshot()` (les quatre valeurs des deux familles en un appel). Sans log, sans allocation, sans `String` — appelable depuis les chemins critiques. Pas de 24 appels `heap_caps_*` dispersés : une seule définition de « la mémoire disponible ».
2. **Toute GARDE lit la métrique 8 bits.** Il est **interdit** de fonder une décision (reboot, refus, saut de push, réarmement) sur `ESP.getFreeHeap()` / `ESP.getMaxAllocHeap()`. Ces deux fonctions ne subsistent que comme **information brute** dans les surfaces de diagnostic.
3. **Les deux familles restent exposées côté diagnostic**, et l'écart entre elles est rendu visible : `/get-system-info` publie `free_heap_8bit`, `max_alloc_8bit` et `max_alloc_internal` **en plus** de `free_heap` (ajout strictement additif), la trace INFO périodique donne les deux, et l'interface affiche « Mémoire utilisable » avec la mesure brute en note.

**Le déploiement est découpé en deux versions, et ce découpage fait partie de la décision** :

- **v2.19.0 — MESURER.** Le helper est introduit, l'anneau de cinétique et le miroir du post-mortem passent à la mesure 8 bits (**purement observationnel** : aucune décision n'en dépend), les libellés du journal sont suffixés (`maxB8=`, `free8=`), une trace INFO de calibrage est émise toutes les 5 min, `/get-system-info` et l'interface exposent les deux familles. **Aucun seuil de [`constants.h`](../../src/constants.h) n'est modifié, aucune des 9 gardes n'est basculée. Le comportement de l'appareil est strictement inchangé — risque nul.**
- **v2.19.1 — BASCULER.** Après **≥ 24 h de relevés on-target**, les seuils sont recalibrés sur les valeurs réelles puis les gardes basculent **dans l'ordre du risque croissant, l'auto-guérison en DERNIER** (c'est la seule qui provoque un redémarrage).

**Les deux magics de RAM RTC sont bumpés** en 2.19.0 : anneau `0xB10C4EA9` → **`0xB10C4EAA`**, post-mortem `0xF00DFA11` → **`0xF00DFA12`**. Une trame écrite par un firmware 2.18.x porte l'**ancienne** métrique ; la relire avec la nouvelle grille de lecture afficherait un chiffre faux **et crédible**. Perdre la trace du boot de bascule est acceptable ; afficher une valeur trompeuse ne l'est pas.

**Règle de recalibrage retenue pour la 2.19.1** (validée par le gate de sécurité chimique) :

```
kRestartMaxAllocHeapBytes = clamp(0,40 × min(maxBloc8 observé sur 24 h) ; 8192 ; 16384)
```

avec débounce **maintenu à 3** et l'**invariant d'ordre** :

```
restart < rearmOom ≤ kWsPushMinAllocBytes < kLowHeapWarnBytes < min(maxBloc8 sur 24 h)
```

Le gate a par ailleurs **refusé** de différer un redémarrage d'auto-guérison pendant une injection en cours : le PWM des pompes est **matériel**, une boucle figée laisserait la pompe **énergisée**. Différer transformerait une dose tronquée (bénigne, rattrapée au cycle suivant) en **surdosage potentiel**.

## Alternatives considérées

- **Garder `MALLOC_CAP_INTERNAL` et se contenter de relever les seuils** (rejetée) — c'est exactement ce qui a été fait en 2.18.5, 2.18.6 et 2.18.8, et **c'est ce qui a produit trois crashs**. Relever un seuil sur un indicateur qui ne descend jamais ne change rien : le problème n'est pas la valeur du seuil, c'est la grandeur comparée. Cette option n'a été retenue comme candidate que parce qu'elle est la moins intrusive — et c'est précisément ce confort qui a coûté deux cycles.
- **N'exposer qu'une seule valeur (la 8 bits) et supprimer la mesure brute** (rejetée) — tentant pour « ne plus jamais se tromper ». Mais l'**écart entre les deux familles EST l'information** : c'est lui qui rend le piège visible et mémorisable. Sans l'écart affiché, un futur cycle rebranchera innocemment `ESP.getFreeHeap()` (nom naturel, disponible partout, aucun avertissement du compilateur) et le piège se refermera. On garde donc les deux, avec la 8 bits en position principale et la brute en note explicitement qualifiée de « zone technique inutilisable ».
- **Basculer les gardes ET recalibrer les seuils dans la même version** (rejetée) — c'était la réponse « logique ». Elle exige de **deviner** les nouvelles valeurs : le régime 8 bits réel de l'appareil n'a **jamais** été mesuré, et l'auto-guérison est la garde qui **redémarre l'appareil**. Un seuil trop haut posé à l'aveugle produirait des reboots en cascade sur un appareil sain — c'est-à-dire une panne créée par le correctif, sur un système qui pilote des pompes doseuses. Mesurer d'abord, décider ensuite.
- **Conserver les magics RTC et se contenter d'un champ « version de format »** (rejetée) — plus élégant, mais il faudrait alors écrire (et tester) une branche de lecture rétrocompatible pour une trame qu'on ne relira **qu'une seule fois dans la vie du produit**, celle du boot de bascule. Le bump de magic obtient le même résultat en une constante, et l'ancienne trame est simplement ignorée.
- **Retenue : helper centralisé + mesure 8 bits dans toutes les gardes + double exposition + découpage mesure/bascule + bump des magics** — seule combinaison qui corrige la grandeur mesurée, rend le piège durablement visible, et n'introduit aucun risque de comportement tant que les vraies valeurs ne sont pas connues.

## Conséquences

### Positives

- La **cause racine des trois crashs** est identifiée et nommée : il n'y a jamais eu de contradiction entre « 27 Ko libres » et « 92 octets refusés » — les deux chiffres parlaient de deux mémoires différentes.
- Le verdict `SUSPICION CORRUPTION` du post-mortem ([ADR-0037](0037-postmortem-oom-et-integrite-tas.md)) redevient **exploitable** une fois alimenté par la mesure 8 bits : la règle « comptabilité saine ⇒ corruption » n'était fausse que parce que la comptabilité l'était.
- L'auto-guérison ([ADR-0029](0029-auto-guerison-heap.md)) **redeviendra capable de se déclencher** en 2.19.1 — pour la première fois depuis sa création.
- Le piège reste visible : l'écart `max_alloc_8bit` / `max_alloc_internal` est publié par l'API, journalisé toutes les 5 min et affiché dans l'interface.
- La v2.19.0 est **déployable sans risque** : elle n'altère aucune décision de l'appareil.

### Négatives / dette assumée

- **Tous les seuils fixés entre 2.18.5 et 2.18.8 sont à jeter et à refaire.** Les valeurs 8 192 / 12 288 / 16 384 / 20 000 / 24 576 ont été calibrées contre des mesures fausses ; leur **ordre relatif** garde du sens, pas leurs valeurs absolues.
- **La v2.19.0 laisse volontairement le firmware dans un état incohérent** : l'anneau et le post-mortem rapportent la métrique 8 bits pendant que les gardes décident encore sur la métrique brute. C'est **assumé et temporaire** (une version), et c'est écrit dans la documentation des deux sous-systèmes concernés pour qu'aucun lecteur ne prenne cette incohérence pour un oubli.
- **Perte de la trace RTC du boot de bascule** (anneau + post-mortem), conséquence directe du bump des deux magics.
- **La cause de la consommation de mémoire 8 bits reste inconnue** : cet ADR corrige l'**instrument**, pas le phénomène. Identifier le consommateur (pistes : `AsyncStaticWebHandler`, `LittleFS.begin(false)` limité à 10 fichiers ouverts, fuite de `File`) est une phase 2 qui n'avait aucun sens tant que l'instrument mentait.
- Coût permanent de 3 champs JSON (+88 octets mesurés sur `/get-system-info`, budget 716 o sur 1024).

### Ce que ça verrouille

- **Interdiction permanente d'utiliser `ESP.getFreeHeap()` / `ESP.getMaxAllocHeap()` dans une garde** (toute décision de reboot, de refus HTTP, de saut de push, de réarmement). Ces fonctions ne sont admises que dans les surfaces d'information, et toujours accompagnées de la valeur 8 bits correspondante.
- **`heap_metrics` est la source unique de la mesure** : tout nouveau besoin passe par ce module, jamais par un `heap_caps_*` isolé.
- **L'interdiction réentrante d'[ADR-0037](0037-postmortem-oom-et-integrite-tas.md) s'étend au helper** : `heapFree8Bit()`, `heapLargestFreeBlock8Bit()` et `heapMetricsSnapshot()` sont **interdits** depuis `oomFailedAllocHook` et depuis le `new_handler` — le verrou du composant heap y est déjà détenu par l'allocateur. Ces chemins lisent exclusivement le miroir alimenté par `oomGuardNoteHeapSample()`.
- **Les magics RTC `0xB10C4EAA` et `0xF00DFA12` rejoignent la liste des valeurs réservées** (avec `0xA5C0FFEE`), au même titre que leurs prédécesseurs `0xB10C4EA9` et `0xF00DFA11`, qui ne doivent jamais être réemployés.
- **Tout changement de métrique d'une zone RTC noinit impose un bump de son magic** — c'est désormais la règle, pas un cas particulier.
- **L'invariant d'ordre des seuils** (`restart < rearmOom ≤ wsPush < lowHeapWarn < min24h`) fait contrat : aucun de ces cinq seuils ne se modifie isolément, et toute modification passe par un relevé on-target.
- **Un redémarrage d'auto-guérison ne sera jamais différé au motif qu'une injection est en cours** (gate `pool-chemistry`) : PWM matériel, risque de pompe laissée énergisée.

## Références

- Code : [`src/heap_metrics.h`](../../src/heap_metrics.h) / [`src/heap_metrics.cpp`](../../src/heap_metrics.cpp) (helper), [`src/heap_ring.cpp`](../../src/heap_ring.cpp) (anneau + miroir sur mesure 8 bits, magic bumpé), [`src/oom_guard_logic.h`](../../src/oom_guard_logic.h) / [`src/oom_guard.cpp`](../../src/oom_guard.cpp) (post-mortem, magic bumpé, libellés `free8=` / `maxB8=`), [`src/main.cpp`](../../src/main.cpp) (`checkHeapHealth` — trace de calibrage), [`src/web_routes_config.cpp`](../../src/web_routes_config.cpp) (`/get-system-info`), [`src/constants.h`](../../src/constants.h) (seuils **inchangés** en 2.19.0)
- Spec : `specs/features/doing/bug-metrique-heap-inclut-iram-32bits.md`
- Doc : [subsystems/oom-guard.md](../subsystems/oom-guard.md), [subsystems/system-health.md](../subsystems/system-health.md), [API.md](../API.md#get-get-system-info--write), [features/page-settings.md](../features/page-settings.md)
- Amont : `framework-arduinoespressif32/cores/esp32/Esp.cpp:163-173` (définition de `ESP.getFreeHeap()` / `ESP.getMaxAllocHeap()`)
- ADR liés : [ADR-0029](0029-auto-guerison-heap.md) (auto-guérison — seuil jamais atteignable jusqu'ici), [ADR-0035](0035-parachute-oom-new-handler.md) (parachute OOM), [ADR-0036](0036-psram-requise-mise-a-jour-en-ligne.md) (garde TLS), [ADR-0037](0037-postmortem-oom-et-integrite-tas.md) (post-mortem et verdict — comptabilité à réévaluer), [ADR-0017](0017-logique-metier-pure-humble-object-testabilite.md) (logique pure testable)
