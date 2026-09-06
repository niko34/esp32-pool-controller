---
name: decodage-coredump-et-incident-oom-async-tcp
description: Commande qui marche pour décoder un coredump ESP32 (esp-coredump + gdb toolchain-xtensa-esp32) + incident OOM async_tcp du 11/08/2026 en prod core 3
metadata: 
  node_type: memory
  type: project
  originSessionId: 82048c5b-7e8f-4cec-8242-ad9b5ed01f91
  modified: 2026-08-13T13:18:54.136Z
---

## Décoder un coredump du contrôleur (procédure validée 11/08/2026)

```bash
/Users/nicolas/.platformio/penv/bin/python -m esp_coredump info_corefile \
  --core coredump.bin --core-format raw \
  --gdb /Users/nicolas/.platformio/packages/toolchain-xtensa-esp32/bin/xtensa-esp32-elf-gdb \
  .pio/build/esp32dev_core3/firmware.elf
```

- Le gdb de `toolchain-xtensa-esp-elf/bin/` échoue (pygdbmi « could not be resolved ») — utiliser celui de **`toolchain-xtensa-esp32/bin/`**.
- `esp-coredump` est déjà installé dans le penv PlatformIO.
- L'ELF doit être le build EXACT du firmware qui tournait (vérifier via `strings .pio/build/<env>/firmware.elf | grep 2.18`).
- La partition `coredump` (64 Ko @ 0x3F0000, layout v4) stocke le dump au panic.

## Incident OOM du 11/08/2026 ~22:05 (prod, core 3, v2.18.4)

`std::bad_alloc` non rattrapé → abort → reboot. Tâche `async_tcp` :
`operator new(356)` = `new AsyncWebServerRequest` dans ESPAsyncWebServer
WebServer.cpp:50 au moment où un client HTTP se connectait.

**Cause racine (prouvée par le ring heap RTC noinit, dumpé au boot suivant —
« Cinétique heap avant reset (PANIC) ») :** `HistoryManager::saveToFile()`
(toutes les 5 min, dans la loop) construit TOUT l'historique (~354 points,
fichier 32 Ko) dans un `JsonDocument` en RAM → pic ~60 Ko tenu 10-15 s pendant
l'écriture LittleFS (loop bloquée : jitter d'échantillonnage 2 s→5 s visible
dans le ring). free passe de 94 Ko à 32 Ko. Si un navigateur (re)charge l'UI
dans cette fenêtre (rafale de connexions TCP parallèles, ~30 Ko), le heap
tombe à zéro et l'accept suivant crashe. ws=0 pendant tout l'épisode = pas
d'action UI en cause. Race window ~5 % par chargement de page.

**Investigation du 11-12/08 (mesures live)** : hypothèses RÉFUTÉES une à une —
saveToFile historique (~23 Ko bref, 108 points réels, pas 350), clients TCP gelés
(~200 o), rafale page lente (< 1 Ko), check-update cliqué (démenti utilisateur).
**Cause du hold de 60 Ko NON attribuée** ; candidats restants : buffers RX
dynamiques du driver WiFi pendant tempête réseau (socket MQTT gelé au crash,
3 décrochages MQTT le même soir), ou appel /check-update authentifié fantôme
(IP non loggée à l'époque). Fait utile : le TLS GitHub sur core 3 est
INTERMITTENT, pas impossible — dépend de maxB au départ (échec BIGNUM en 0,14 s
à maxB=38,9 Ko ; GitHub ATTEINT en 2,7 s sur heap reposé).

**Durcissement livré en v2.18.5 (ADR-0035, cycle complet 12/08)** :
parachute OOM global (set_new_handler + réserve 12 Ko libérable, réarmement
hystérésis 32 Ko/10 s, critère pointeur anti-course) + callback IDF flag-seul
(diagnostic malloc C) + garde bicritère /check-update (free ≥ 64 Ko ET
maxB ≥ 20 Ko → sinon 503 rapide) + IP client loggée (attribution des récidives).
459 tests natifs, builds bi-core verts.

**SUITE ET FIN (v2.18.6/2.18.7, validées on-target le 13/08)** : la garde 2.18.5
s'est révélée trop permissive dès le premier essai réel (free=87708/maxB=34804
passaient, puis famine avec une allocation de 100 o refusée = la fenêtre du
crash). Garde alignée sur le contrat matériel (ADR-0036) : **1 Mo de PSRAM LIBRE
exigé** (`getFreePsram`, PAS `getPsramSize` — une PSRAM saturée ferait retomber
mbedTLS sur la RAM interne), fail-closed, appliquée AUSSI à `/download-update`
qui n'avait AUCUNE garde (contournement par POST direct, avec flash à la clé).
Trois codes machine : `psram_required` / `psram_low` / `tls_low_memory`.
Mesuré sur l'appareil en 2.18.7 : **503 en 35 ms, plus aucune ligne
« Allocation heap échouée »** derrière — fenêtre de famine fermée.
Release GitHub **v2.18.7** publiée (assets `firmware.bin` core 3 + `littlefs.bin`,
digests vérifiés). Reste à mesurer : branche PSRAM au montage du DevKitC-VE.
Contexte heap core 3 : [[plan-migration-core3]].
