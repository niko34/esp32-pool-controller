# Bug — Le parachute OOM ne se réarme jamais + post-mortem d'allocation manquant

- **Type** : bug (format minimal `specs/modele.md`)
- **Statut** : **livré en v2.18.8 — tests verts, revue OK ; validation on-target restante** (voir
  « Validation on-target restante » en fin de document). Spec maintenue en `doing/`
  tant que les 2 tests exigés par `pool-chemistry` n'ont pas été passés sur l'appareil.
- **Version cible** : 2.18.8
- **Créée** : 2026-08-13 (2ᵉ crash en production, v2.18.7, coredump décodé)
- **Domaine** : Firmware (`src/oom_guard*`, `src/constants.h`, `src/main.cpp`,
  éventuellement `src/heap_ring*`) — aucun chemin de dosage
- **Specs liées** : bug-oom-accept-http-durcissement (v2.18.5, parachute),
  bug-garde-tls-trop-permissive-sans-psram (v2.18.6), ADR-0035, bug-heap-toctou
  (anneau RTC)

## Symptôme (13/08/2026 ~21:37, uptime 12,9 h)

Abort dans `async_tcp` en servant un **fichier statique** de l'interface :
`AsyncStaticWebHandler::handleRequest` (WebHandlers.cpp:263) →
`new AsyncFileResponse(...)` (**156 octets**) échoue → `throw bad_alloc` →
`__cxa_allocate_exception(100)` échoue AUSSI → `std::terminate` → abort.

**L'anneau RTC (2 s avant) montre un tas SAIN** : `free=81408`, `maxB=27584`,
`ws=0`, `skip=0`, **stables à l'octet près depuis 78 s**. Piles saines
(`async_tcp` 2176/14196). Un malloc de 156 o ne peut pas échouer dans cet état.

## Défaut n°1 — CONFIRMÉ : le parachute est one-shot en pratique

