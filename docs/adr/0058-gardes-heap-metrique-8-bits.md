# ADR-0058 — Les gardes heap basculent sur la métrique 8 bits : l'immunité vient du temps, pas du seuil

- **Statut** : Accepté
- **Date** : 2026-09-02
- **Décideurs** : `debugger` (diagnostic coredump), `architect`, `pool-chemistry` (**gate GO du 02/09 + contre-visa GO** — révoque son propre NO GO d'[ADR-0040](0040-pas-de-calibrage-sur-fragmentation-pathologique.md)), `code-reviewer`, utilisateur
- **Spec(s) liée(s)** : `specs/features/doing/bug-080-redemarrage-panic-et-fragmentation-heap.md` (lot A, v2.33.0)

## Contexte

### La cause racine du 15/08 est restée à moitié traitée

[ADR-0038](0038-metrique-memoire-allouable-8-bits.md) a établi que `ESP.getMaxAllocHeap()` / `ESP.getFreeHeap()` mesurent `MALLOC_CAP_INTERNAL`, **IRAM 32 bits comprise** — de la mémoire qu'aucun `malloc`/`new` ne peut servir. Sur cet appareil, la mesure brute possède un **plancher infranchissable ~27 636 o** (reliquat d'IRAM figé) : toute garde comparant un seuil < 27 636 à cette mesure est **morte par construction**.

Or quatre gardes comparaient encore la mesure brute : le redémarrage d'auto-guérison (`kRestartMaxAllocHeapBytes`), le WARN heap + flush one-shot (`kLowHeapWarnBytes`), le saut des pushes WebSocket (`kWsPushMinAllocBytes`) et la garde 503 de `/get-logs` (`sz + kHttpHeapMarginBytes` vs maxBloc). La preuve de leur cécité est double :

- **PANIC du 29/08** (coredump décodé) : `abort()` dans `lock_init_generic` au service d'une page HTML — un `malloc` de ~90 o refusé en famine 8 bits totale, **sans une seule alerte préalable** ;
- **famine longue du 02/09** : ~9 min sans pouvoir allouer, **1 466 échecs d'allocation comptés**, journal muet côté gardes.

### Le NO GO v2.20.2 avait raison — avec ses données

