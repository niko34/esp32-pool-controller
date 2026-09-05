## Métadonnées
- **ID** : feature-013
- **Slug** : ui-mode-expert-parametres-dur
- **Type** : feature
- **Statut** : **clôturée — écartée** (2026-07-05, non implémentée, décision utilisateur sur analyse)
- **Priorité** : basse (à la rédaction — caduque à la clôture)
- **Version cible** : n/a
- **Dernière mise à jour** : 2026-07-05

## Note de clôture (2026-07-05)

Spec rédigée en avril 2026, **écartée** après audit contre le code v2.9.2. État factuel : les 11 paramètres visés vivent dans `PumpProtection` (`config.h` l.132-140) et `PumpControlParams` avec défauts en dur, ni persistés ni exposés — la prémisse technique tenait. C'est le rapport coût/bénéfice/risque qui condamne la spec :

1. **Coût maximal** : la spec la plus chère du backlog (11 champs × config/NVS/API/validation/fallback corruption/UI + ADR + ux-ui-designer + double passage pool-chemistry), pour une priorité « basse ».
2. **Risque contraire à la philosophie du projet** : ces paramètres (anti-cycling, deadbands, bornes PID) sont les **garde-fous pompes/chimie**, verrouillés depuis les features 025-041 derrière une logique pure testée à 100 % en natif. Les exposer dans l'UI — même bornés, même avec avertissement — recrée la surface de risque (short-cycling, oscillation, surdosage) que ce travail a éliminée. Un garde-fou n'est pas une préférence utilisateur.
3. **Le canal de réglage adapté existe et a fait ses preuves** : micro-spec ciblée avec validation pool-chemistry quand un seuil doit réellement changer (précédent : `orpStartThreshold` 10→15 mV, feature-025). Pour un système mono-utilisateur itérant via Claude Code + OTA, l'argument « le rebuild freine l'itération » ne correspond pas à l'usage réel. Tout ce qui mérite d'être réglable sans rebuild l'est déjà (cibles, limites journalières/horaires, vitesse de régulation `regulation_speed`, débits/duty pompes, délai de stabilisation).

**Porte de sortie** : si un réglage précis devient nécessaire un jour, ouvrir une micro-spec dédiée au paramètre concerné (avec plages validées pool-chemistry) plutôt que d'exposer le lot.

