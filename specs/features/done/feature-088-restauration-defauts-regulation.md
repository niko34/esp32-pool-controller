## Métadonnées
- **ID** : feature-088
- **Slug** : restauration-defauts-regulation
- **Type** : feature
- **Statut** : done — livré en v2.31.0 (tests verts, revue Approved après correctif)
- **Priorité** : normale
- **Version cible** : 2.31.0
- **Dernière mise à jour** : 2026-08-31
- **Spec(s) liée(s)** : feature-087 (valeurs d'usine recalibrées, v2.30.0)

## Domaines touchés
- [ ] Capteurs (pH / ORP / température)
- [x] Régulation (pump_controller, PID) → **passage obligatoire par `pool-chemistry`**
- [ ] Filtration / Éclairage
- [x] Web — routes HTTP / WebSocket
- [ ] MQTT / Home Assistant
- [ ] UART / écran LVGL
- [ ] OTA
- [x] Authentification (action protégée)
- [x] Historique / persistance (écriture NVS)
- [x] Frontend (`data/`)

## Agents requis
- [x] spec-reader
- [x] architect
- [x] pool-chemistry — **obligatoire** (écriture de paramètres de régulation)
- [ ] ux-ui-designer — non : un bouton + confirmation dans une card existante
- [x] embedded-developer (route + application config)
- [x] web-ui-developer (UI + appel)
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description

Un bouton « Restaurer les valeurs par défaut » dans Paramètres → Avancé,
section régulation pH/ORP, qui remet les paramètres de régulation aux valeurs
d'usine (celles de la v2.30.0), avec confirmation explicite avant application.

## Contexte

Les valeurs d'usine viennent d'être recalibrées sur mesures (feature-087).
Un utilisateur qui a expérimenté des réglages (limites horaires, consignes,
vitesse) n'a aujourd'hui aucun moyen de revenir à un état connu et sûr sans
tout ressaisir à la main — avec le risque de se tromper de valeur.

## Comportement attendu

- Bouton dans Paramètres → Avancé, à proximité des réglages de régulation.
- Clic → **dialogue de confirmation** listant les valeurs qui seront
  appliquées (pas un « êtes-vous sûr ? » vide : montrer AVANT/APRÈS).
- Confirmation → appel HTTP authentifié → application immédiate + persistance
  NVS → l'UI recharge la config et reflète les nouvelles valeurs.
- Message de succès explicite ; journal `systemLogger.info` de l'action avec
  les valeurs appliquées (traçabilité).

### Périmètre de la restauration — À FAIRE ARBITRER PAR pool-chemistry

**Proposition** (réglages de « tuning », restaurables sans danger) :
- Consigne pH (→ 7,3) et consigne ORP (→ 600 mV)
- Limites horaires d'injection pH (→ 10 min/h) et ORP (→ 10 min/h)
- Vitesse de régulation (→ normal)
- Délai de stabilisation (→ 5 min)
- Volumes quotidiens cibles du mode programmé (→ 0)

**EXCLUS d'office** (état physique ou calibration, PAS du tuning) :
- Calibration des pompes (duty, débits pesés — `pumpNFlowWeighed`)
- Type de produit (`phCorrectionType` : acide/base — décrit le bidon
  physiquement branché, le « restaurer » serait déclarer un autre produit)
- Modes de régulation (automatic/scheduled/manual — un état, pas un réglage)
- Mode d'installation, planning de filtration, calibration température
- Tout ce qui touche réseau/MQTT/auth

### Cas limites
- Session de dosage en cours au moment de la restauration : comportement à
  définir (l'empreinte de contexte `phIntegralContextHash` change ⇒ latch FF
  et purge intégrale existants doivent se déclencher — vérifier que c'est
  suffisant, ne rien court-circuiter).
- Restauration pendant une pause de mélange / une stabilisation.
- Client WS multiple : les autres clients doivent voir les nouvelles valeurs
  (rechargement config poussé ou au prochain cycle).
- Firmware antérieur côté client (page en cache) : le bouton absent ne casse
  rien.

## Comportement actuel

Rien n'existe. Chaque paramètre se ressaisit individuellement.

## Contraintes techniques
- ESP32, offline first ; vanilla JS ; français
- Route HTTP authentifiée (POST), AsyncWebServer < 50 ms
- Les VALEURS D'USINE ne doivent exister qu'à UN SEUL endroit (les defaults
  de `MqttConfig` dans config.h) — la route ne doit PAS les dupliquer en dur
  (sinon elles divergeront au prochain recalibrage)
- Flash ~97,9 % : budget firmware ≤ 2 Ko, FS ≤ 1,5 Ko

## Fichiers potentiellement concernés
- `src/web_routes_config.cpp` — nouvelle route POST
- `src/config.h/.cpp` — mécanisme de restauration sans duplication des valeurs
- `data/index.html`, `data/app.js` — bouton + dialogue + rechargement
- `docs/API.md`, `docs/features/page-settings.md`, `CHANGELOG.md`

## Livrables — check-list
- [x] A1 — Restauration côté firmware sans duplication des valeurs d'usine (`src/regulation_defaults.h`, source UNIQUE partagée entre les initialiseurs de `MqttConfig`, `restoreRegulationDefaults()` et `fillRegulationDefaults()`)
- [x] A2 — Route POST authentifiée (`/reset-regulation-defaults`, CRITICAL, corps vide) + journal compact avant → après des 8 champs (IP d'origine, niveau `warning` si la restauration augmente la capacité de dosage)
- [x] A3 — Interaction session en cours arbitrée par pool-chemistry et testée : retune PID par le hook DIFFÉRÉ existant (`applyRegulationSpeed()` → drapeau `_phTuningChangeRequested` consommé par `update()`, invariant v2.26.0 ; un changement effectif de vitesse purge l'intégrale, ADR-0052) — rien n'est court-circuité
- [x] B1 — Bouton + dialogue de confirmation avant/après (changements en gras, confirmation désactivée si rien ne change) + rechargement complet de la config
- [x] C1 — docs/API.md, docs/features/page-settings.md, docs/subsystems/pump-controller.md, docs/subsystems/ws-manager.md, CHANGELOG (v2.31.0)
- [x] D1 — Tests natifs : suite `test_native_regulation_defaults` (template pur `applyRegulationDefaults` instancié sur un double portant TOUS les champs de `MqttConfig`, prédicat `regulationDefaultsRaiseDosing`) — 1007/1007 verts, 4/4 mutants tués dont un `static_assert` prouvé par échec de compilation
- [x] D2 — Revue de code : Request changes → bloquant corrigé (la route répond 503 `success:false` sur timeout du mutex au lieu d'un faux succès) → Approved

## Nouvelles signatures / API
- Endpoint : `POST /reset-regulation-defaults` (auth) — nom indicatif
- Pas de nouveau champ WS/MQTT attendu

## Sécurité chimique
- Dosage impliqué : indirectement — l'action modifie consignes et limites
- Limites concernées : horaires pH/ORP (restaurées), journalières (mode
  programmé remis à 0)
- Filtration requise avant action : non (c'est une écriture de config)
- Calibration capteur modifiée : **non** (exclue du périmètre)
- Algorithme PID touché : non, mais ses entrées de config changent d'un coup
- Risque identifié : restauration en pleine session de dosage ; liste du
  périmètre à faire viser (ne JAMAIS restaurer le type de produit ni la
  calibration des pompes)

## ADR attendu ?
Probablement non (mécanisme local) — sauf si l'architecte identifie un
trade-off structurant sur la non-duplication des valeurs d'usine.

**Décision finale : pas d'ADR.** Mécanisme local sans alternative structurante ;
la source unique est documentée dans le code (`regulation_defaults.h`) et le
périmètre tracé ici + dans le visa pool-chemistry.

## Notes techniques (livraison v2.31.0, 2026-08-31)

### Périmètre visé (pool-chemistry — GO SOUS CONDITIONS, 3 conditions livrées)

8 champs de tuning restaurés : `phTarget` (7,3), `orpTarget` (600 mV),
`ph/orpInjectionLimitMinutes` (10 min/h), `stabilizationDelayMin` (5 min),
`ph/orpDailyTargetMl` (0), `regulationSpeed` ("normal"). JAMAIS touchés :
modes, `ph/orpEnabled`, `phCorrectionType`, `phOffset`, trace `phAutoManual*`,
calibration/sélection des pompes, `SafetyLimits`, `maxCyclesPerDay`.

Les 3 conditions du visa :
1. **Journal avant → après** en UN message compact (8 champs, `x → x` si
   inchangé), niveau `warning` + mention « dosage AUGMENTÉ » si la restauration
   augmente la capacité de dosage (prédicat pur `regulationDefaultsRaiseDosing`).
2. **Verrous compile-time** : `static_assert` de chaque défaut contre sa
   fenêtre de sécurité — un recalibrage hors fenêtre casse la compilation.
3. **Liste des champs copiés = logique PURE** : template `applyRegulationDefaults<Cfg>`
   testé en natif par mutation (exhaustif ET exclusif).

### Mécanisme source unique

`src/regulation_defaults.h` (header pur, ADR-0017) est le seul endroit où les
valeurs d'usine existent. Trois consommateurs : initialiseurs de `MqttConfig`
(config.h), `restoreRegulationDefaults()` (config.cpp — mutex borné, snapshot,
`saveMqttConfig()` dans la même section critique, journal), et
`fillRegulationDefaults()` (web_routes_config.cpp — partagé entre l'objet
`regulation_defaults` de `/get-config` et la réponse de la route). La route est
SANS corps : aucune valeur ne transite par le client.

### Correctif de revue (bloquant)

Première implémentation : timeout du mutex → `success:true` mensonger. Corrigé :
`restoreRegulationDefaults()` retourne `false` (RIEN modifié, annulation en
bloc), la route répond **503** `{"success":false,"error":"Configuration
occupée, réessayez"}` sans retune ni broadcast.

### Fichiers modifiés

- `src/regulation_defaults.h` (nouveau) — constantes, static_assert,
  `applyRegulationDefaults<Cfg>`, `regulationDefaultsRaiseDosing`
- `src/config.h` / `src/config.cpp` — `restoreRegulationDefaults()`,
  initialiseurs référencés sur les constantes
- `src/web_routes_config.cpp` — route `POST /reset-regulation-defaults`,
  `fillRegulationDefaults()`, objet `regulation_defaults` de `/get-config`
  (gabarit 3072 → 3328)
- `data/index.html` — bloc `#regulation_reset_block` (masqué par défaut)
- `data/app.js` — `kRegulationResetFields` (libellés seuls), dialogue
  avant/après, cache `_regulationDefaults`, `syncRegulationDefaults()` ;
  `confirmDialog` étendu (`messageNode`, `confirmDisabled`)
- `test/test_native_regulation_defaults/` (nouveau)
- Docs : API.md, page-settings.md, pump-controller.md, ws-manager.md, CHANGELOG

### Bilan

- Builds verts : `pio run` SUCCESS 0 warning ; **1007/1007** tests natifs ;
  **4/4 mutants tués** (dont static_assert prouvé à la compilation).
- Empreinte : **+2 020 o firmware / +1 378 o FS** — budgets spec respectés
  (≤ 2 Ko = 2 048 o ; ≤ 1,5 Ko = 1 536 o).
- Cas limites : WS multi-clients couvert par le broadcast config différé
  existant ; page en cache d'un firmware antérieur → bouton masqué (objet
  `regulation_defaults` absent).
- Écart de parité PRÉEXISTANT de la trame WS `config`
  (`stabilization_delay_min`, `regulation_speed`, `regulation_defaults`
  absents) relevé au passage → spec dédiée
  `specs/features/todo/bug-089-parite-ws-config-champs-manquants.md`
  (mitigé côté UI : `loadConfig()` frais avant dialogue + cache).
- Pas d'observation terrain requise : fonction déclenchée par l'utilisateur,
  pas par la régulation.
