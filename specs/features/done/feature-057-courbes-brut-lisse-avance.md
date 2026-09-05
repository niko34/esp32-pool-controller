## Métadonnées
- **ID** : feature-057
- **Slug** : courbes-brut-lisse-avance
- **Type** : feature (réintroduction fidèle d'un outil retiré en feature-045)
- **Statut** : done
- **Priorité** : normale
- **Version cible** : 2.7.0 (réalignement du versioning sur la dernière release `v2.6.0` ; l'ancienne cible `2.19.2` de cette spec est abandonnée)
- **Dernière mise à jour** : 2026-07-11
- **Spec(s) liée(s)** : feature-045 (retrait outils debug, commit f18893c ; état complet au tag **v2.4.0**), feature-025 (filtre médian+EMA)

## Domaines touchés
- [ ] Régulation / dosage (aucun — observabilité seule)
- [x] Firmware — capteurs (`sensors.*`) + routes debug (`web_routes_debug.cpp`)
- [x] Web — routes HTTP (GET trace) + Frontend (`data/`)

## Agents requis
- [x] spec-reader
- [x] architect
- [ ] pool-chemistry — **non requis** : aucune modif de dosage / régulation / calibration ; enregistrement en lecture seule des valeurs déjà calculées. (code-reviewer vérifie le non-impact sur le cycle I²C / watchdog.)
- [x] embedded-developer
- [x] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description
Réafficher, dans les réglages **Avancé** (`panel-dev`), la carte de trace des signaux capteurs — **courbes pH et ORP, valeurs brutes vs lissées**, échantillonnées à **chaque cycle capteur** (haute cadence, bien supérieure aux 5 s du WebSocket) via un **ring buffer firmware**, avec graphe uPlot et stats min/max/Δ. Réintroduction **fidèle** de la carte « Debug oscillation pH » retirée en feature-045, **adaptée** à l'architecture actuelle (EZO numérique, pH **et** ORP) et **sans** les outils « pause WiFi » ni « diagnostic EZO » (eux restent retirés).

## Contexte
feature-045 (commit f18893c, v2.5.0) a retiré la carte « Debug oscillation pH » : ring buffer serveur de 300 échantillons (~6 KB RAM), routes `/debug/ph_trace` + `/debug/ph_trace_clear`, enregistrement à chaque cycle capteur, graphe uPlot + stats (pH brut/lissé min/max/Δ, ORP, T° envoyée). L'implémentation d'origine est **figée au tag v2.4.0** — s'en inspirer, mais l'adapter :
- getters actuels `getPhRaw()/getPhFiltered()/getOrpRaw()/getOrpFiltered()` (EZO, feature-025) ;
- routes dans `web_routes_debug.cpp` (pattern des routes préservées `/debug/sensor_filter_state`) ;
- hook d'enregistrement dans `SensorManager::update()` ;
- helpers uPlot existants (`createLineChart`, `createMiniLineChart`).
On veut la **fidélité** (haute cadence) et non l'option WS 5 s allégée (écartée par l'utilisateur).

## Comportement attendu
### Firmware
- **Ring buffer** (~300 échantillons) enregistrant à chaque cycle capteur : timestamp relatif (ms), `phRaw`, `phFiltered`, `orpRaw`, `orpFiltered`, et **T° eau** (proxy de la « T° envoyée » EZO). RAM bornée (~7–8 KB pour 300 × {uint32 + 5 float} ; ajuster la taille si besoin, la logger si réduite).
- **Route GET** `/debug/sensor_trace` (auth cohérente avec les autres routes /debug) → JSON du buffer (compact ; découpage/streaming si nécessaire pour rester < 50 ms et ne pas exploser la heap).
- **Route** `/debug/sensor_trace_clear` (POST) → vide le buffer.
- Enregistrement **non bloquant**, **aucun I²C** ajouté (on lit les valeurs déjà en cache), **watchdog** non impacté, ring buffer protégé si accès multi-tâches.

### UI (onglet Avancé, `panel-dev`)
- Carte « Signaux capteurs (brut / lissé) » avec :
  - **Graphe pH** : 2 séries brut vs lissé.
  - **Graphe ORP** : 2 séries brut vs lissé.
  - **Stats** min / max / **Δ** : pH brut, pH lissé, ORP brut, ORP lissé, T° (min/max).
  - Compteur « Échantillons : N », boutons **« Recharger la trace »** et **« Vider le buffer »**.
- Rafraîchissement à la demande (bouton) ; pas de polling permanent.

### Cas limites
- Valeurs `null`/NaN (filtre non amorcé, EZO injoignable) → trou dans la courbe, pas de crash.
- Buffer vide → message « aucune donnée », graphe masqué.
- Trace volumineuse → réponse HTTP bornée (pagination/troncature documentée si appliquée).

## Comportement actuel
Brut/lissé visibles seulement **instantanément** dans le modal « État filtre » (pages pH/ORP). Aucune trace temporelle haute cadence.

## Contraintes techniques
- ESP32 ; RAM sous surveillance (le ring buffer coûte ~7–8 KB — vérifier la marge, `pio run` rapporte l'occupation).
- Handler HTTP non bloquant (< 50 ms) ; sérialisation JSON maîtrisée (heap).
- Vanilla JS + uPlot (déjà embarqué), pas de dépendance nouvelle. Français.
- Aucune régression sur le cycle capteur / la régulation / le watchdog.

## Fichiers potentiellement concernés
- `src/sensors.h` / `src/sensors.cpp` — ring buffer + enregistrement dans `update()` + accesseurs.
- `src/web_routes_debug.cpp` — routes `/debug/sensor_trace` (GET) et `/debug/sensor_trace_clear` (POST).
- `src/constants.h` — taille du ring buffer (constante `kSensorTrace*`).
- `data/index.html` — carte dans `panel-dev`.
- `data/app.js` — récupération trace + graphes uPlot + stats.
- `data/app.css` — styles éventuels.
- `docs/features/page-settings.md`, `docs/subsystems/sensors.md`, `docs/API.md`, `CHANGELOG.md`.

## Livrables — check-list
- [x] A1 — Ring buffer firmware (pH/ORP brut+lissé + T°) + enregistrement dans `update()` + constante taille
- [x] A2 — Route GET `/debug/sensor_trace` (non bloquante) + POST `/debug/sensor_trace_clear`
- [x] B1 — Carte UI `panel-dev` : 2 graphes uPlot (brut/lissé) + stats min/max/Δ + compteur + boutons recharger/vider
- [x] B2 — Récupération/tracé côté client + gestion null/NaN
- [x] C1 — Docs : API.md (routes), page-settings.md (carte), sensors.md (ring buffer), CHANGELOG [2.7.0]
- [x] D1 — `pio run` SUCCESS (RAM 19,2 % / Flash 80,3 %), 214 tests natifs verts, revue code-reviewer Approved

## Éléments visibles (doc-writer)
### Endpoints HTTP
- `GET /debug/sensor_trace`, `POST /debug/sensor_trace_clear` (nouveaux).

### Documentation à produire / mettre à jour
- `docs/API.md` — nouvelles routes debug.
- `docs/features/page-settings.md` — carte de trace dans l'onglet Avancé.
- `docs/subsystems/sensors.md` — ring buffer de trace.
- `CHANGELOG.md` — entrée [2.19.2].

## Critères d'acceptation
- [x] L'onglet Avancé affiche 2 courbes (pH brut/lissé, ORP brut/lissé) reconstruites depuis la trace firmware, à cadence capteur.
- [x] Stats min/max/Δ correctes pour les séries ; compteur d'échantillons cohérent.
- [x] « Vider le buffer » réinitialise trace + graphes ; « Recharger » récupère l'état courant.
- [x] Valeurs nulles/NaN gérées sans crash.
- [x] Aucun impact mesurable sur le cycle capteur / la régulation ; watchdog OK ; handler < 50 ms.
- [x] `pio run` SUCCESS, RAM dans la marge (19,2 %) ; 214 tests natifs verts ; revue code-reviewer Approved.
- [x] Libellés en français.

## Hors périmètre
- Pause WiFi et diagnostic EZO (retirés en feature-045, **non** réintroduits).
- Persistance de la trace (buffer volatil, perdu au reboot — comme l'original).

## Notes techniques (clôture)
- **Firmware** : ring buffer `SensorManager::_traceBuffer[kSensorTraceBufferSize=300]` (constants.h), struct `SensorTraceSample {ms, phRaw, phFiltered, orpRaw, orpFiltered, tempEau}` ≈ 24 o → ~7,2 Ko RAM statique. Enregistrement via `_recordSensorTrace()` **en fin de `_readEzoSensors()`** (1×/cycle ~5 s), lock-free (1 producteur loopTask / 1 lecteur AsyncTCP), aucune transaction I²C ajoutée.
- **Routes** (`web_routes_debug.cpp`) : `GET /debug/sensor_trace` (JSON `{count, interval_ms, now, samples[]}`, null si NaN, arrondis pH×1000 / ORP×10 / T°×10, streamé non bloquant), `POST /debug/sensor_trace_clear`. Pas d'auth (cohérent `/debug/*`).
- **UI** (`data/index.html` + `app.js`) : card « Signaux capteurs (brut / lissé) » dans `#panel-dev`, 2 graphes uPlot (pH, ORP), stats min/max/Δ, compteur, boutons Recharger/Vider ; chargée uniquement à l'activation de l'onglet Avancé.
- **Versioning** : cible réalignée 2.19.2 → **2.7.0** (dernière release `v2.6.0` ; `version.h` déjà à 2.7.0).
- **ADR** : aucun requis — réintroduction fidèle suivant des patterns existants (routes `/debug/*`, contrat lock-free `_lastPh`, helpers uPlot), aucune alternative structurante ni contrainte verrouillante.
- **Validation** : build SUCCESS (RAM 19,2 % / Flash 80,3 %), 214 tests natifs verts, code-reviewer Approved.
- Fichiers touchés : `src/constants.h`, `src/sensors.h`, `src/sensors.cpp`, `src/web_routes_debug.cpp`, `data/index.html`, `data/app.js`, `src/version.h` ; docs : `docs/API.md`, `docs/features/page-settings.md`, `docs/subsystems/sensors.md`, `CHANGELOG.md`.
