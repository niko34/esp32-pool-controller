## Métadonnées
- **ID** : feature-065
- **Slug** : retrait-ecran-uart
- **Type** : feature (retrait de fonctionnalité)
- **Statut** : done
- **Priorité** : normale (prérequis du levier WROVER-E/PSRAM — cf. bug-checkupdate-tls-core3)
- **Version cible** : 2.17.0
- **Dernière mise à jour** : 2026-08-08
- **Spec(s) liée(s)** : bug-checkupdate-tls-core3 (motivation GPIO 16/17), ADR-0034

## Domaines touchés
- [x] Capteurs (émissions d'événements UART dans sensors.cpp — retrait uniquement)
- [x] Régulation (pump_controller — émissions d'événements UART uniquement) → **passage obligatoire par `pool-chemistry`**
- [x] Filtration / Éclairage (émissions d'événements UART uniquement)
- [ ] Web — routes HTTP / WebSocket
- [ ] MQTT / Home Assistant
- [x] UART / écran LVGL (SUPPRESSION complète)
- [ ] OTA / Authentification / Historique / Frontend

## Agents requis
- [x] spec-reader
- [x] architect
- [x] pool-chemistry — **obligatoire** (pump_controller.cpp touché, même si retrait pur)
- [ ] ux-ui-designer
- [x] embedded-developer
- [ ] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description
Supprimer intégralement le support de l'écran LVGL externe (liaison UART2,
GPIO 16/17) : couches `uart_transport`/`uart_protocol`/`uart_commands` (~990
lignes) et toutes les émissions d'événements dispersées dans le firmware.
L'écran était un essai, jamais mis en production (décision utilisateur
2026-08-08).

## Contexte
Double motivation : (1) libérer les **GPIO 16/17**, physiquement requis par la
PSRAM du module WROVER-E envisagé (levier mémoire du bug-checkupdate-tls-core3) ;
(2) retirer ~1 000 lignes de code mort de production (flash à 92,3 % d'app0
sous core 3, maintenance). Le projet écran séparé (esp32-pool-controller-screen)
reste intact ; l'historique git du contrôleur conserve le code si besoin.

## Comportement attendu
- Le firmware ne référence plus rien de l'UART2/écran : fichiers `src/uart_*`
  supprimés, appels retirés de `main.cpp` (begin/update), émissions d'événements
  retirées de `pump_controller.cpp`, `filtration.cpp`, `sensors.cpp`,
  `web_routes_calibration.cpp`, mention nettoyée de `constants.h`.
- **AUCUN changement de comportement** hors liaison écran : régulation, gardes
  de sécurité chimique, web, MQTT, historique strictement identiques.
- Les commandes UART entrantes de l'écran (wifi_scan, wifi_connect,
  change_password, set_config NTP...) disparaissent avec `uart_commands`.
- GPIO 16/17 : plus aucun pinMode/périphérique — libres.

