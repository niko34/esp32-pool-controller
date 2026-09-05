# Feature — Limite de cycles de dosage par jour : défaut fondé (48) et paramètre configurable borné

## Métadonnées
- **ID** : feature-060
- **Slug** : limite-cycles-configurable
- **Type** : feature
- **Statut** : done (livré en v2.12.0 — tests verts 374/374, revue Approved, double GO pool-chemistry)
- **Priorité** : haute (la régulation ORP de l'utilisateur se bloque en cours de journée en régime d'entretien)
- **Version cible** : 2.12.0
- **Dernière mise à jour** : 2026-07-29
- **Spec(s) liée(s)** : verdict pool-chemistry du 2026-07-29 (GO sous conditions — retranscrit ci-dessous), feature-013 (mode expert paramètres)

## Domaines touchés
- [ ] Capteurs (pH / ORP / température)
- [x] Régulation (pump_controller, PID) → **passage obligatoire par `pool-chemistry`**
- [ ] Filtration / Éclairage
- [x] Web — routes HTTP / WebSocket
- [ ] MQTT / Home Assistant
- [ ] UART / écran LVGL
- [ ] OTA
- [ ] Authentification
- [ ] Historique / persistance
- [x] Frontend (`data/`)

## Agents requis
- [x] spec-reader
- [x] architect
- [x] pool-chemistry — **obligatoire** (limite de sécurité dosage modifiée) : verdict pré-design DÉJÀ obtenu (GO sous conditions), re-validation du diff d'implémentation requise
- [ ] ux-ui-designer — non requis (un champ numérique dans une section paramètres existante, pattern établi)
- [x] embedded-developer
- [x] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description
Porter la limite anti-cycling `maxCyclesPerDay` de 20 (codée en dur) à 48 par défaut, et la rendre configurable dans l'UI (paramètres experts) avec des bornes de sécurité strictes clampées côté serveur.

## Contexte
Incident réel (2026-07-29) : régulation ORP bloquée en début d'après-midi (« Limite cycles/jour atteinte — reprise à minuit ») avec seulement 104 mL injectés. Depuis la remontée du stabilisant, la consommation de chlore a chuté (~104 mL/j vs ~3 L) : la régulation travaille par petites retouches (~5 mL/cycle, seuil démarrage 15 mV / arrêt 2 mV, pause mélange 20 min) — un régime **sain** qui produit naturellement 20-40 cycles/jour et épuise la limite de 20.

