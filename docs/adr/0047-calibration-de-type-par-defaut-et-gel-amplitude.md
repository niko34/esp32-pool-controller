# ADR-0047 — Calibration de type par défaut pour la flotte + gel de l'amplitude d'impulsion (amende le fail-closed « pesée obligatoire » de l'ADR-0043)

- **Statut** : Accepté
- **Date** : 2026-08-24
- **Décideurs** : gate `pool-chemistry` (GO amont sous conditions C1-C5 + GO en
  contre-visite, 2026-08-24), architect, utilisateur (contexte produit)
- **Spec(s) liée(s)** : [feature-074-calibration-de-type-et-gel-amplitude](../../specs/features/doing/feature-074-calibration-de-type-et-gel-amplitude.md) (v2.23.0)

## Contexte

L'[ADR-0043](0043-dosage-proportionnel-de-temps-amplitude-calibree.md) posait une
calibration **fail-closed sans valeur provisoire** : défauts `pumpN_calibrated_duty = 0`
et `pumpN_pulse_flow_ml_per_min = 0`, tout dosage automatique/programmé **refusé** tant
que l'utilisateur n'a pas pesé le débit de SA pompe. Décision adaptée à **un** appareil
en cours de mise au point.

Le **contexte produit a changé** : l'utilisateur va fournir des appareils à des tiers,
tous équipés du **même modèle de pompe péristaltique**. Deux conséquences :

1. **Aucun tiers ne pèsera jamais** — le fail-closed en flotte signifie un dosage mort
   à vie sur chaque exemplaire livré. Les doseuses du commerce embarquent un **débit
   nominal du modèle** (calibration de type), raffinable par mesure individuelle.
2. Le curseur d'amplitude (`pumpN_max_duty_pct`) est devenu un **piège** depuis le
   verrou d'égalité exacte amplitude == duty calibré : tout déplacement arrête le dosage
   jusqu'à re-pesée, et le champ a déjà été réécrit **deux fois** par des sauvegardes
   d'onglets périmés.

Le verdict feature-073/2.21.0 du gate (« pesée obligatoire, pas de valeur provisoire »)
devait être **ré-arbitré** sur ce dossier nouveau — c'est chose faite (GO).

## Décision

### Calibration de type par défaut

Les défauts de configuration passent de « non caractérisé » (0, fail-closed) à la
**calibration de type** (valeur usine du modèle de pompe) :

- `kPumpTypeCalDuty` **= 127**, dérivé de `(kPumpAmpDefaultPct × MAX_PWM_DUTY) / 100`
  (amplitude par défaut 50 %) — **même expression entière** que `getPumpAmplitudeDuty`,
  verrouillée par `static_assert` (== 127 et ≥ `MIN_ACTIVE_DUTY`) ;
- `kPumpTypePulseFlowMlPerMin` **= 28.0 ml/min** — mesures de référence du 2026-08-23
  (P1 : 12,0 @82 et 61,0 @255 → interpolation 24,7 @127 ; P2 : ~27,2 @127 ; dispersion
  inter-unités ±10 %). **Provisoire assumé** : à remplacer par la pesée de type à
  duty 127 sur l'unité de référence.

**Sémantique NVS** : clé **absente** (appareil vierge) → défaut de type, le dosage
fonctionne au premier flash ; clé **présente à 0** (effacement explicite via l'API, ou
appareil ≤ 2.22 jamais calibré ayant sauvegardé sa config) → 0 est lu tel quel,
**fail-closed conservé** — aucune conversion silencieuse. Toute la machinerie 2.21.0
(`PulseRefusal::FlowUncharacterized`, badge, critical throttlé) est inchangée : seuls
les DÉFAUTS changent.