[ADR-0040](0040-pas-de-calibrage-sur-fragmentation-pathologique.md) : le gate `pool-chemistry` avait **interdit** cette bascule. À l'époque, le régime 8 bits mesuré donnait `minMaxBloc8_boot = 3 060 o` — un seuil d'auto-guérison 8 bits à 8 192 o (plancher de la règle d'ADR-0038) aurait rendu le **mode maintenance (dosage suspendu) atteignable en régime normal** : reboots en boucle en quelques minutes. Le refus était fondé.

### Les données ont changé

Relevés 2.26 → 2.32 (signal débruité depuis la v2.20.2 — les échecs auto-infligés du réarmement ont disparu) :

- **régime nominal fragmenté STABLE : `maxBloc8` = 21 492 o** (valeur identique à l'octet près sur 2.26.0 et 2.32.0 — fragmentation structurelle, pas événementielle) ;
- **creux transitoires jusqu'à 1 268 o**, mais de **quelques secondes** seulement ;
- **famine longue du 02/09** : le seul épisode où les creux durent des minutes — précisément celui qu'il faut détecter.

## Décision

**Les quatre gardes heap basculent sur `heapLargestFreeBlock8Bit()`** (v2.33.0), avec une échelle recalibrée cohérente, en une seule métrique :

| Seuil (o) | Constante | Effet |
|---|---|---|
| **2 048** | `kOomFallbackRearmMinMaxAllocBytes` | Réarmement réserve de **repli** (inchangé) |
| **4 096** | `kRestartMaxAllocHeapBytes` | Reboot d'**auto-guérison** (était 8 192 en brute) |
| **6 144** | `kWsPushMinAllocBytes` | Pushes **WS sautés** — ≈ 2,4× la trame pire cas 2 552 o (était 16 384 en brute) |
| **12 288** | `kLowHeapWarnBytes` | **WARN** heap + flush one-shot (était 20 000 en brute) |
| **18 432** | `kOomRearmMinMaxAllocBytes` | Réarmement réserve **nominale** (était 24 576) |
| 21 492 | — (documentaire) | **Nominal terrain** en régime fragmenté stable |

La garde 503 de `/get-logs` reste **dynamique** (`measureJson + kHttpHeapMarginBytes`) : seule la mesure comparée change.

### L'argument central du gate : l'immunité vient du TEMPS, pas du seuil

Le NO GO reposait sur le risque de faux positifs sur les creux transitoires. La révocation repose sur ce constat : **aucun seuil statique > 1,3 Ko n'est sous tous les creux réels** (minimum observé : 1 268 o). Chercher un seuil « sous les creux » est sans issue — c'est le **débounce temporel** qui discrimine : l'auto-guérison exige **3 mesures consécutives** sous seuil (cadence 60 s nominale, **5 s dès le passage sous le WARN**), soit une famine **continue ≥ 10-15 s** avant tout redémarrage. Un creux de quelques secondes ne compte jamais 3 mesures ; la famine de 9 min du 02/09 en aurait compté des dizaines. Le mode maintenance reste inatteignable en régime normal — l'invariant d'ADR-0040 est préservé par le temps, plus par le niveau.

### L'amendement du gate : réarmement 16 384 REFUSÉ → 18 432

Le lot A proposait 16 384 pour le réarmement nominal. Refusé par arithmétique : allouer la réserve nominale de **12 288** depuis un bloc de 16 384 laisse un reliquat **< 4 096** — le parachute, en s'armant, ferait passer le tas **sous le seuil d'auto-guérison** : il sèmerait son propre auto-heal. Valeur retenue :

```
kOomRearmMinMaxAllocBytes = 18 432
                          = kOomReserveBytes (12 288)
                          + kRestartMaxAllocHeapBytes (4 096)
                          + kOomFallbackRearmMinMaxAllocBytes (2 048)
```

Après armement nominal, il reste au moins la marge auto-heal **plus** le palier de repli. Cette arithmétique est **matérialisée par un test natif sur les constantes** (`test/test_native_oom_guard/`) : la casser casse la suite. Et 18 432 < 21 492 (nominal terrain) : le réarmement nominal redevient **atteignable en régime réel** — il était impossible depuis le 13/08.

### Position sur le creux long : le reboot propre vaut mieux que le zombie

Un appareil qui ne peut plus allouer 1,6 Ko pendant 9 min **ne dose plus fiablement** (trames WS mortes, logs en échec, allocations de la régulation à risque). Le redémarrage d'auto-guérison est **propre** : `stopAll()`, compteurs journaliers persistés, alerte MQTT, plafonné à 3 par fenêtre de 15 min, **mode maintenance en terminal** (joignable, dosage suspendu). Depuis v2.27/v2.28, l'**intégrale pH et le feedforward sont persistés** : le coût de convergence d'un reboot a fortement baissé ([ADR-0053](0053-persistance-conditionnelle-integrale-ph.md), [ADR-0055](0055-feedforward-appris-additif-plafonne.md)).

### Cohérence d'affichage (réserve du contre-visa)

Tous les logs santé affichent **`maxBloc8=`** — la même métrique que celle qui décide. Deux corrections d'affichage livrées après contre-visa : le log d'armement du parachute au boot (`main.cpp`) et le WARN « pushes WS sautés » (`ws_manager.cpp`) affichaient encore la mesure brute.

## Alternatives considérées

- **Rester en métrique brute** (rejetée) — gardes prouvées mortes **deux fois** (29/08 et 02/09) : le statu quo n'est pas conservateur, il est aveugle.
- **Seuils d'époque (v2.20.2) transposés en 8 bits** (rejetée) — avec `min = 3 060`, un seuil d'auto-guérison à 8 192 rendait la maintenance atteignable en régime normal. **Le NO GO d'ADR-0040 avait raison AVEC ses données** ; ce sont les données qui ont changé (nominal stable 21 492, creux courts), pas le raisonnement.
- **Mutualiser la mesure avec l'échantillon du ring (2 s)** (rejetée) — fraîcheur insuffisante pour une décision : des effondrements de ~45 Ko ont été observés **entre deux échantillons** ; chaque garde mesure au moment où elle décide.
- **Retenue : bascule des 4 gardes + échelle unique 8 bits + immunité par débounce** — seule option qui rend les gardes opérantes sans rendre la maintenance atteignable.

## Conséquences

### Positives

- **Fin du régime bi-métrique** d'ADR-0040 pour les gardes décisionnelles : une seule échelle, une seule métrique, comparable de bout en bout.
- La prochaine famine longue déclenche un **reboot propre journalisé** au lieu d'un PANIC silencieux.
- Le parachute **se réarme nominalement** en régime fragmenté réel (18 432 < 21 492).
- Les logs (`maxBloc8=`) disent la métrique qu'ils mesurent — diagnostic auto-descriptif.

### Négatives / dette assumée

- Un creux **anormalement long mais bénin** (> 15 s sous 4 096) déclencherait un reboot — assumé : un tel régime n'est de toute façon pas fiable pour doser.
- **Hors périmètre consigné** : `web_routes_debug.cpp:55` (garde 503 de la trace capteurs) et `web_routes_ota.cpp` (garde TLS `kCheckUpdateMin*`, `/get-system-info`) restent en métrique **brute** — suivi lot B ou spec dédiée.
- L'**origine de la fragmentation** (36 852 → 21 492) reste non identifiée — objet du lot B (instrumentation + déport PSRAM).

### Ce que ça verrouille

- **Toute nouvelle garde heap lit `heapLargestFreeBlock8Bit()`** — plus jamais la mesure brute pour une décision.
- **L'échelle est un tout ordonné** : `2 048 < 4 096 < 6 144 < 12 288 < 18 432 < nominal terrain` — aucun seuil ne se modifie isolément ; l'arithmétique du réarmement (somme des trois constantes) est verrouillée par test.
- **Un seuil de redémarrage se défend par son débounce**, pas par sa distance aux creux : toute modification du débounce (3 mesures, cadence 5 s sous WARN) ré-ouvre le gate `pool-chemistry`.
- Le principe d'ADR-0040 reste en vigueur : pas de recalibrage sans **signal exogène prouvé** — c'est ce qui a été fait ici (relevés 2.26→2.32 débruités).

## Références

- Code : [`src/constants.h`](../../src/constants.h) (bloc « ÉCHELLE COMPLÈTE », constants.h:316), [`src/main.cpp`](../../src/main.cpp) (`checkHeapHealth` — point de mesure unique), [`src/ws_manager.cpp`](../../src/ws_manager.cpp) (`_heapAllowsPush`), [`src/web_routes_data.cpp`](../../src/web_routes_data.cpp) (garde 503 `/get-logs`), [`src/self_heal_logic.h`](../../src/self_heal_logic.h)
- Spec : `specs/features/doing/bug-080-redemarrage-panic-et-fragmentation-heap.md`
- Doc : [subsystems/oom-guard.md](../subsystems/oom-guard.md), [subsystems/system-health.md](../subsystems/system-health.md), [subsystems/ws-manager.md](../subsystems/ws-manager.md), [API.md](../API.md#get-get-logs--write)
- Tests : 1029/1029 natifs verts, 15/15 mutants tués (dont l'arithmétique du réarmement)
- ADR liés : [ADR-0038](0038-metrique-memoire-allouable-8-bits.md) (cause racine), [ADR-0040](0040-pas-de-calibrage-sur-fragmentation-pathologique.md) (**superseded** — NO GO révoqué, doctrine « signal exogène » reprise ici), [ADR-0029](0029-auto-guerison-heap.md) (auto-guérison), [ADR-0035](0035-parachute-oom-new-handler.md) (parachute)
