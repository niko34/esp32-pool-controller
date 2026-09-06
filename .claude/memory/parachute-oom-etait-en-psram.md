---
name: parachute-oom-etait-en-psram
description: "Le parachute anti-OOM était alloué en PSRAM depuis feature-066 — donc décoratif ; prouvé par l'écart de psram_free au déploiement de la v2.35.0"
metadata: 
  node_type: memory
  type: project
  originSessionId: 82048c5b-7e8f-4cec-8242-ad9b5ed01f91
  modified: 2026-09-04T23:00:58.597Z
---

Le parachute anti-OOM (réserve de 12 288 o libérée par `new_handler` en famine)
était alloué **en PSRAM** depuis l'activation de la PSRAM (feature-066), et non
en RAM interne. Il libérait donc de la mémoire externe pendant des famines de
mémoire **interne** : il ne pouvait rien sauver, tout en journalisant « palier
nominal », vrai sur la taille et faux sur l'utilité.

**Cause** : `malloc(12288)` dépasse `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096`
et le test de l'allocateur est `taille <= seuil ⇒ interne`. Personne n'a rejugé
le sens de ce `malloc()` nu quand la PSRAM est arrivée. Incohérence adjacente :
le réarmement mesurait **déjà** `heapLargestFreeBlock8Bit()` (interne) et exigeait
18 432 o **avant** d'allouer — la garde vérifiait l'interne puis allouait de
l'externe.

**Preuve (05/09/2026, déploiement v2.35.0)** : `psram_free` est passé de
4 169 840 à 4 182 164, soit **+12 324 o de PSRAM LIBRE EN PLUS** alors que le lot
y envoie *davantage* d'allocations. 12 324 = 12 288 (réserve) + 36 o de
métadonnées. Le raisonnement vaut parce qu'aucun autre consommateur PSRAM
n'existe au boot ; `maxBloc8` et `free_heap_8bit`, eux, ne sont PAS comparables
entre un boot frais et 1 j 13 h d'uptime.

**Corrigé en v2.35.0** (bug-080 lot B-1) : entonnoir
`heap_caps_malloc(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)` aux trois sites
(nominal, repli, **réarmement**), `#pragma GCC poison malloc calloc realloc
strdup` dans `oom_guard.cpp`, et l'invariant est désormais **observable** :
ligne d'armement (`RAM interne` / `PSRAM (ANOMALIE)`) et champs
`oom_reserve_bytes` + `oom_reserve_internal` dans `/get-system-info`.

**Leçon transposable** : un `malloc()` nu change de sens quand la plateforme
gagne une nouvelle classe de mémoire. Chercher les autres allocations dont la
localisation est une hypothèse tacite.

Voir [[cause-racine-oom-metrique-heap-fausse]] (l'autre moitié du problème : la
métrique qui mesurait l'IRAM 32 bits) et
[[incident-oom-2-fichier-statique-13-08]].
