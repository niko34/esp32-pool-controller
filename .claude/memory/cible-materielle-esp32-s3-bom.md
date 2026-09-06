---
name: cible-materielle-esp32-s3-bom
description: "Cible matérielle officielle du PCB v2 = ESP32-DevKitC-VE (WROVER-E, PSRAM) car la mise à jour en ligne GitHub exige la PSRAM ; le S3 a été écarté"
metadata: 
  node_type: memory
  type: project
  originSessionId: 82048c5b-7e8f-4cec-8242-ad9b5ed01f91
  modified: 2026-08-12T20:42:28.165Z
---

**2026-08-12, décision utilisateur** : le BOM (`hardware/BOM.csv` ET
`kicad v2/BOM.csv`, repère **U2**) spécifie un **ESP32-DevKitC-VE**
(module WROVER-E, ESP32 classique + 8 Mo PSRAM), commentaire « Avec de la PSRAM ».
Un passage intermédiaire par `ESP32-S3-WROOM-1` a été **abandonné le même jour**
(architecture Xtensa LX7 non supportée, brochage incompatible PCB v2 : GPIO 22 =
I²C SCL et GPIO 25 = pompe pH n'existent pas sur S3). La spec d'étude de portage
S3 (feature-067) a été supprimée en conséquence.

**Motif de l'exigence PSRAM** : la mise à jour automatique GitHub ouvre une
session TLS réclamant plusieurs blocs contigus ~16 Ko. Sans PSRAM (WROOM-32) la
RAM interne ne les fournit plus de façon fiable → échec intermittent (`BIGNUM -
Memory allocation failed`) et, depuis v2.18.5, refus 503 par la garde mémoire.
Avec PSRAM, ces allocations y vont automatiquement (seuil 4 Ko). Le firmware
reste un **binaire unique** qui tourne sur WROOM sans PSRAM (tout fonctionne sauf
la fiabilité de l'update en ligne — l'upload manuel et deploy.sh ota-* n'ouvrent
aucun TLS sortant).

**Docs porteuses de cette exigence** (à maintenir cohérentes) : README section
« Module ESP32 — ⚠️ PSRAM requise », `docs/BUILD.md` (note cible matérielle),
`docs/UPDATE_GUIDE.md` (colonne « PSRAM requise » + encadré méthode GitHub).

⚠️ **Coquille repérée dans le BOM le 12/08** : la ligne U2 porte encore le
**lien Waveshare vers la carte ESP32-S3** alors que la référence est
ESP32-DevKitC-VE — signalé à l'utilisateur, à corriger côté BOM (fichier
matériel, non modifié par Claude).

Voir aussi [[plan-migration-core3]] (WROVER-E commandés le 08/08) et
[[decodage-coredump-et-incident-oom-async-tcp]] (garde TLS v2.18.5).