Pour que libstdc++ lève à `new_op.cc:54`, il faut `get_new_handler() == nullptr`
— donc notre handler s'était **désenregistré** (état `Depleted`). Or le
réarmement exige `maxBloc >= kOomRearmMinMaxAllocBytes = 32768`, alors que le
**plus gros bloc en régime établi vaut 27-30 Ko** sur cet appareil (mesuré :
29 684 o à 33 min d'uptime, 27 584 o à 12,9 h). Le seuil est **au-dessus du
régime normal** : une fois la réserve consommée, elle n'est JAMAIS ré-allouée.
La protection livrée en 2.18.5 est donc inopérante après son premier
déclenchement. Erreur de calibrage introduite par ce projet, à corriger.

## Défaut n°2 — traçabilité insuffisante pour trancher la cause

Deux hypothèses restent ouvertes, non départageables avec l'instrumentation
actuelle :
- **(a) consommateur très rapide** dans l'angle mort de 2 s entre deux
  échantillons de l'anneau (précédent : 60 Ko en ~5 s le 11/08) ;
- **(b) corruption du tas** — la comptabilité annonce 81 Ko libres alors que
  l'allocateur ne sert plus 156 octets.

Limites constatées : le CRITICAL « Réserve OOM consommée » est émis en DIFFÉRÉ
par la loop → **perdu** quand l'abort suit immédiatement ; le journal persistant
d'avant crash est perdu aussi (flush toutes les 10 min). Seul l'anneau RTC survit.

## Correctif attendu

1. **Réarmement réaliste** : abaisser/repenser `kOomRearmMinMaxAllocBytes` (ordre
   16 Ko, ou exprimé relativement à `kOomReserveBytes` + marge, à trancher par
   l'architect au vu des mesures ci-dessus). Envisager une **réserve de repli
   plus petite** si le bloc nominal n'est pas allouable, pour que le parachute
   soit toujours armé plutôt que jamais.
2. **Post-mortem d'allocation en RTC NOINIT** (le livrable décisif) : depuis le
   callback `heap_caps_register_failed_alloc_callback` **déjà en place**,
   enregistrer l'état **au moment exact de l'échec** — taille demandée, heap
   libre, plus gros bloc, horodatage (uptime s), compteur — puis le **dumper au
   boot suivant** (comme l'anneau). Une seule récidive suffira alors à départager
   (a) et (b). ⚠️ Point de conception à valider : ce callback est appelé depuis
   l'intérieur de l'allocateur ; vérifier que `heap_caps_get_free_size()` /
   `heap_caps_get_largest_free_block()` y sont sûrs (réentrance/verrous), sinon
   se limiter aux données disponibles sans réentrer dans l'allocateur.
3. **Drapeau RTC « réserve consommée »** posé par le handler lui-même, pour que
   l'information survive à un abort immédiat (le log différé ne suffit pas).
4. **Contrôle d'intégrité périodique** du tas (`heap_caps_check_integrity_all`)
   à cadence lente depuis la loop, pour détecter l'hypothèse (b) — avec
   `esp_task_wdt_reset()` autour (le parcours du tas peut durer), et log une
   seule fois en cas d'échec.

## Critères d'acceptation

- [x] Le parachute se réarme dans le régime mémoire RÉEL de l'appareil
      (maxBloc 27-30 Ko) — seuil ramené à 24 576 o (= 2 × `kOomReserveBytes`)
      + palier de repli 2 048 o dès 12 288 o. Log de réarmement enrichi du
      palier obtenu et du maxBloc. Confirmation on-target : voir ci-dessous.
- [x] Un échec d'allocation produit, au boot suivant, une ligne exploitable
      donnant l'état du tas AU MOMENT de l'échec (`oomPostMortemDumpIfBoot()`,
      ligne unique + verdict, appelée juste après `heapRingDumpIfCrash()`)
- [x] Aucune allocation, aucun mutex bloquant, aucun log dans le handler ni dans
      le callback (contrainte ADR-0035 inchangée) — **renforcé** : aucun appel
      `heap_caps_*` non plus (deadlock), d'où le miroir de l'anneau
- [x] Budget RTC NOINIT documenté (auto-heal 9 o + anneau 1032 o + post-mortem
      32 o = **1073 o sur 8192**) — `docs/subsystems/system-health.md`
- [x] `pio run` core 2 + core 3 verts (flash 83,2 % / 93,0 %, RAM 20,0 % /
      21,1 %), `pio test -e native` **507 tests** (+41) sans régression, logique
      nouvelle intégralement testée en natif (ADR-0017)

## Notes d'implémentation (v2.18.8)

### Valeurs retenues

| Constante | Valeur | Justification |
|---|---|---|
| `kOomRearmMinMaxAllocBytes` | 24576 | = 2 × `kOomReserveBytes` ; atteignable dans le régime réel mesuré (27-30 Ko) |
| `kOomFallbackReserveBytes` | 2048 | 8× le duo fautif du 13/08 (156 + 100 o) |
| `kOomFallbackRearmMinMaxAllocBytes` | 12288 | Au-dessus du seuil d'auto-guérison (8192), sous le WARN heap (20000) |
| `kOomCorruptionMaxFails` | 8 | Comparaison **stricte** (9 échecs déclenchent) |
| `kOomCorruptionBurstSpanS` | 60 | Fenêtre de rafale (ajoutée en revue, voir plus bas) |
| `kOomCorruptionSaneBlockRatio` | 4 | maxBloc ≥ 4 × taille demandée = comptabilité saine |
| `kHeapIntegrityCheckIntervalMs` | 300000 | 5 min ; durée mesurée attendue 1-3 ms (`CONFIG_HEAP_POISONING_LIGHT`) |
| `kHeapIntegrityWarnDurationUs` / `kHeapIntegrityMaxDurationUs` | 5000 / 20000 | WARN / désactivation définitive |

### Format du post-mortem

3ᵉ zone RTC noinit, magic `0xF00DFA11` (distinct de l'anneau `0xB10C4EA9` et de
l'auto-guérison `0xA5C0FFEE`), **32 octets** vus comme 8 mots alignés :
`magic | failCount | firstSize | lastSize | lastCaps | firstUptimeS+lastUptimeS |
(ringFree>>8)+(ringMaxB>>6) | ringUptimeS+flags+releaseCount`. Le **drapeau
« réserve consommée » demandé au §3 de la spec EST l'octet `flags`** (bit0
nominale, bit1 repli, bit2 handler désenregistré, bit3 intégrité échouée, bit4
intégrité jamais exécutée) — pas de variable RTC séparée : une zone, un dump, un
budget. Dump en **une ligne** avec verdict, juste après celui de l'anneau.

### Verdict de sûreté du callback (point ouvert du §2 de la spec)

`heap_caps_get_free_size()` / `heap_caps_get_largest_free_block()` sont
**INTERDITS** depuis `heap_caps_register_failed_alloc_callback` : le verrou du
composant heap est déjà détenu par l'allocateur au moment de l'appel → tout appel
réentrant est un **deadlock**. Conséquence de conception : le chemin d'échec ne
mesure jamais le tas ; sa seule source « free / maxBloc » est un **miroir
atomique du dernier échantillon de l'anneau** (`oomGuardNoteHeapSample()`,
valeurs déjà mesurées, cadence 2 s **inchangée**, aucun parcours de tas
supplémentaire). Horloge du chemin d'échec : `xTaskGetTickCount()` (lecture de
`xTickCount`, sans verrou) — le miroir utilise la **même** horloge pour que le
calcul d'âge reste juste.

### Budget RAM RTC noinit

auto-guérison 9 o + anneau heap 1032 o + post-mortem 32 o = **1073 o sur 8192**.

### Corrections issues de la revue

1. **Fenêtre de rafale** ajoutée au verdict : sans borne temporelle, quelques
   échecs bénins étalés sur des heures concluaient « famine » sur le scénario
   même que le verdict doit trancher (13/08 : 2 échecs, tas sain).
2. **Validité indépendante de l'ordre des uptimes** : le contrôle
   `lastUptimeS ≥ firstUptimeS` rendait le dump **muet** après un repliement
   16 bits (18 h 12), donc précisément dans le régime des deux crashs. Les durées
   sont calculées en arithmétique modulo.
3. **Instantané pris avant l'armement des chemins d'échec** : sinon un échec
   pendant `setup()` polluait la zone du boot précédent avant sa lecture.
4. **Pas de double arrêt / double alerte** dans `handleHeapCorruptionDetected()` :
   `performFailSafeRestart()` fait déjà la séquence complète.
5. **Ceinture anti-déclassement** au réarmement : « pointeur vivant + taille 0 »
   (fenêtre de course avec le handler) est interprété comme palier nominal.
6. **`isAnyPumpEnergized()`** ajouté : les indicateurs de dosage ne couvraient que
   la régulation automatique — injection manuelle et test de pompe passaient au
   travers de la garde.

### Périmètre

Firmware seul : aucun fichier `data/`, aucun endpoint HTTP, aucun champ
WebSocket. **La cause racine du crash du 13/08 reste INCONNUE** — ce cycle
instrumente et rétablit la protection, il ne répare pas.

Fichiers : `src/constants.h`, `src/oom_guard_logic.h`, `src/oom_guard.{h,cpp}`,
`src/heap_ring.cpp`, `src/main.cpp`, `src/pump_controller.h`, `src/version.h`,
`test/test_native_oom_guard/test_oom_guard.cpp`.
Docs : [ADR-0037](../../../docs/adr/0037-postmortem-oom-et-integrite-tas.md) (nouveau),
note additive dans ADR-0035, `docs/subsystems/oom-guard.md`,
`docs/subsystems/system-health.md`, `docs/MQTT.md`, `CHANGELOG.md`.

## Validation on-target restante

À faire sur l'appareil avant de basculer la spec en `done/`.

1. **Test d'innocuité du contrôle d'intégrité** (exigé par `pool-chemistry`) :
   après **plus de 3 min sans dosage** (uptime ≥ `kAutoHealMinUptimeMs`), le
   moniteur série doit afficher
   `Contrôle d'intégrité du tas : 1er passage en X µs` — **X attendu < 5000**
   (au-delà, un WARN est émis ; au-delà de 20 000 le contrôle se désactive).
2. **Test de la garde « pompe énergisée »** (exigé par `pool-chemistry`) : lancer
   un **test de pompe** et le maintenir **plus de 6 min** (> 1 cadence de 5 min)
   → **AUCUNE ligne d'intégrité** ne doit apparaître pendant la marche, et le
   contrôle doit **reprendre après l'arrêt** de la pompe. Preuve directe que la
   garde `isAnyPumpEnergized()` fonctionne.
3. **Réarmement en régime réel** : provoquer/attendre un épisode et vérifier le
   log `Parachute OOM réarmé`/`surclassé` avec le palier et le `maxBloc`.
4. **Post-mortem** : à la prochaine occurrence d'un échec d'allocation, vérifier
   la ligne `Post-mortem OOM : …` au boot suivant et son verdict.
