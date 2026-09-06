---
name: incident-oom-2-fichier-statique-13-08
description: "2e crash abort en async_tcp (13/08, v2.18.7) — échec d'un new de 156 o AVEC 81 Ko libres ; défaut identifié - le parachute OOM ne peut pas se réarmer (seuil 32 Ko > maxBloc réel ~28-30 Ko)"
metadata: 
  node_type: memory
  type: project
  originSessionId: 82048c5b-7e8f-4cec-8242-ad9b5ed01f91
  modified: 2026-08-14T08:25:10.466Z
---

**13/08/2026 ~21:37, v2.18.7 core 3, WROOM.** Uptime 46 438 s (12,9 h). Abort
dans `async_tcp` — mais **rien à voir avec la famine du 11/08**.

Pile : `AsyncStaticWebHandler::handleRequest` (WebHandlers.cpp:263,
`new AsyncFileResponse(...)` = 156 o) → `operator new` échoue → `throw bad_alloc`
→ `__cxa_allocate_exception(100)` échoue AUSSI → `std::terminate` → abort.

**Anneau RTC (2 s avant) : heap SAIN et stable depuis 78 s — free=81408,
maxB=27584, ws=0, skip=0.** Piles toutes saines (async_tcp 2176/14196).
Un malloc de 156 o ne peut pas échouer dans cet état → deux hypothèses :
(a) consommateur très rapide dans l'angle mort de 2 s de l'échantillonnage
(comme le 11/08 : 60 Ko en 5 s), ou (b) **corruption du tas** (la comptabilité
annonce 81 Ko libres mais l'allocateur ne sert plus rien).

**DÉFAUT CONFIRMÉ (v2.18.5, à corriger) — le parachute est one-shot en pratique** :
pour que libstdc++ lève à new_op.cc:54 il faut `get_new_handler() == nullptr`,
donc notre handler s'était désenregistré (état Depleted). Or le réarmement exige
`maxBloc >= kOomRearmMinMaxAllocBytes = 32768`, alors que le **maxBloc réel en
régime établi est de 27-30 Ko** (mesuré : 29 684 o après 33 min d'uptime,
27 584 o après 12,9 h). Une fois consommée, la réserve n'est donc JAMAIS
ré-allouée. À revoir : seuil ~16 Ko, ou relatif à la taille de réserve.

**Autre limite constatée** : le CRITICAL « Réserve OOM consommée » est émis en
DIFFÉRÉ par la loop → perdu si l'abort suit immédiatement. Le journal persistant
d'avant crash a lui aussi été perdu (flush toutes les 10 min). Seul l'anneau RTC
a survécu.

**Instrumentation décisive à ajouter** : écrire en RTC NOINIT, DEPUIS le callback
`heap_caps_register_failed_alloc_callback` (déjà en place, sans alloc ni mutex),
un post-mortem : taille demandée + free heap + plus gros bloc **au moment de
l'échec** (pas 2 s avant), dumpé au boot suivant. Cela départage (a) et (b) en
une occurrence. Compléments utiles : `heap_caps_check_integrity_all()` périodique
(détecte (b)), flag RTC « réserve consommée » pour survivre à l'abort.

**LIVRÉ en v2.18.8 (ADR-0037, cycle complet du 14/08)** : seuil de réarmement
32768 → **24576** + **palier de repli 2048 o** armé dès 12288 (surclassement
possible, déclassement interdit) → le parachute est enfin réarmable dans le
régime réel. Post-mortem RTC 32 o (magic 0xF00DFA11) écrit par les chemins
d'échec en stores bruts, dumpé au boot avec verdict FAMINE / SUSPICION
CORRUPTION / INDÉTERMINÉ. Contrôle d'intégrité du tas toutes les 5 min (1-3 ms,
`CONFIG_HEAP_POISONING_LIGHT`), avec 8 gardes de sécurité chimique dont le
nouvel accesseur `isAnyPumpEnergized()` — les indicateurs de dosage ne
couvraient QUE la régulation auto, une injection manuelle passait au travers
(prise du gate pool-chemistry). 507 tests natifs, revue Approved après 4
correctifs bloquants (dont : la validité rejetait la zone après le repliement
16 bits → dump jamais émis ; le callback était armé avant la lecture du relevé
précédent ; le verdict sans fenêtre de rafale aurait dit « famine » sur le cas
du 13/08). Binaire : `builds/essais-core3/firmware-2.18.8-core3.bin`.
⚠️ **La cause racine reste inconnue** — ce cycle instrumente, il ne répare pas.

Voir [[decodage-coredump-et-incident-oom-async-tcp]] (1er incident + procédure de
décodage, inchangée : gdb de `toolchain-xtensa-esp32`).