### Cas limites
- Écran physiquement encore câblé : aucun effet (plus d'écoute UART2), aucun
  risque électrique nouveau (broches en haute impédance par défaut).

## Comportement actuel
UART2 115200 bps sur GPIO16/17, 3 couches (`uart_transport` →
`uart_protocol` → `uart_commands`), begin() dans setup(), update() dans loop(),
événements asynchrones émis depuis filtration/pompes/calibration/capteurs.
Doc : `docs/uart_protocol.md`, script `tools/uart_test.py`.

## Contraintes techniques
- Retrait PUR : aucune ligne de logique métier modifiée autour des appels
  supprimés (diff minimal, revue facilitée)
- Bi-core : compiler sur `esp32dev` ET `esp32dev_core3`
- Sécurité chimique : les émissions retirées dans pump_controller ne doivent
  emporter AUCUNE garde ni compteur adjacents

## Fichiers potentiellement concernés
- `src/uart_transport.*`, `src/uart_protocol.*`, `src/uart_commands.*` — [supprimés]
- `src/main.cpp`, `src/pump_controller.cpp`, `src/filtration.cpp`,
  `src/sensors.cpp`, `src/web_routes_calibration.cpp`, `src/constants.h` — retrait des appels/références
- `tools/uart_test.py`, `docs/uart_protocol.md` — [supprimés]
- `docs/subsystems/*.md` (mentions), `CHANGELOG.md`, `README.md` si l'écran y figure

## Livrables — check-list
- [x] A1 — Suppression des 6 fichiers `src/uart_*` + `tools/uart_test.py`
- [x] A2 — Retrait des intégrations : main.cpp (begin/update), pump_controller,
      filtration, sensors, web_routes_calibration, constants.h (note GPIO)
- [x] A3 — `src/version.h` → 2.17.0
- [x] D1 — `pio run` core 2 sans warning ; gain flash mesuré et consigné (−35,3 Ko)
- [x] D2 — `pio run -e esp32dev_core3` SUCCESS ; gain flash mesuré (92,3 % → 90,9 %)
- [x] D3 — `pio test -e native` : 428 tests, zéro régression (428/428 verts)
- [x] C1 — mentions purgées des docs vivantes (`docs/uart_protocol.md` n'existait déjà plus — voir Notes)
- [x] C2 — `CHANGELOG.md` 2.17.0 (retrait visible utilisateur)
- [x] C3 — Note ADR-0034 : GPIO 16/17 libérés (prérequis WROVER coché)

## Sécurité chimique
- Dosage impliqué : non — retrait d'émissions d'ÉVÉNEMENTS uniquement dans
  pump_controller ; aucune garde, aucun compteur, aucun chemin d'injection modifié
- Risque identifié : suppression accidentelle d'une ligne adjacente à un appel
  UART retiré → validation pool-chemistry du diff obligatoire (P2 sur diff réel)

## ADR attendu ?
- **Probable** : non — décision consignée dans la spec + note ADR-0034 (le
  retrait sert le levier PSRAM déjà documenté). `architect` confirmera.

## Éléments visibles (pour doc-writer)
- Endpoints HTTP / WS / MQTT : aucun changement
- Docs : suppression `docs/uart_protocol.md`, purge des mentions écran/UART
  dans README + subsystems, CHANGELOG 2.17.0, note ADR-0034

## Critères d'acceptation
- [x] `grep -ri "uart_transport\|uart_commands\|uart_protocol\|Uart2" src/` ne retourne rien (revérifié par doc-writer le 2026-08-08)
- [x] Aucun diff fonctionnel hors retrait (revue ligne à ligne des 6 fichiers intégrés — revue Approved, double GO pool-chemistry)
- [x] Builds core 2 + core 3 verts, 428 tests natifs verts
- [x] Gains flash consignés dans la spec (les deux cores — voir Notes d'implémentation)

## Hors périmètre
- Le projet esp32-pool-controller-screen (repo séparé, intact)
- Le changement de module WROVER-E lui-même (spec future dédiée)
- Toute réaffectation des GPIO 16/17 (se fera avec le WROVER)

## Notes d'implémentation (2026-08-08, v2.17.0)

- **Verdicts** : double GO `pool-chemistry` (P2 sur diff réel : aucune garde ni compteur emporté), revue `code-reviewer` **Approved**, builds core 2 + core 3 verts, **428/428** tests natifs.
- **Gains flash mesurés** :
  - core 2 (production) : **−35,3 Ko** → 1 481 445 o, **80,7 %** d'app0 ;
  - core 3 (banc) : **−30,0 Ko** → 1 667 566 o, marge app0 **92,3 % → 90,9 %**.
- **GPIO 16/17 libérés** : plus aucun pinMode/périphérique — prérequis matériel WROVER-E (+8 Mo PSRAM) levé, note additive posée sur ADR-0034.
- **Constat** : `docs/uart_protocol.md` n'existait déjà plus au démarrage du chantier (supprimé antérieurement) — livrable C1 réduit à la purge des mentions dans les docs vivantes.
- **Décision `screen_enabled`** : le champ config et la card « Écran LVGL » de Paramètres → Avancé (déjà masquée, `display:none`) sont **conservés mais inertes** — valeur persistée sans effet. Retrait éventuel : spec de suivi.
- **Sécurité chimique — bypass fermé** : la voie non gardée `run_action/pump_test` (UART → `setManualPump()` sans `evaluateManualInject()`, condition pool-chemistry #4 de feature-006) **disparaît** avec `uart_commands` ; l'avertissement de `docs/subsystems/pump-controller.md` est historicisé (« FERMÉ en v2.17.0 »).
- **Post-revue** : commentaires résiduels mentionnant l'UART purgés de `pump_controller.cpp`.
- **ADR** : aucun nouvel ADR (verdict architect — aucun des 4 critères : retrait de code mort, décision portée par la spec + note ADR-0034).