**Verdict pool-chemistry (2026-07-29, GO sous conditions)** :
- 20/j n'est pas chimiquement fondé ; le chattering rapide est déjà couvert par l'anti-rafale (6/min, 20/15 min, en dur) et la pause mélange borne physiquement la cadence à ~65-70 démarrages/j. La garde ne couvre plus que l'emballement lent (sonde mentant sous la cible) — 48 (1/30 min moyen) le détecte toujours.
- Le blocage actuel (ORP qui dérive tout l'après-midi) est lui-même un risque sanitaire.
- Options refusées : exemption des reprises post-pause-mélange (angle mort sonde menteuse), plafond dynamique lié au volume (quasi illimité à petit volume), statu quo.
- En régime déficit, les cycles sont longs (10 min/h × 30-90 mL/min) : le plafond 3 000 mL/j est atteignable en 4-10 cycles — la limite de cycles ne bride pas le rattrapage.

## Comportement attendu
1. Défaut usine : `maxCyclesPerDay = 48` (au lieu de 20).
2. Paramètre exposé dans l'UI, section paramètres experts (pattern feature-013), libellé français explicite (ex. « Cycles de dosage max/jour (par pompe) »), avec aide contextuelle mentionnant les bornes.
3. Bornes de sécurité **en dur** : min 10, max 60 (`constants.h`), **clampées côté serveur** (route de config) — jamais uniquement côté UI. Valeur hors bornes reçue → clampée + warn loggé.
4. Toute modification du paramètre est loggée (`warn`, ancienne → nouvelle valeur).
5. La valeur est persistée (NVS) et survit au reboot ; s'applique aux DEUX pompes (source unique `pumpProtection.maxCyclesPerDay`, déjà partagée auto + manuel — inchangé).
6. Le message UI « Limite cycles/jour atteinte — reprise à minuit » reste inchangé (il devient simplement rare).

### Cas limites
- Valeur NVS absente (migration depuis ≤ 2.11.x) → défaut 48.
- Valeur NVS hors bornes (écrite par un firmware antérieur/bidouille) → clampée au chargement + warn.
- Modification en cours de journée : le compteur `cyclesToday` en cours n'est PAS remis à zéro — seule la borne de comparaison change (effet immédiat, dans les deux sens).
- UART `set_config` : si le protocole écran expose la config dosage, appliquer le même clamp (vérifier).

## Comportement actuel
`maxCyclesPerDay = 20` codé en dur (config.h:144, struct PumpProtection), non persisté, non exposé. Consommé par `shouldStartDosingPure` (règle 9 dosing_logic) et `evaluateManualInject` — les deux via la même source (à conserver).

## Contraintes techniques
- ESP32, offline first ; vanilla JS ; français.
- **Sécurité chimique — non négociables (verdict pool-chemistry)** : la garde 9 n'est jamais supprimée ; anti-rafale (constants), volume/jour, limite horaire, présence d'eau, pause mélange, watchdog : inchangés. Clamp serveur obligatoire.
- L'ordre des gardes de `dosing_logic` est figé (invariant documenté) — aucun changement de logique, seulement la valeur/bornage de la constante consommée.
- Tests natifs : suite `test_native_dosing` existante à étendre.

## Fichiers potentiellement concernés
- `src/constants.h` — `kCyclesPerDayMin = 10`, `kCyclesPerDayMax = 60`, défaut
- `src/config.h` — défaut du champ `maxCyclesPerDay` (20 → 48)
- `src/config.cpp` — persistance NVS (save/load + clamp au chargement)
- `src/web_routes_config.cpp` — champ dans get-config/save-config + clamp serveur + warn log
- `src/uart_commands.cpp` — même clamp si la config dosage y transite (à vérifier)
- `data/index.html` / `data/app.js` — champ paramètres experts
- `test/test_native_dosing/` — cas limites
- `docs/API.md`, `docs/subsystems/pump-controller.md`, `docs/features/page-*.md` (paramètres), `CHANGELOG.md`

## Livrables — check-list
- [x] A1 — Constantes de bornes `kCyclesPerDayMin/Max` + défaut 48 (constants.h + config.h)
- [x] A2 — Persistance NVS avec clamp au chargement (config.cpp)
- [x] A3 — Clamp serveur + warn log sur la route de config (web_routes_config.cpp) ; vérif/alignement UART si applicable (constat : non applicable — voir notes)
- [x] B1 — Champ UI paramètres experts (index.html + app.js), libellé + aide avec bornes — implémenté dans la carte « Régulation pH / ORP » (la « section paramètres experts » de feature-013 n'existe pas : feature close sans implémentation, réorientation architect)
- [x] D1 — Tests natifs : 48ᵉ cycle accepté / 49ᵉ refusé (auto ET manuel), clamp 5→10 et 100→60, valeur limite exactes 10/60, source partagée auto+manuel
- [x] D2 — `pio run` 0 warning nouveau, suites natives vertes, `./build_fs.sh` OK
- [x] C1 — `docs/subsystems/pump-controller.md` (garde 9 : nouveau défaut, bornes, configurabilité, verdict pool-chemistry référencé)
- [x] C2 — `docs/API.md` (champ config), doc page paramètres (`docs/features/page-settings.md`)
- [x] C3 — `CHANGELOG.md` [2.12.0]

## Nouvelles signatures / API
- Endpoints : champ supplémentaire dans `GET /get-config` / `POST /save-config` (nom exact : architect)
- Constantes : `constexpr unsigned int kCyclesPerDayMin = 10;` `kCyclesPerDayMax = 60;`
- Aucun topic MQTT ni message WS nouveau

## Sécurité chimique (obligatoire)
- Dosage impliqué : oui — pH- ET chlore (limite partagée)
- Limites concernées : anti-cycling journalier (garde 9) uniquement ; toutes les autres inchangées
- Filtration requise avant action : inchangé (garde 2)
- Calibration modifiée : non ; PID touché : non
- Risque identifié : relèvement d'un plafond de sécurité → couvert par le verdict pool-chemistry pré-design (GO sous conditions : 48, bornes 10-60, clamp serveur, log) + re-validation du diff obligatoire

## ADR attendu ?
- **Probable** : non — paramétrage d'une garde existante, décision et alternatives tracées dans cette spec + verdict pool-chemistry ; pas de contrainte structurante nouvelle. L'architect confirme.

## Éléments visibles (pour doc-writer)
### Endpoints HTTP concernés
- `GET /get-config` + `POST /save-config` : nouveau champ (clamp 10-60)
### Champs WebSocket
- aucun
### Topics MQTT
- aucun
### Documentation à produire / mettre à jour
- `docs/subsystems/pump-controller.md`, `docs/API.md`, doc de la page paramètres, `CHANGELOG.md` [2.12.0]

## Critères d'acceptation
- [x] Défaut 48 sur installation neuve ET migration depuis 2.11.x (NVS absent).
- [x] Saisie 100 dans l'UI → serveur enregistre 60 + warn loggé ; saisie 5 → 10 + warn.
- [x] 48ᵉ démarrage accepté, 49ᵉ refusé (natif, auto + manuel).
- [x] Un démarrage refusé par une autre garde (horaire, mélange) ne consomme pas de cycle (non-régression).
- [x] Tous les libellés en français ; `pio run` + suites natives + `./build_fs.sh` verts.
- [x] Re-validation pool-chemistry du diff : GO (2026-07-29).

## Plan de test manuel
1. Après flash : vérifier 48 dans l'UI paramètres experts et `(cycle n/48)` dans les logs de dosage.
2. Saisir 100 → vérifier retour 60 + warn dans le journal ; saisir 25 → appliqué immédiatement.
3. Reboot → valeur conservée.

## Hors périmètre
- Exposition MQTT/HA du paramètre (non demandée).
- Toute modification des autres gardes, du PID, des seuils start/stop, de la pause mélange.
- Le compteur `cyclesToday` lui-même (persistance/rollover inchangés).

## Notes d'implémentation (v2.12.0, 2026-07-29)

### Fichiers touchés
- `src/constants.h` — `kCyclesPerDayMin = 10`, `kCyclesPerDayMax = 60` (+ commentaire « ne JAMAIS élargir sans nouveau verdict pool-chemistry »)
- `src/config.h` — défaut `PumpProtection::maxCyclesPerDay` 20 → 48 (fondement documenté en commentaire)
- `src/config.cpp` — persistance NVS + clamp au chargement + warn hors-bornes
- `src/web_routes_config.cpp` — champ dans `/get-config`, clamp serveur `/save-config`, 2 warns (hors-bornes, modification ancienne → nouvelle)
- `src/dosing_logic.{h,cpp}` — fonction pure `clampCyclesPerDay(long, bool*)`
- `data/index.html` / `data/app.js` — champ `#max_cycles_per_day` (carte « Régulation pH / ORP »), aide, pré-clamp JS indicatif
- `test/test_native_dosing/` — 6 nouveaux tests (374 au total)
- `src/version.h` — 2.12.0 ; docs : `docs/API.md`, `docs/subsystems/pump-controller.md`, `docs/features/page-settings.md`, `CHANGELOG.md`

### Contrats
- Champ JSON (`/get-config` + `/save-config`) : `max_cycles_per_day` (integer, défaut 48, clamp serveur [10, 60])
- Clé NVS : `max_cycles_day` (uint) — absente (≤ 2.11.x) → défaut 48 ; hors bornes → clampée + warn
- `clampCyclesPerDay` est une **sanitization de config, PAS une garde de dosage** : hors de l'ordre figé des gardes de `dosing_logic` (invariant intact)

### Constat UART (cas limite « set_config » de la spec)
`set_config` (protocole écran LVGL) ne transporte **aucun** champ de `PumpProtection` → **aucun clamp UART requis**. Fail-safe : si un futur champ y transitait sans clamp, la valeur serait de toute façon re-clampée au prochain chargement NVS.

### ADR non requis
Aucun des 4 critères de déclenchement n'est rempli (paramétrage borné d'une garde existante, décision + alternatives déjà tracées ici et dans les verdicts pool-chemistry, pattern `/save-config` existant). Précédent : feature-025 (`orpStartThreshold` 10 → 15 mV, pas d'ADR).

### Validations
- **pool-chemistry (2026-07-29, ×2)** : GO pré-design **sous conditions** (48, bornes 10-60, clamp serveur, logs — toutes implémentées) + **GO** sur re-validation du diff d'implémentation.
- **code-reviewer : Approved**, 2 remarques mineures acceptées en l'état : (1) duplication des bornes dans le pré-clamp UI (pattern identique aux champs voisins `ph_limit`/`orp_limit`, le serveur fait autorité) ; (2) valeur > 2³¹ dans le JSON : le cast `long` peut donner une valeur négative, clampée au **min** (10) — comportement conservateur assumé.
- **test-engineer** : 374/374 tests natifs verts (dont 6 nouveaux F060 : 48ᵉ accepté / 49ᵉ refusé auto + manuel, clamps 5→10 / 100→60, frontières exactes 10/60, source partagée), `pio run` 0 warning nouveau, `./build_fs.sh` OK.