**Drapeaux `pumpN_flow_weighed`** (NVS `p1/p2_flow_weighed`, défaut `false`) :
distinguent « valeur usine » d'une « pesée individuelle ». **Purement informatifs**
(indication UI) — **aucun gate ne s'y fie**. La garde de plausibilité du chargement NVS
rabat aussi `weighed = false` (une pesée corrompue n'en est plus une).

### Conditions du gate `pool-chemistry` (C1-C5)

| # | Condition | Réalisation |
|---|---|---|
| C1 | Clé NVS absente = vierge → défaut de type ; clé présente à 0 = fail-closed conservé (jamais de conversion silencieuse) | [`config.cpp`](../../src/config.cpp) `loadMqttConfig()` |
| C2 | `kPumpTypeCalDuty` dérivé de la **même expression entière** que `getPumpAmplitudeDuty` — l'égalité exacte amplitude == duty calibré du gate d'impulsion est garantie **par construction** (pas de divergence float/arrondi) | [`config.h`](../../src/config.h) + `static_assert` |
| C3 | Biais **SÛR** de la valeur de type : 28.0 est AU-DESSUS des interpolations mesurées — surestimer le débit ⇒ la pompe tourne **moins** (sous-dosage) ET les compteurs créditent **plus** (les limites mordent plus tôt) | [`constants.h`](../../src/constants.h) |
| C4 | La garde de plausibilité NVS rabat aussi `weighed = false` (pas de « pesée » fantôme après corruption) | [`config.cpp`](../../src/config.cpp) |
| C5 | Les drapeaux `weighed` restent **purement informatifs** — aucune décision de dosage ne s'y adosse | `evaluatePulseGate` inchangé |

### Gel de l'amplitude d'impulsion

- **UI** : curseurs « Puissance max » retirés, remplacés par une ligne **lecture seule**
  « Amplitude d'impulsion : X % — débit Y ml/min (valeur usine\|pesé) ». Le champ
  `pumpN_max_duty_pct` est **purgé des deux payloads de save** du frontend — immunité
  contre les sauvegardes d'onglets périmés.
- **Serveur** : champ + validation **[40..100]** conservés (rétrocompat API, modifiable
  par API seule pour cas exceptionnel — précédent `screen_enabled` : surface UI retirée,
  contrat serveur conservé).

### Pesée simplifiée (raffinement individuel, facultatif)

Formulaire à **champ unique** « débit mesuré (ml/min) » (vide/0/NaN refusés). Le
frontend n'envoie plus le duty : un débit valide reçu **sans** clé duty est une pesée à
l'amplitude **courante** — le serveur déduit `cal_duty = pumpN_max_duty_pct ×
MAX_PWM_DUTY / 100` et pose `weighed = true`. L'effacement (`duty = 0`) remet
`flow = 0` **et** `weighed = false`. Le badge d'alerte « non caractérisé » est
**restreint** au cas `cal_duty = 0`, seul état encore fail-closed.

## Alternatives considérées

- **Conserver le fail-closed en flotte (ADR-0043 inchangé)** (rejetée) — correct pour
  un appareil unique en mise au point ; en flotte, aucun tiers ne réalisera jamais la
  pesée : le dosage serait mort à vie sur chaque exemplaire livré.
- **Valeur de type + limites journalières réduites** (rejetée par le gate) — jugée
  inutile : les défauts struct des limites (300 ml/j pH, 500 ml/j chlore) sont déjà
  prudents et bornent l'exposition réelle d'un appareil neuf ; réduire davantage
  n'apporte rien face à un biais de type déjà orienté sous-dosage.
- **Convertir les NVS présents-à-0 vers le défaut de type** (rejetée, C1) — l'utilisateur
  de CET appareil avait un état explicite (effacement ou jamais-calibré assumé) ; le
  convertir silencieusement créditerait un débit jamais validé sur cette unité.
- **Faire porter un gate sur `weighed`** (rejetée, C5) — le drapeau est déclaratif ;
  le seul verrou de sécurité reste la calibration elle-même (duty + débit plausible +
  égalité amplitude).

## Résidus statués (assumés)

- **(a) API duty-seul → `weighed = true`** : un vieux client qui envoie encore un
  `pumpN_calibrated_duty` explicite non nul pose le drapeau « pesé ». Acceptable :
  le drapeau est informatif, **sans gate** (C5).
- **(b) Appareil 2.21/2.22 n'ayant JAMAIS sauvegardé sa config** (clés NVS absentes) :
  recevrait la calibration de type au flash 2.23.0 au lieu de rester fail-closed. Cas
  **étroit** (toute sauvegarde de config écrit les clés), équivalent à un appareil
  vierge — assumé.

## Conséquences

### Positives
- Un appareil **neuf dose immédiatement** au premier flash, sur une valeur mesurée du
  modèle de pompe — le pattern des doseuses du commerce.
- L'amplitude n'est plus un piège : plus de dosage arrêté par un déplacement de curseur
  ni d'écrasement par onglet périmé.
- La provenance du débit (usine/pesé) est visible et honnête ; la pesée individuelle
  reste possible et prime.

### Négatives / dette assumée
- La valeur de type peut être fausse de **±10 %** pour une unité donnée (dispersion
  tube constatée) — biais orienté **sous-dosage** (C3), limites journalières/horaires
  inchangées en filet.
- `kPumpTypePulseFlowMlPerMin = 28.0` est une **interpolation**, pas une pesée à 127 —
  dette explicite, à solder par la pesée de type sur l'unité de référence.
- Le serveur accepte toujours `pumpN_max_duty_pct` d'un client périmé (rétrocompat) —
  l'immunité ne vaut que pour le frontend embarqué à jour.
- Résidus (a) et (b) ci-dessus.

### Ce que ça verrouille
- `kPumpTypeCalDuty` reste **dérivé de l'expression entière de l'amplitude par défaut**
  (`static_assert`) : changer l'amplitude par défaut sans changer le duty de type (ou
  inversement) ne compile pas.
- Toute modification de `kPumpTypePulseFlowMlPerMin` exige une **mesure au banc** +
  gate `pool-chemistry` (même règle que les constantes ADR-0043).
- La sémantique NVS « présent-à-0 = fail-closed » est un contrat : la convertir = nouvel
  ADR + gate.
- `pumpN_flow_weighed` reste informatif : l'utiliser dans une décision de dosage = nouvel
  ADR + gate.
- Le « pesée obligatoire, pas de valeur provisoire » de l'ADR-0043 est **amendé** pour
  les DÉFAUTS uniquement — le reste de l'ADR-0043 (proportionnel de temps, kick,
  égalité exacte, aucune interpolation runtime) est inchangé.

## Références

- Code : [`src/constants.h`](../../src/constants.h) (`kPumpTypePulseFlowMlPerMin`),
  [`src/config.h`](../../src/config.h) (`kPumpAmpDefaultPct`, `kPumpTypeCalDuty`,
  `pumpNFlowWeighed`, static_asserts), [`src/config.cpp`](../../src/config.cpp)
  (sémantique NVS, garde de plausibilité), [`src/web_routes_config.cpp`](../../src/web_routes_config.cpp)
  (duty déduit de l'amplitude, drapeau weighed), [`data/app.js`](../../data/app.js) /
  [`data/index.html`](../../data/index.html) (curseur retiré, ligne lecture seule,
  formulaire à champ unique)
- Spec : [feature-074-calibration-de-type-et-gel-amplitude](../../specs/features/doing/feature-074-calibration-de-type-et-gel-amplitude.md)
  (justification de 28.0, mesures du 2026-08-23)
- ADR liés : [ADR-0043](0043-dosage-proportionnel-de-temps-amplitude-calibree.md)
  (fail-closed des défauts amendé — le reste inchangé), [ADR-0046](0046-accumulateur-demande-sous-pouls-minimal.md)
  (accumulateur, indépendant — s'applique tel quel à la calibration de type)
- Doc : [pump-controller.md](../subsystems/pump-controller.md),
  [page-settings.md](../features/page-settings.md), [API.md](../API.md)
