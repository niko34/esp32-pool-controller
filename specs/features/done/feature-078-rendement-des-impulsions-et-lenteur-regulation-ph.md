# feature-078 — Rendement des impulsions de dosage et lenteur de la régulation pH

## Métadonnées

| Champ | Valeur |
|---|---|
| Statut | **CLOSE le 2026-10-08** — causes n°1 et n°3 traitées ; M2 abandonnée (deux campagnes ne discriminent pas, et l'entrée dominante était une étiquette) ; modèle de comptage **non corrigé**, décision utilisateur. Voir « CLÔTURE » en fin de fichier |
| Origine | Constat utilisateur du 28/08/2026 : « la correction de pH est très lente, on n'atteint pratiquement jamais la cible sur plusieurs jours » |
| Priorité | Moyenne — dégrade le service, aucun risque de sécurité |
| Analyse | `pool-chemistry`, 28/08/2026 (7 leviers examinés) ; **verdict du 06/09/2026** sur la concordance bidon/compteur |
| Préalable | [feature-110](feature-110-compteur-d-impulsions-de-dosage.md) — compteur d'impulsions : **sans lui, la nature du biais reste indécidable** |

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

> **État au 06/09/2026** : **M1 est FAITE** (26,0 et 25,5 mL/min à duty 127,
> drapeau « mesuré » posé — cf. [feature-075](../done/feature-075-assistant-mesure-debit-pompe.md)),
> ce qui **écarte la cause n°3**. **M2 reste due** : c'est elle, ou son équivalent
> sans effort humain ([feature-110](feature-110-compteur-d-impulsions-de-dosage.md)),
> qui tranchera la **nature** du biais désormais chiffré à **+4,76 %** (voir le
> suivi du 06/09 ci-dessous).

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

### 2026-09-06 — le biais de comptage est MESURÉ : +4,76 %, dans le sens conservateur

La concordance bidon/compteur de [feature-075](../done/feature-075-assistant-mesure-debit-pompe.md)
(critère P12) a été relevée sur **baseline propre** (posée le 31/08 : bidon chlore
mesuré 8 750 mL, compteur remis à 0). **Ce n'est plus une hypothèse : c'est un
chiffre.** Cette spec part désormais de données mesurées.

| Grandeur | Valeur |
|---|---:|
| Compteur appareil (`orp_total_injected_ml`) | **5 290,44 mL** |
| Restant **estimé** | 3 459,56 mL |
| Restant **mesuré** (doseur 3 L à ras + 0,7 L) | **3 700 mL** |
| Injecté **réel** | **5 050 mL** |
| **Biais** | **+240,44 mL = +4,76 %** — l'appareil **sur-compte** |

Incertitude de lecture ~±30-50 mL ⇒ biais réel entre **+3,8 % et +5,8 %** :
**distinguable de zéro**. Fenêtre : **6 jours**, 5 050 mL injectés, amplitude
`duty = 127`, pouls minimal 10 s.

#### Ce que ces chiffres apportent aux hypothèses de cette spec

- **La cause n°2 (perte fixe par démarrage) est CONFIRMÉE dans son sens** — mais
  son ampleur est **bien inférieure** à l'estimation initiale de 15-45 % de
  sur-comptage. Le chiffre à retenir est **+4,76 %** (plancher).
- **Quantification par impulsion** ([`src/pulse_dosing_logic.cpp:136-163`](../../../src/pulse_dosing_logic.cpp)) :
  un pouls minimal est compté **4,573 mL** (kick 0,493 + plateau 4,08) ⇒
  ~**1 160 impulsions** sur la fenêtre ⇒ **0,21 mL de surcoût par impulsion**,
  soit ~**0,5 s de débit perdu au démarrage**. C'est l'ordre de grandeur exact
  d'un transitoire péristaltique — donc **dans la fourchette basse** des 0,1-0,5 mL
  postulés, pas au milieu.
- **~19 % de ce surcoût est VOLONTAIRE et doit être préservé** : le kick est
  crédité à `kPumpFlow255MlPerMin = 74,0` contre **68,0 réellement mesurés**
  ([`constants.h:2114-2136`](../../../src/constants.h),
  [ADR-0050](../../../docs/adr/0050-debit-pleine-puissance-constante-unique-de-flotte.md)).
  ⛔ **Ne pas « corriger » cette marge** en croyant gagner en précision : elle
  couvre une dispersion inter-unités de 11 % déjà constatée sur un même lot.
- **+4,76 % est un PLANCHER, pas une valeur.** Une purge expulse du liquide
  **sans le compter** : elle **masque** une partie du sur-comptage.
  `measures_purge_count` valait 0 au relevé, mais l'appareil avait redémarré
  20 min plus tôt — ce compteur **ne couvre pas la fenêtre**.
- **Réserve sur l'ampleur mesurée** : `kPulseMinMs` porté de 5 à 10 s
  ([ADR-0051](../../../docs/adr/0051-pouls-minimal-double-rendement-avant-comptage.md))
  a **divisé par deux** le nombre d'impulsions. Le +4,76 % est donc mesuré
  **après** l'atténuation — et ce même allongement a réduit d'autant le coussin
  de sécurité involontaire que le sur-comptage constituait.

#### ⛔ NO GO ferme sur un facteur de comptabilité de flotte — verdict `pool-chemistry` du 06/09

Un facteur **multiplicatif** calé sur ce point unique corrigerait **mal** une perte
**par impulsion** : la fraction de biais varie avec la **durée moyenne de pouls**,
donc avec la demande. Sur pouls longs, il **sur-corrigerait et relâcherait les
quotas**. La branche « correctif de flotte » envisagée en feature-075 est
**écartée**, et elle ne pourra être rouverte qu'une fois la **nature** du biais
établie.

**Aucune urgence de sécurité chimique** : le sens est **conservateur** — sur-compter
**resserre** les quotas journaliers et **avance** l'alerte de bidon vide. La
correction du modèle reste donc, comme annoncé, **une hausse de dose** à instruire
comme telle.

#### Comment trancher la nature — deux chemins, tous deux valides

1. **Le chemin sans effort humain** : [feature-110](feature-110-compteur-d-impulsions-de-dosage.md),
   compteur d'impulsions par pompe, persistant, remis à zéro **avec** le volume.
   Volume **et** impulsions ⇒ **deux inconnues, deux équations**. Le relevé se
   produit tout seul au changement de bidon — un geste déjà fait.
   **Sans lui, chaque concordance future sera aussi peu concluante que celle du 06/09.**
2. **Le chemin rapide** — le protocole de banc est **déjà écrit** dans
   [`src/constants.h:2088`](../../../src/constants.h) et repris en M2 ci-dessus :
   **24 pouls de 5 s contre 12 pouls de 10 s, à temps de pompe égal**.
   **Volumes égaux ⇒ biais multiplicatif ; le premier plus faible ⇒ perte fixe
   par impulsion** (`V₁₀ − V₅ = 12 × L`).

## Critères de clôture

- M1 et M2 exécutées, valeurs consignées. *(M1 faite le 28/08 ; M2 due — soit au
  banc, soit par le couple volume/impulsions de
  [feature-110](feature-110-compteur-d-impulsions-de-dosage.md).)*
- **Nature du biais établie** (multiplicatif vs perte fixe par impulsion) **avant**
  toute correction du modèle — ⛔ NO GO sur un facteur multiplicatif de flotte tant
  qu'elle ne l'est pas (verdict `pool-chemistry` du 06/09).
- **Marge délibérée du kick préservée** : `kPumpFlow255MlPerMin = 74,0` contre 68,0
  mesurés — ~19 % du sur-comptage, volontaire, **jamais à « corriger »**.
- Si sur-comptage confirmé : modèle de comptage corrigé **et** hausse de dose
  assumée explicitement, visa `pool-chemistry`.
- Le pH atteint la consigne et s'y maintient sur 3 jours consécutifs.
- Aucun franchissement de `kPhLowWarn` (7,00) ni du seuil 6,70 sur 72 h de suivi.

## Hors périmètre

- La nature et la valeur de l'offset pH — voir mémoire dédiée : l'offset corrige
  un décalage de **mesure** (courants de fuite), le pH affiché est juste. Un
  offset élevé n'est **pas** un signal de risque.

---

## CLÔTURE — 2026-10-08, décision utilisateur

**Statut : CLOSE.** Les deux causes qui dégradaient réellement le service sont traitées depuis
longtemps ; ce qui restait — **M2** (nature du biais de comptage), la correction du modèle et sa
vérification terrain — **perd son objet**.

### Pourquoi M2 ne sera pas exécutée

La deuxième campagne de concordance, qui devait la trancher, a été relevée le **2026-10-08** :

| | Campagne 1 (08/09) | Campagne 2 (08/10) |
|---|---|---|
| Injecté **compté** | 6 989,96 mL | 18 221,21 mL |
| Injecté **réel** | 6 650 mL | 17 150 mL |
| **Biais** | **+5,11 %** | **+6,25 %** |
| Démarrages | 86 | 356 |
| **Biais par démarrage** | **+3,95 mL** | **+3,01 mL** |

**Les deux points ne discriminent pas.** Ni le modèle « perte fixe par démarrage » (−24 % d'écart
relatif) ni le modèle « erreur de débit » (+22 %) n'est constant. Un modèle mixte les ajusterait
exactement, mais deux équations à deux inconnues s'ajustent **toujours** exactement : ça ne
valide rien.

**Et l'entrée dominante est fausse.** La campagne 2 partait de **20 000 mL nominaux** —
l'utilisateur a confirmé le 2026-10-08 qu'il s'agissait de **l'étiquette, pas d'une mesure**.
Or il suffirait que le bidon ait contenu 20 185 mL (**+0,9 %**, banal pour un contenant scellé)
pour que le modèle « pourcentage » redevienne parfaitement constant entre les deux campagnes.
La conclusion est donc **dominée par l'incertitude de l'étiquette**, pas par la physique.

### Décision : ne pas corriger le modèle de comptage

Arbitrage de l'utilisateur, repris tel quel :

> *« une erreur de 1 litre sur 20 litres, est-ce que ce n'est pas acceptable. L'intention est de
> donner une tendance de fin de bidon, pas une mesure parfaitement juste. »*

C'est juste, et le **sens du biais** le confirme : l'appareil se croit **plus pauvre** en chlore
qu'il ne l'est. Il alerte donc **trop tôt, jamais trop tard**. Au seuil de 2 000 mL comptés,
il reste réellement ~3 L. L'erreur inverse aurait laissé les pompes tourner à vide sans prévenir.

Corriger un débit sur deux points discordants, avec une contenance nominale comme entrée,
**introduirait plus d'erreur qu'elle n'en retirerait**.

### ⚠️ Ce que le biais coûte AILLEURS — à connaître, pas à corriger

`accountPumpVolume()` (`pump_controller.cpp:2960-2975`) incrémente **avec la même valeur**
`safetyLimits.daily{Ph,Orp}InjectedMl` **et** `productCfg.{ph,orp}TotalInjectedMl`. Le biais ne
touche donc pas que la jauge :

- un plafond réglé à **2 000 mL/jour** délivre en réalité **≈ 1 880 mL** ;
- une cible ORP de **1 500 mL/jour** en délivre **≈ 1 410**.

Toujours le sens prudent — l'appareil injecte **moins** que ce qui est autorisé, jamais plus.
Mais à retenir : pendant l'épisode d'octobre, tourner « au plafond » signifiait disposer de
**6 % de marge en moins** que ce que l'écran annonçait.

### Ce qui reste vrai de cette spec (acquis, à ne pas défaire)
- **Cause n°1** (loi P, `Ki = 0` structurellement incapable de rejoindre la consigne sous charge
  basique permanente) : **traitée** en v2.26.0.
- **Levier 2** : appliqué en v2.25.3.
- **M1** (débit à l'amplitude, 26,0 et 25,5 mL/min à duty 127) : **faite** le 28/08, elle
  **écarte la cause n°3**.

### Hérité par d'autres specs — ce lot ne les bloque plus
- Le **volume fantôme épisodique** (400 à 950 mL crédités d'un coup sur une coupure) est un
  problème d'un **autre ordre** qu'un biais régulier de 6 % : c'est **bug-115**, débloqué le
  2026-10-08 et traité séparément.
- Toute campagne de concordance future devra partir d'un volume **MESURÉ AU DOSEUR**, jamais du
  nominal — c'est la leçon de ce lot — et porter la **version de firmware**, car bug-115 et
  feature-136 modifient la quantité délivrée : aucun relevé postérieur ne sera comparable à ces
  deux-là.
