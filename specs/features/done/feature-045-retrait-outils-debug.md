## Métadonnées
- **ID** : feature-045
- **Slug** : retrait-outils-debug
- **Type** : feature (retrait de fonctions — allègement)
- **Statut** : done
- **Priorité** : basse
- **Version cible** : 2.5.0 (retrait de fonctionnalités visibles = bump MINEUR)
- **Dernière mise à jour** : 2026-07-04
- **Spec(s) liée(s)** : feature-021/024 (origine des outils), feature-025 (le filtre et ses gardes NE SONT PAS touchés)

## Description
Retirer trois outils de debug ajoutés pour la campagne de diagnostic d'oscillation pH (2026-05/06), devenus inutiles en exploitation : **Debug oscillation pH** (carte UI + trace ring buffer + routes), **Pause WiFi** (même carte, même campagne) et **Diagnostic EZO** (carte UI + routes de commande manuelle). **Le code reste récupérable via git** : commit de suppression atomique, réintégration documentée par `git revert`.

## Périmètre EXACT du retrait
### Backend (src/)
- `sensors.h/.cpp` : struct `PhDebugSample`, `kPhDebugBufferSize` (300), `_phDebugBuffer` (~6 KB RAM), `_phDebugIdx`, `_recordPhDebugSample` + son appel (sensors.cpp l.442), `getPhDebugSampleCount`, `getPhDebugSamplesJson`, `clearPhDebugSamples` (et tout membre associé).
- `web_routes_debug.cpp` : routes `/debug/ph_trace` (GET), `/debug/ph_trace_clear` (POST), `/debug/wifi_pause` (GET **et** POST, + tout état/mécanisme de pause associé — tracer l'implémentation), `/debug/ezo_command` (POST), `/debug/ezo_factory` (POST).

### Frontend (data/)
- `index.html` : carte **« Debug oscillation »** complète (~l.1803-1830, inclut compteur, boutons refresh/vider, **bouton et statut pause WiFi**, chart wrap, stats) + carte **« Diagnostic EZO »** complète (~l.1837-1880).
- `app.js` : rendu debug chart uPlot (`__debugPhRender`, `__debugPhLoad`, destroy/vider), handlers pause WiFi, handlers Diagnostic EZO (~l.5543+). ⚠️ **NE PAS toucher aux helpers uPlot partagés** (tooltipPlugin, ResizeObserver, makeChartState, usines) utilisés par les 6 autres graphiques.
- `app.css` : styles spécifiques au debug chart uniquement (s'il y en a — vérifier qu'ils ne servent pas aux autres charts).

## À GARDER ABSOLUMENT (hors périmètre)
- `/debug/ph_slope_refresh` — utilisé par l'UI de calibration (app.js l.2678).
- `/debug/sensor_filter_reset` et `/debug/sensor_filter_state` — **levier de sécurité** (levée du latch « capteur instable », feature-025).
- Routes coredump (fichier séparé), tout le filtre `sensor_filter` et ses gardes.

## Réintégration (à documenter)
- Un commit de suppression **atomique** ; message indiquant « réintégrable via git revert <ce commit> ».
- Le tag v2.4.0 fige l'état complet avec les outils.

## Critères d'acceptation
- [x] AC1 — Les 5 routes retirées répondent 404 ; `ph_slope_refresh` et `sensor_filter_*` répondent toujours.
- [x] AC2 — Plus aucune référence morte : grep `ph_trace|wifi_pause|ezo_command|ezo_factory|PhDebugSample|debug_ph_|debug_ezo_|debug_wifi_` → 0 occurrence active dans src/ et data/ (hors docs historiques/ADR).
- [x] AC3 — Les 6 graphiques restants (3 mini + 3 historiques) fonctionnent (helpers uPlot partagés intacts).
- [x] AC4 — `pio run` SUCCESS sans warning ; gain flash ET RAM statique mesurés vs avant.
- [x] AC5 — `./build_fs.sh` OK ; gain payload FS mesuré ; `node --check` OK.
- [x] AC6 — `pio test -e native` : 118 PASSED (rien du filtre n'est touché).
- [x] AC7 — Docs : API.md (routes retirées), page-settings.md (cartes retirées), sensors.md (section trace debug retirée — aucune section dédiée n'existait, seule une mention historique `/debug/ezo_command` annotée « retiré en v2.5.0 »), CHANGELOG 2.5.0 avec note de réintégration git. Bonus : `docs/BUILD.md` (décompte graphiques uPlot 7 → 6).
- [x] AC8 — Validation navigateur utilisateur : panneau Avancé sans les 2 cartes, le reste inchangé.

## Hors périmètre
- Tout autre contenu du panneau Avancé (coredump, logs) ; le filtre capteur ; la calibration.

---

## Clôture (2026-07-04)
- Livrée en **v2.5.0** (commit atomique `f18893c`, release GitHub avec binaires OTA `firmware.bin`/`littlefs.bin`).
- **AC8 validé** : mise à jour OTA en ligne 2.4.0 → 2.5.0 effectuée par l'utilisateur via l'app (premier test du flux GitHub intégré, assets aux noms exacts attendus par web_routes_ota) ; panneau Avancé vérifié sans les cartes debug lors de la validation de feature-046.
- Réintégration : `git revert f18893c` (les 3 outils) ; état complet figé au tag v2.4.0.
- Gains confirmés : Flash −12,3 KB (83,1 %), RAM −6 KB (16,7 %), FS −13,8 KB.