Aucun code modifié au titre de cette spec. Le contenu ci-dessous est conservé à titre de traçabilité.
- **Doc(s) liée(s)** : [docs/subsystems/pump-controller.md](../../docs/subsystems/pump-controller.md#paramètres-en-dur-récapitulatif), [docs/features/page-settings.md](../../docs/features/page-settings.md)

## Domaines touchés
- [ ] Capteurs (pH / ORP / température)
- [x] Régulation (pump_controller, PID) → **passage obligatoire par `pool-chemistry`**
- [ ] Filtration / Éclairage
- [x] Web — routes HTTP / WebSocket
- [ ] MQTT / Home Assistant
- [ ] UART / écran LVGL
- [ ] OTA
- [ ] Authentification
- [x] Historique / persistance (NVS)
- [x] Frontend (`data/`)

## Agents requis
- [x] spec-reader
- [x] architect
- [x] pool-chemistry — **obligatoire** (ces paramètres protègent matériel + chimie)
- [x] ux-ui-designer — nouvelle frontière UI sensible (avertissement, activation explicite, bandeau rouge, bouton restore)
- [x] embedded-developer
- [x] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description
Ajouter un **mode expert** dans la page Paramètres (onglet Avancé ou nouvel onglet Expert) exposant les paramètres aujourd'hui en dur : seuils anti-cycling, `maxCyclesPerDay`, `minInjectionTimeMs`, deadband pH/ORP, `integralMax`. L'accès nécessite un bandeau d'avertissement + activation explicite. Valeurs par défaut inchangées.

## Contexte
Voir le tableau récap dans [docs/subsystems/pump-controller.md](../../docs/subsystems/pump-controller.md#paramètres-en-dur-récapitulatif) pour la liste complète des paramètres aujourd'hui en dur.

L'écart actuel : toute modification de ces valeurs exige un nouveau build firmware, ce qui freine l'itération. Mais exposer sans garde-fou risquerait de dégrader la protection pompes / chimie. La réponse produite : **mode expert désactivé par défaut**, activation explicite, valeurs bornées, reset facile aux défauts.

## Comportement attendu

### Côté firmware
- Les paramètres suivants deviennent configurables via `/save-config`, persistés en NVS, défaut = valeur actuelle :
  - `minInjectionTimeMs` (défaut 30 000, plage 10 000–120 000)
  - `maxCyclesPerDay` (défaut 20, plage 5–50)
  - `phStartThreshold` (défaut 0.05, plage 0.01–0.2)
  - `phStopThreshold` (défaut 0.01, plage 0.005–0.1)
  - `orpStartThreshold` (défaut 10, plage 5–50 mV)
  - `orpStopThreshold` (défaut 2, plage 1–20 mV)
  - `phDeadband` (défaut 0.01, plage 0.005–0.05)
  - `orpDeadband` (défaut 2, plage 1–10 mV)
  - `integralMax` (défaut 50, plage 10–200)
  - `kPhMaxError` (défaut 1.0, plage 0.5–2.0)
  - `kOrpMaxError` (défaut 200, plage 100–500 mV)
- Validation : refuser toute valeur hors plage → 400 + message précis
- Bouton « Restaurer les défauts » côté UI → `POST /save-config` avec tous les champs expert remis à la valeur constante d'origine
- `config.h` : garder les constantes comme valeurs par défaut ; ajouter les champs correspondants dans `PumpControlParams` (ou nouvelle struct `ExpertParams`)
- Le code existant (`pump_controller.cpp`, `sensors.cpp`) lit désormais ces valeurs depuis la config au lieu des `#define`/`constexpr`

### Côté UI
- Dans `/settings`, nouvel onglet **Expert** (9ᵉ), **désactivé** par défaut.
- Activation : une case à cocher « J'accepte de modifier les paramètres avancés. Des valeurs incorrectes peuvent endommager les pompes ou dégrader la chimie. » → déverrouille les champs.
- Champs saisis en nombre décimal selon la plage, info-bulle précisant le défaut et la plage autorisée.
- Bouton « Restaurer les défauts » visible à tout moment.
- Bandeau rouge haut de page : « Mode expert actif — modifications appliquées au prochain redémarrage si indiqué ».
- Paramètres pris en compte sans reboot (rechargés dans `PumpController` après `/save-config`).

### Cas limites
- Valeur hors plage saisie → refus côté UI + rappel de la plage. Si soumission forcée → 400 firmware.
- Activation expert sur mobile : la case doit être grande (touch target ≥ 44 px).
- Corruption NVS des champs expert → fallback aux constantes en dur + log `warning` + toast UI.
- Restauration des défauts : bouton unique qui écrase les 11 champs en une seule requête.

## Comportement actuel
Les 11 paramètres sont des `constexpr` dans `config.h` et `constants.h`, modifiables uniquement par build + flash. Voir [docs/features/page-settings.md](../../docs/features/page-settings.md) (tableau ⚠️ des paramètres non exposés).

## Contraintes techniques
- ESP32, offline first
- Ne pas déstabiliser le PID : les nouveaux champs sont lus dans `applyRegulationSpeed()` / `begin()` et reinjetés à la volée
- Mutex `configMutex` respecté
- Validation **stricte** côté firmware : jamais de valeur hors plage en mémoire
- Aucune régression sur les valeurs par défaut (tout utilisateur n'activant pas le mode expert garde exactement le comportement actuel)

## Fichiers potentiellement concernés
- `src/config.h` — nouvelle struct `ExpertParams` (ou extension de `PumpControlParams`)
- `src/config.cpp` — load/save NVS, validation
- `src/pump_controller.cpp` / `.h` — lire les nouveaux champs
- `src/sensors.cpp` — si deadband utilisé côté capteurs, lire depuis config
- `src/web_routes_config.cpp` — validation des champs expert + 400 si hors plage
- `src/constants.h` — garder comme valeurs de défaut, documentées
- `data/index.html` — nouvel onglet Expert dans /settings
- `data/app.js` — handler `setupExpert()` + bouton restore + case à cocher d'activation
- `data/app.css` — styles bandeau rouge
- `docs/API.md` — nouveaux champs config
- `docs/features/page-settings.md` — nouvel onglet + suppression partielle du tableau ⚠️ (les 11 paramètres sortent du « en dur »)
- `docs/subsystems/pump-controller.md` — mise à jour §anti-cycling
- `docs/adr/NNNN-parametres-expert.md` [nouveau] — ADR qui formalise la frontière et les plages
- `CHANGELOG.md` — entrée

## Livrables — check-list
- [ ] A1 — Struct `ExpertParams` (ou extension `PumpControlParams`) dans `src/config.h` couvrant les 11 champs avec leurs défauts
- [ ] A2 — Load/save NVS dans `src/config.cpp` + validation stricte des plages (rejet hors plage)
- [ ] A3 — Migration au boot : NVS absente ou corrompue → fallback aux constantes `constexpr` + log `warning`
- [ ] A4 — `src/pump_controller.cpp` / `.h` lit ces valeurs depuis la config (plus de `constexpr` dans le code de régulation)
- [ ] A5 — `src/sensors.cpp` : si deadband consommé ici, lecture depuis config
- [ ] A6 — `GET /get-config` expose les 11 champs (`min_injection_time_ms`, `max_cycles_per_day`, `ph_start_threshold`, `ph_stop_threshold`, `orp_start_threshold`, `orp_stop_threshold`, `ph_deadband`, `orp_deadband`, `integral_max`, `k_ph_max_error`, `k_orp_max_error`)
- [ ] A7 — `POST /save-config` valide les plages, retourne 400 + message FR si hors plage
- [ ] A8 — Rechargement à chaud des paramètres dans `PumpController` après save (pas de reboot requis)
- [ ] B1 — Nouvel onglet **Expert** dans `data/index.html` (9ᵉ onglet de /settings)
- [ ] B2 — Case d'activation « J'accepte… », touch target ≥ 44 px, déverrouille les champs
- [ ] B3 — Bandeau rouge en haut de l'onglet quand mode expert actif
- [ ] B4 — Pour chaque champ : info-bulle (défaut + plage), validation client (refus hors plage)
- [ ] B5 — Bouton « Restaurer les défauts » (un seul `POST /save-config` avec les 11 champs aux constantes d'origine)
- [ ] B6 — Toast d'erreur si la 400 firmware remonte (corruption NVS / refus serveur)
- [ ] C1 — `docs/API.md` : section config expert (champs + plages)
- [ ] C2 — `docs/features/page-settings.md` : nouvel onglet documenté + tableau ⚠️ allégé (les 11 paramètres sortent du « en dur »)
- [ ] C3 — `docs/subsystems/pump-controller.md` : §anti-cycling (constantes devenues configurables)
- [ ] C4 — `docs/adr/NNNN-parametres-expert.md` [nouveau]
- [ ] C5 — `CHANGELOG.md` : entrée
- [ ] D1 — `pio run` sans nouveau warning, `./build_fs.sh` passe
- [ ] D2 — Test sans activation expert : aucune régression observable
- [ ] D3 — Test corruption NVS : fallback aux défauts + log + toast UI

## Nouvelles signatures / API
- Champs config dans `/get-config` et `/save-config` : `min_injection_time_ms`, `max_cycles_per_day`, `ph_start_threshold`, `ph_stop_threshold`, `orp_start_threshold`, `orp_stop_threshold`, `ph_deadband`, `orp_deadband`, `integral_max`, `k_ph_max_error`, `k_orp_max_error`
- Struct C++ : `ExpertParams` dans `config.h`
- ADR : définir les plages, la politique de défaut, le mécanisme d'activation UI

## ADR attendu ?
- **Probable** : oui
- **Critère(s) candidat(s)** : 1 (contrainte structurante : passage de `constexpr` → config NVS qui sera réutilisée pour d'autres paramètres dans le futur), 4 (verrouillage long terme : la convention de plages + activation explicite devient un pattern répliqué)
- **Sujet pressenti** : `docs/adr/NNNN-parametres-expert.md` — formaliser la frontière (« quels paramètres restent en dur, lesquels deviennent configurables »), la politique de défaut (constexpr = source de vérité de la valeur initiale), les plages, le mécanisme d'activation UI (case à cocher non persistée), et le fallback NVS.

## Sécurité chimique
- Dosage impliqué : **oui** (indirectement — ces paramètres pilotent la régulation)
- Limites journalières / horaires concernées : indirect (anti-cycling, deadband)
- Filtration requise avant action : inchangé
- Calibration capteur modifiée : non
- Algorithme PID touché : **oui** (integralMax, kPhMaxError, kOrpMaxError, deadband)
- Risque identifié : paramètres mal réglés = short-cycling (pompes abîmées), dosage excessif, oscillation pH. Mitigation : plages bornées + avertissement UI + bouton de restauration des défauts.

## Éléments visibles (pour doc-writer)

### Endpoints HTTP concernés
- `GET /get-config` (champs expert)
- `POST /save-config` (champs expert)

### Champs WebSocket
- Aucun (pas d'intérêt temps réel)

### Topics MQTT
- Aucun

### Documentation à produire / mettre à jour

#### Docs vivantes (1 par surface touchée)
- `docs/features/page-settings.md` — nouvel onglet Expert + alléger le ⚠️ tableau (11 paramètres sortent du « en dur »)
- `docs/subsystems/pump-controller.md` — §anti-cycling (constantes devenues configurables)

#### Docs transverses (si contrat externe modifié)
- `docs/API.md` — section config expert (champs + plages)
- `CHANGELOG.md` — entrée

#### Docs immuables
- `docs/adr/NNNN-parametres-expert.md` — formalise la frontière, les plages et le mécanisme d'activation UI

## Critères d'acceptation
- [ ] Sans activation expert : UI exactement identique, comportement identique
- [ ] Après activation + modification : les valeurs appliquées sont prises en compte au prochain cycle PID
- [ ] `/save-config` avec valeur hors plage → 400 + message français précis
- [ ] Bouton « Restaurer les défauts » → remise à la valeur initiale, persistée, appliquée
- [ ] Reboot : les valeurs expert sont rechargées depuis NVS
- [ ] Corruption NVS (valeurs hors plage) : fallback aux défauts + log + toast UI
- [ ] ADR créée
- [ ] Tous les libellés utilisateur sont en français
- [ ] `pio run` compile sans nouveau warning
- [ ] `./build_fs.sh` passe

## Plan de test manuel
1. Sans activation : aucun champ modifiable, tous les comportements actuels validés.
2. Activation + mettre `minInjectionTimeMs = 10_000` → sauver → provoquer une injection courte, vérifier qu'elle dure bien 10 s et non 30 s.
3. Valeur hors plage (`maxCyclesPerDay = 100`) → refus UI + refus firmware.
4. « Restaurer les défauts » → tous les champs reviennent à leur valeur d'origine, dosage à nouveau protégé.
5. Reboot après modification → valeurs préservées.
6. Simuler corruption NVS (écriture manuelle d'une valeur aberrante) → fallback + log.

## Notes pour agent
- **Décision de fond** : le principe **zéro régression sans activation explicite** est non négociable. Toute implémentation doit démontrer qu'un utilisateur qui n'active pas le mode expert ne voit strictement aucune différence.
- **ADR incontournable** : c'est un choix d'architecture (passage de `constexpr` à config NVS) qui sera réutilisé pour d'autres paramètres. Documenter la méthode.
- **`pool-chemistry` doit valider les plages** : notamment les seuils min de `minInjectionTimeMs` (10 s = limite pompe) et les plages de deadband (trop petit = short-cycling sur bruit capteur).
- **Mode expert ≠ mode dev** : l'onglet existe toujours en environnement utilisateur, il n'est pas caché via une variable de compilation. Activation = case à cocher.

## Hors périmètre
- Exposition des gains PID `kp/ki/kd` individuels (déjà réglables via `regulation_speed` slow/normal/fast)
- Modification de `kPumpMaxFlowMlPerMin` (calibration pompe — spec séparée si besoin)
- Export / import d'un profil expert (par ex. fichier JSON) — nice-to-have post-livraison
