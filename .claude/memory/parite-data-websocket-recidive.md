---
name: parite-data-websocket-recidive
description: "Motif de bug récurrent — un champ publié sur le WebSocket mais absent de GET /data (ou l'inverse) ; trois occurrences en trois versions"
metadata: 
  node_type: memory
  type: project
  originSessionId: 82048c5b-7e8f-4cec-8242-ad9b5ed01f91
  modified: 2026-08-27T06:59:48.400Z
---

**Tout champ ajouté à la trame WebSocket doit être ajouté à `GET /data` dans le
même geste, avec des règles null/arrondi identiques — et réciproquement.**

Trois occurrences en trois versions, toutes trouvées après livraison :

1. **v2.21.2** — `phSlopeAcid/Base/Zero/AgeMs` étaient dans la trame WS depuis
   feature-024 mais absents de `/data`. Un client sans WebSocket était aveugle
   sur la santé de sonde, et ce trou a compliqué le diagnostic lui-même.
2. **v2.23.0** — les 6 champs de calibration de dosage manquaient dans la trame
   de config WS ; au premier push, l'UI écrasait l'affichage avec du vide.
   Trouvé on-target par l'utilisateur (champs de mesure vides).
3. **v2.25.0** (27/08/2026) — `phDoseBlockedReason`/`orpDoseBlockedReason`
   publiés en WS seulement. Découvert en voulant construire un avertissement UI.

**Why:** côté client, `latestSensorData` est **intégralement remplacé** par les
charges `/data` sur plusieurs chemins (`route-view`, `cal-await` toutes les
1,5 s, `filtration-save`). Un champ présent seulement en WS ne disparaît donc
pas franchement — il **clignote**. Le défaut se manifeste par intermittence,
précisément quand le réseau flanche, ce qui est le pire mode d'échec possible
pour un signal de sécurité. Et la leçon symétrique de la v2.23.0 : côté client,
**clé absente = pas d'information, jamais zéro**.

**How to apply:** à chaque ajout de champ de télémétrie, grep le nom du champ
dans `src/ws_manager.cpp` ET `src/web_routes_data.cpp` avant de conclure — la
parité se prouve par grep des deux côtés, pas par relecture. Vérifier au passage
`kWsFrameBufBytes` et le buffer JSON de `/data` : une troncature silencieuse de
trame est pire que le champ manquant. Voir aussi
[[cause-racine-oom-metrique-heap-fausse]] pour l'autre grande famille de
« la mesure ne dit pas ce qu'on croit » dans ce projet.
