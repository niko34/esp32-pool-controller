---
name: cause-racine-oom-metrique-heap-fausse
description: "CAUSE RACINE des 3 crashs OOM — ESP.getFreeHeap/getMaxAllocHeap mesurent MALLOC_CAP_INTERNAL (IRAM 32 bits comprise), pas le pool 8 bits réellement utilisé ; toutes les gardes heap du projet sont donc aveugles"
metadata: 
  node_type: memory
  type: project
  originSessionId: 82048c5b-7e8f-4cec-8242-ad9b5ed01f91
  modified: 2026-08-15T08:49:11.861Z
---

**15/08/2026, 3ᵉ crash (v2.18.9) — le post-mortem livré en 2.18.8 a payé.**

Chaîne complète décodée : `esp_littlefs` échoue (« Unable to allocate FD ») →
`ESP_LOGE` → `vprintf` → écriture console → création PARESSEUSE du verrou UART
(`lock_init_generic`, newlib locks.c:77) → `xSemaphoreCreateRecursiveMutex`
échoue faute de mémoire → **abort() direct**. Le parachute OOM ne peut RIEN ici :
il ne couvre que les `new` C++, pas les malloc de newlib/FreeRTOS.

Post-mortem : `échecs=767 sur 36023 s (1er 1622 o, dernier 92 o caps=0x0804),
tas comptabilisé t-2s free=67584 maxB=27584 → verdict SUSPICION CORRUPTION`.
Les 92 o / caps 0x0804 (= INTERNAL|8BIT) sont très probablement le mutex fatal.

## LE VERDICT « CORRUPTION » EST FAUX — artefact de notre propre métrique

`ESP.getFreeHeap()` = `heap_caps_get_free_size(MALLOC_CAP_INTERNAL)` et
`ESP.getMaxAllocHeap()` = `heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)`
(vérifié dans `framework-arduinoespressif32/cores/esp32/Esp.cpp:163-173`).
**MALLOC_CAP_INTERNAL inclut l'IRAM accessible en 32 bits SEULEMENT**, inutilisable
pour un malloc/new ordinaire (qui exige MALLOC_CAP_8BIT). D'où :
- un maxBloc figé à 27584/27636 o pendant des JOURS et à travers les reboots =
  signature d'un reliquat d'IRAM constant, pas d'un vrai bloc DRAM ;
- des allocations de 92 à 2872 o qui échouent « alors qu'il reste 27 Ko » : il n'y
  a pas de contradiction, la famine 8 bits est RÉELLE.

**Conséquence grave : toutes les gardes heap du projet mesurent le mauvais pool** —
`kRestartMaxAllocHeapBytes` (auto-guérison, 8192 : ne se déclenche probablement
JAMAIS puisque maxB reste à 27 Ko), `kWsPushMinAllocBytes` (16384),
`kCheckUpdateMinMaxAllocBytes`/`kCheckUpdateMinFreeHeapBytes` (garde TLS
ADR-0036), l'anneau heap, le post-mortem et son verdict (ADR-0037).

**Correctif à faire** : passer partout à
`heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)` et
`heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)`,
puis RE-CALIBRER tous les seuils sur les valeurs réelles (ils ont été fixés sur
des mesures faussées), et re-tester le verdict du post-mortem.

Reste à élucider APRÈS cette correction : ce qui consomme la mémoire 8 bits
(oscillations de ~45 Ko dans l'anneau, 767 échecs en 10 h). Piste : chemin de
service des fichiers statiques (`AsyncStaticWebHandler`, déjà présent dans le
crash du 13/08) et `LittleFS.begin(false)` = **maxOpenFiles 10 par défaut**.

Voir [[incident-oom-2-fichier-statique-13-08]] et
[[decodage-coredump-et-incident-oom-async-tcp]].
