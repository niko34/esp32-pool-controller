# feature-078 — Rendement des impulsions de dosage et lenteur de la régulation pH

## Métadonnées

| Champ | Valeur |
|---|---|
| Statut | todo — **levier 2 appliqué en v2.25.3**, **cause n°1 traitée en v2.26.0** (levée de `Ki = 0`) ; reste en attente des mesures M1/M2 et de la vérification terrain |
| Origine | Constat utilisateur du 28/08/2026 : « la correction de pH est très lente, on n'atteint pratiquement jamais la cible sur plusieurs jours » |
| Priorité | Moyenne — dégrade le service, aucun risque de sécurité |
| Analyse | `pool-chemistry`, 28/08/2026 (7 leviers examinés) |

## Domaines touchés (pressentis)

- `src/pulse_dosing_logic.h` — modèle de comptage du volume par impulsion
- `src/constants.h` — `kPulseMinMs`
- Configuration — `pump1_pulse_flow_ml_per_min` (débit à l'amplitude)

## Sécurité chimique

**Oui — `pool-chemistry` obligatoire avant toute modification.** Toute correction
du comptage **augmente l'acide réellement injecté** : ce n'est pas un correctif
neutre, c'est une hausse de dose. À traiter comme telle.

## Constat

La consigne n'est pratiquement jamais atteinte. Mesures du 28/08 :
`ph_daily_ml = 235 mL` après 3 h 20 de filtration (~71 mL/h), `ph_pulse_ms = 5000`
**en permanence** — toutes les injections sont au pouls minimal.

## Trois causes identifiées, d'ampleur inégale

### 1. La loi P plafonne la demande — cause principale, et ce n'est pas un défaut

`flow = kp × erreur`, kp = 8 (`regulation_speed = normal`).

| Écart pH | Demande | Sur 8 h | Besoin pour combler |
|---|---|---|---|
| 0,22 | 105 mL/h | 845 mL | ~880 mL |
| 0,10 | 48 mL/h | 384 mL | ~400 mL |
| 0,05 | 24 mL/h | 192 mL | ~200 mL |

**Le dosage journalier suit le besoin : il faut donc toujours ~1 journée pour
combler l'écart restant, quel qu'il soit.** Approche asymptotique classique d'une
loi P pure. Pendant cette journée le pH remonte (dégazage CO₂) — le système
s'installe en équilibre sans converger. La pompe n'est utilisée qu'à **6 %** de
sa capacité (25 mL/min × 8 h = 12 L possibles).

### 2. Perte fixe par démarrage — surestimation du comptage

`pulseDeltaVolumeMl` suppose un débit établi **instantanément** :
`0,4 s × 61,0/60 + 4,6 s × 25,0/60 = 2,323 mL`.

Réel, sur péristaltique (analyse `pool-chemistry`) : le coût dominant n'est pas
le kick — la décélération 255 → 127 le compense en partie — mais la **perte fixe
par démarrage** (relaxation puis ré-occlusion du tube, reprise d'amorçage),
typiquement **0,1 à 0,5 mL** sur une pompe dont un tour vaut 1-2 mL.

Estimation : **1,3-2,0 mL délivrés pour 2,32 comptés, soit 15-45 % de
sur-comptage**. Aggravant : toutes les injections étant au pouls minimal, cela
porte sur **100 % du volume**, pas sur une fraction.

### 3. Débit à l'amplitude jamais mesuré

`pump1_pulse_flow_ml_per_min = 25,0` avec `pump1_flow_weighed = false` — valeur
d'usine extrapolée, sur une courbe dont le projet a déjà prouvé la non-linéarité
(modèle linéaire faux ×1,9 en bas, ×1,5 en haut, mesure au banc du 23/08).

## Données chimiques établies (28/08)

Produit : **pH Moins Ultra**, 12 % H₂SO₄ + 13 % HCl ≈ **6,9 éq/L**. TAC déclaré
80-120 mg/L. Bassin 30-50 m³.

Besoin pour 7,50 → 7,30 sur 40 m³ : **~860 mL** par le calcul, **800 mL** par le
dosage constructeur (0,2 L/10 m³ pour 0,1-0,2 unité). **Les deux concordent** —
le dimensionnement n'est donc pas en cause, contrairement à l'hypothèse d'un
acide trop dilué.

## Mesures à faire (par l'utilisateur) — DEUX mesures distinctes

**M1 — débit à l'amplitude** *(décidée par l'utilisateur le 28/08)*. Via
l'assistant de mesure (`/flow-assist/*`) : purge 60 s à duty 255, puis mesure
120 s à duty 127. Donne `q_réel` en **régime établi**. Corrige la cause n°3.
⚠️ **Ne mesure PAS la perte par démarrage** — l'injection y est continue.

**M2 — perte fixe par démarrage**. À temps de pompe égal (120 s) :
24 impulsions de 5 s (60 s d'écart) → V₅ ; puis 12 impulsions de 10 s → V₁₀.
Modèle `V = q·t − L` par démarrage ⇒ **V₁₀ − V₅ = 12 × L**.
Éprouvette 100 mL graduée au mL : un écart ≥ 5 mL est significatif (L ≥ 0,4 mL).
Critère de sur-comptage : `V₅ < 0,90 × 24 × (0,4·61/60 + 4,6·q_réel/60)`.

Précautions communes : aspiration en **eau claire**, refoulement en éprouvette,
**bypass de sondes ouvert** (leçon du 23/08 : 35 min de dosage sur des mesures
non représentatives).

## Leviers, du plus sûr au plus dangereux (analyse `pool-chemistry`)

1. **Mesurer M1 et M2** — risque nul, débloque tout. ⚠️ si `q_réel < 25`,
   corriger le comptage **augmente la dose réelle**.
2. **Allonger le pouls minimal 5 s → 10 s** — ⭐ **meilleur rapport gain/risque,
   retenu comme piste par l'utilisateur.** N'augmente **pas** la dose moyenne
   (l'accumulateur régule sur la moyenne PID) : amortit la perte fixe sur 2× plus
   de volume, améliore le rendement et la reproductibilité. Bolus unitaire
   ~4,6 mL sur 40 m³, négligeable ; temps ON max 16,7 %/période, toujours sous la
   limite de 25 min/h.
3. **kp 8 → 12 (`fast`)** — **déconseillé.** P pur sur un procédé à retard long
   (mélange 15 min + brassage 4-6 h) : dépassement sous pH 7,0 probable. À
   n'envisager qu'après 1 et 2, si le besoin mesuré le justifie.
4. **Amplitude > 50 %** — **contre-productif** : exige une nouvelle pesée, et à
   moyenne constante **raccourcit** les pouls (moins reproductibles). Refusé.
5. **Allonger la période** — neutre sur la moyenne, dégrade la latence. Inutile.
   *(Déjà refusé par l'utilisateur lors du cycle feature-073.)*
6. **Relever la limite journalière** — sans objet (235 mL utilisés sur 2000).

## Suivi

### v2.25.3 (28/08/2026) — levier 2 appliqué, seul

- `kPulseMinMs` : **5 000 → 10 000 ms** (`src/constants.h`). Gate `pool-chemistry` :
  **GO SOUS CONDITIONS**. Formalisé par
  [ADR-0051](../../../docs/adr/0051-pouls-minimal-double-rendement-avant-comptage.md).
- Chiffres retenus (ampFlow **mesuré** 25,75 mL/min) : `V_pouls` **2,47 → 4,61 mL**,
  plancher de demande **2,15 → 4,29 mL/min**, cadence **~1/2,7 min** au lieu de
  ~1/1,4 min à demande 1,76 mL/min. `kPulsePeriodMs` **maintenu à 60 s**.
- ⚠️ **Restent dus** : (1) la **mesure M2** de la perte fixe par démarrage ; (2) la
  correction du modèle de comptage, à traiter comme une **hausse de dose** ;
  (3) la **surveillance du plafond journalier sur 3 jours** (condition n°1 du gate — la
  limite est devenue légèrement moins conservatrice).
- Causes n°1 (loi P) et n°3 (débit d'amplitude, mesure M1) **non traitées** par ce cycle.

### v2.26.0 (28/08/2026) — cause n°1 traitée : levée de `Ki = 0`

- **Diagnostic affiné** : la cause n°1 n'est pas « la loi P plafonne la demande », c'est plus
  précis — **un P pur sous charge permanente ne peut PAS annuler une erreur statique**. Il se
  stabilise sur son **droop** `e* = besoin/(kp × T)`. Vérification : kp = 8, `e*` = 0,358 ⇒
  2,86 mL/min ⇒ 1 800 mL/j pour 628 min ≈ **10,5 h de filtration/j**. Le système était
  **exactement à son équilibre théorique** — ni le firmware, ni le dosage n'étaient en cause.
- **Charge basique quotidienne** : CO₂ (dégazage) ≈ **80 %**, chlore (hypochlorite) ≈ **20 %**.
  ~1 800 mL/j pour **tenir**, ~2 700 pour descendre de 0,2.
- **Livré** : `Ki` levé sur le **seul** pH, `integralMax` **dimensionné par l'autorité**
  (`U = 6,0 mL/min` ⇒ 43 200 / 16 200 / 7 200 pH·s selon la vitesse), intégration **signée**
  sur `erreur − phStopThreshold` par quanta d'1 s, gel d'anti-windup **asymétrique**, module
  pur `src/ph_integral_logic.h`, 3 champs `/data` + WS + MQTT, alerte `ph_integral_saturated`,
  bloc « Rattrapage accumulé » sur la page pH. Gate `pool-chemistry` **GO SOUS CONDITIONS
  C1-C5**. Formalisé par
  [ADR-0052](../../../docs/adr/0052-terme-integral-ph-dimensionne-par-autorite.md) (amende
  l'ADR-0016).
- **Défaut préexistant corrigé au passage** : la bande morte valait `phStartThreshold` (0,05)
  alors que l'hystérésis poursuit jusqu'à `phStopThreshold` (0,01) — la plage `]0,01 ; 0,05]`
  était une **séance vivante à débit nul**.
- **Levier 3 (kp 8 → 12) définitivement écarté** : l'écart résiduel ne tomberait qu'à 0,24
  (pH ~7,54). Atteindre 7,35 en P pur exigerait **kp ≈ 86**.
- **Levier « biaiser la consigne » écarté par principe** : le biais dépend d'une charge qui
  varie (bâche, saison) et exigerait une révision manuelle saisonnière — intenable sur une
  flotte livrée à des tiers.
- Tests **823 → 849** ; 5 mutations tuent désormais (3 ne tuaient rien avant l'extraction).
- ⚠️ **Restent dus** : mesures **M1** et **M2**, correction du modèle de comptage (à traiter
  comme une **hausse de dose**), et la **vérification terrain** de la convergence du pH.

## Critères de clôture

- M1 et M2 exécutées, valeurs consignées.
- Si sur-comptage confirmé : modèle de comptage corrigé **et** hausse de dose
  assumée explicitement, visa `pool-chemistry`.
- Le pH atteint la consigne et s'y maintient sur 3 jours consécutifs.
- Aucun franchissement de `kPhLowWarn` (7,00) ni du seuil 6,70 sur 72 h de suivi.

## Hors périmètre

- La nature et la valeur de l'offset pH — voir mémoire dédiée : l'offset corrige
  un décalage de **mesure** (courants de fuite), le pH affiché est juste. Un
  offset élevé n'est **pas** un signal de risque.
