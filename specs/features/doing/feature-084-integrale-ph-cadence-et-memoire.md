# feature-084 — L'intégrale pH n'apprend que 25 % du temps et perd sa mémoire

## Métadonnées

| Champ | Valeur |
|---|---|
| Type | bug de conception (le mécanisme fonctionne, sa cadence est fausse) |
| Statut | **doing — implémentée, testée (932 tests natifs verts), revue OK, non déployée** |
| Priorité | **Haute** — gain important pour un coût faible ; à faire même si `feature-085` est retenue |
| Origine | Critique utilisateur du 30/08/2026, analyse `pool-chemistry` du même jour |
| Version en cours | **2.27.0** (non déployée — reste dans `doing/`) ; base : 2.26.0 |
| Gate `pool-chemistry` | ✅ **GO** — 30/08/2026, après réexamen du plafond de restauration |
| ADR produits | [ADR-0053](../../../docs/adr/0053-persistance-conditionnelle-integrale-ph.md), [ADR-0054](../../../docs/adr/0054-seuil-de-vraisemblance-fonction-de-la-configuration.md) |

## Domaines touchés

Prévus :

- `src/pump_controller.cpp` — point d'appel de `phIntegralTick()`
- `src/ph_integral_logic.h` — seuil du reset sec
- `src/config.cpp` — persistance éventuelle (voir arbitrage)
- ADR-0052 (à amender), `docs/subsystems/pump-controller.md`

Réellement touchés (v2.27.0) — l'écart tient à l'anneau de charge, qui n'était
pas anticipé :

- `src/ph_persist_logic.h` (**nouveau**, module pur), `src/ph_integral_logic.h`
- `src/pump_controller.{h,cpp}`, `src/config.{h,cpp}`, `src/constants.h`
- `src/web_server.cpp`, `src/web_routes_config.cpp`, `src/mqtt_manager.cpp`
  (flush des compteurs avant redémarrage), `src/version.h`
- `test/test_native_ph_persist/` (**nouveau**), `test/test_native_ph_integral/`
- `docs/adr/0053-*`, `docs/adr/0054-*` (**nouveaux**), `docs/adr/0052-*`
  (annotation de statut), `docs/adr/README.md`, `docs/README.md`,
  `docs/subsystems/pump-controller.md`, `docs/UPDATE_GUIDE.md`, `CHANGELOG.md`

## Sécurité chimique

**Oui — `pool-chemistry` obligatoire.** Tout ce qui accélère l'accumulation de
l'intégrale accélère la montée de la dose. Le gain visé est réel, le risque aussi.

## Le constat

L'utilisateur : *« au bout de 7 h, la régulation n'arrive pas à compenser […]
peut-être sur 2-3 jours si jamais on n'a pas de perturbations externes »*.

**Il a raison, et le chiffre est exactement celui qu'il pressentait.**

`phIntegralTick()` n'est appelé que dans la **branche automatique**, elle-même
court-circuitée par `canDose(0)` : pas pendant les pauses de mélange, pas
filtration coupée. Rapport cyclique ≈ 7 h/24 h × 0,85 ≈ **25 %**.

**Conséquence : `Ti` = 6 h « de branche » vaut ≈ 24 h de temps réel.**

Chiffrage : tenir 4,3 mL/min (1 800 mL sur 420 min) à une erreur de 0,15 exige
≈ 8 400 pH·s, soit 52 % du plafond. À (0,15 − 0,01) × 3 600 = 504 pH·s/h de
branche, il faut **16,7 h de branche ≈ 2,4 jours de filtration**.

### Trois aggravants qui effacent l'apprentissage

1. **Non persistée en NVS** — tout redémarrage remet le rattrapage à zéro. Le
   PANIC du 29/08 (`bug-080`) a donc coûté plusieurs jours d'apprentissage.
2. **Reset sec** à `erreur < −(startThreshold + 0,10)` ≈ −0,15 — **une pluie
   suffit**. L'analyse du 30/08 le souligne : 20 mm sur 32 m² ne représentent que
   1,6 % de dilution mais l'eau de pluie est légèrement acide ; si l'écart passe
   sous −0,15, plusieurs jours d'apprentissage disparaissent d'un coup.
3. **Péremption à 24 h** d'inactivité — un week-end sans filtration remet à zéro.

Le mécanisme est donc correct mais **structurellement incapable de converger sur
une installation réelle**, où les à-coups sont plus fréquents que 2,4 jours.

## ⚠️ Gain réel — corrigé par le gate du 30/08, à lire avant d'implémenter

**Cette feature ne règle PAS le problème de fond.** Une première estimation
annonçait qu'elle « récupérerait les 75 % de temps mort » : c'est **faux**. Le
facteur limitant n'est pas les pauses de dosage, ce sont les **7 heures de
filtration sur 24**.

| Sujet | Gain réel |
|---|---|
| 1. Cadence du tick | rapport cyclique 25 % → **29 %**, convergence 2,4 j → **2,05 j** (**−15 %**) |
| 2. Reset temporisé | une perturbation transitoire n'efface plus 2,4 jours d'apprentissage |
| 3. Persistance NVS | un redémarrage coûte **~1 h au lieu de 2,4 j** |

**C'est donc une feature de ROBUSTESSE, pas de vitesse.** Elle protège l'acquis
contre les à-coups et les redémarrages — d'autant plus utile que le redémarrage
sur PANIC du 29/08 (`bug-080`) n'est pas élucidé. La réponse au problème de fond
reste `feature-085` (dose de base apprise).

## Ce qui est demandé

### 1. Calculer l'intégrale hors de la branche de dosage

Déplacer `phIntegralTick()` hors de la conjonction, sur le modèle de
`tickPhGuardSurveillance()` (v2.25.0) : la **surveillance** avait déjà été
séparée de la **décision** pour la même raison — elle devenait muette
précisément quand elle importait.

⚠️ **Conditions à instruire** : l'intégrale doit-elle continuer d'accumuler
filtration coupée ? Chimiquement **non** — le pH mesuré en eau stagnante n'est
pas représentatif (leçon des mesures du 28/08). Mais pendant une **pause de
mélange**, oui : l'eau circule, la mesure est valide, seul le dosage est suspendu.
Le bon prédicat est probablement « mesure valide et représentative », pas
« autorisé à doser ».

### 2. Assouplir le reset sec

Un reset total sur un écart transitoire de −0,15 est disproportionné au regard
d'un apprentissage de 2,4 jours. Pistes : exiger une **persistance** de l'écart
négatif (N minutes), ou remplacer le reset sec par une décroissance accélérée.
La décroissance à demi-vie 1 h existe déjà — pourquoi un reset sec en plus ?

### 3. Arbitrage à trancher — la persistance NVS

**L'ADR-0052 l'interdit explicitement** : « ne jamais persister l'intégrale — un
reboot doit repartir de zéro (c'est un filet), et une valeur stockée corrompue
serait une **autorisation de dosage pré-chargée** ».

L'analyse du 30/08 recommande pourtant de la persister. **Il y a contradiction, et
elle doit être tranchée explicitement par `pool-chemistry`, pas contournée.**

> ✅ **TRANCHÉ le 30/08/2026 — GO.** L'ADR-0052 est **amendé** par
> l'[ADR-0053](../../../docs/adr/0053-persistance-conditionnelle-integrale-ph.md)
> (statut de l'ADR-0052 annoté ; tout le reste inchangé). Les trois pistes
> intermédiaires listées ci-dessous sont **toutes** retenues, et quatre autres
> s'y ajoutent — voir les sept garanties R1-R7 dans les notes techniques.

Éléments pour l'arbitrage :
- l'argument de l'ADR reste valide : une valeur corrompue en NVS = dose armée
  d'avance ;
- mais un redémarrage coûte aujourd'hui 2,4 jours de convergence ;
- pistes intermédiaires : persister avec un **plafond de restauration** très
  inférieur au plafond courant (ex. 30 %), avec **somme de contrôle**, avec
  **péremption** (ignorer si l'écriture date de plus de N heures), ou ne restaurer
  que si le pH mesuré au boot **confirme** un écart positif.

## Critères d'acceptation

- [x] L'intégrale progresse pendant les pauses de mélange (mesure valide)
- [x] Elle **ne** progresse **pas** filtration coupée (mesure non représentative)
      — garantie **structurelle** : `update()` sort avant le point d'appel
- [x] Un écart négatif transitoire (< N min) n'efface plus l'apprentissage
      — `kPhIntegralResetHoldMs` = 15 min de temps de branche, seuil inchangé
- [x] Décision sur la persistance NVS **tranchée et documentée** (ADR-0052 amendé
      ou confirmé), quelle que soit l'issue — **amendé** par l'ADR-0053
- [ ] Temps de convergence mesuré avant/après, sur données réelles
      — ⏳ **ouvert** : exige le déploiement OTA, qui n'a pas eu lieu
- [x] Aucun dépassement sous la consigne introduit par l'accélération
      — vérifié en boucle fermée native ; gel d'anti-windup étendu à
      l'anti-cycling épuisé, plafond de restauration sous la moitié de
      l'équilibre observé. ⏳ Confirmation terrain à faire au déploiement
- [x] Visa `pool-chemistry` — **GO**, 30/08/2026

## Hors périmètre

- Le feedforward — voir `feature-085`, qui répond au même problème par une autre
  voie. Les deux sont **complémentaires** : celle-ci corrige le correcteur,
  celle-là lui évite d'avoir à tout apprendre.

## Notes techniques (v2.27.0)

### ⚠️ Ce que le gate a établi, et qui dépasse cette spec

**L'installation est limitée par l'ACTIONNEUR, pas par le correcteur.** À 5 min/h
de limite horaire et ~26 mL/min de débit d'amplitude, le maximum physiquement
délivrable vaut **~2,17 mL/min**, soit **~911 mL/jour** sur 7 h de filtration,
pour un besoin de **~1 800 mL/jour**. **Ni feature-084 ni feature-085 ne peuvent
combler cet écart.**

Corollaire : l'alerte « régulation saturée » est **chimiquement vraie et
légitime** — elle désigne la **limite horaire**, pas un bidon vide.

⚠️ Relever `phInjectionLimitMinutes` est le seul levier, et il n'est pas neutre :
il augmente **proportionnellement** le volume injecté pendant les 15 min de
retard de mélange — **130 mL à 5 min/h ⇒ 0,046 pH** ; **780 mL à 30 min/h ⇒
0,28 pH**, à comparer à `kPhSafetyMargin` = 0,50.

### Sujet 1 — cadence

`phIntegralTick()` sort de la conjonction `canDose(0)` et rejoint
`tickPhGuardSurveillance()`. Prédicat `phIntegralAccumulationAllowed()` =
`canDose(0)` **moins** {pause mélange, limite horaire, quota journalier,
anti-cycling, plafond de session}, **plus** `phMode == "automatic"`. La sortie
reste gatée à l'identique. Rapport cyclique **25 % → 29 %**, convergence
**2,4 j → 2,05 j**.

Effet de bord traité : les paliers d'alerte de saturation sont **recalibrés**
(2 h → **2 h 20**, 6 h → **7 h**, 12 h → **14 h**) pour préserver la **fenêtre de
preuve en temps réel** validée en v2.26.0. ⚠️ Une première rédaction avait retenu
un facteur ×4 issu d'une analyse d'impact préliminaire : un bidon vide aurait été
détecté **3,45 × plus lentement**. Trois `static_assert` verrouillent le rapport.

Gel d'anti-windup **étendu à l'anti-cycling épuisé**, par symétrie avec le quota
volumique (il ne se relève qu'au basculement de 24 h : épuisé à 10 h, il laissait
charger neuf heures, ~1 300 pH·s servis d'un bloc le lendemain).

### Sujet 2 — reset sec temporisé

Seuil **inchangé** (−0,15, exigence du gate) ; la purge exige désormais
**15 min continues de temps de branche**. Le différé **ne suspend aucune
protection** : la décharge signée et l'oubli exponentiel travaillent pendant
l'attente (−16 % d'intégrale sur 15 min). 15 min = `kPhMixingDelayMs`.
Conséquence sur l'ordre des étapes du module pur : la base de temps passe
**devant** le reset sec (elle produit le `dt` que la tenue accumule).

### Sujet 3 — persistance NVS : ADR-0052 AMENDÉ, pas contourné

L'objection se décompose en **deux volets** : le volet **corruption** est traité
par le **scellement** (taille exacte + empreinte FNV-1a + schéma, aucune
réinterprétation partielle) ; le volet « **pré-chargée** » est annulé **par R4**,
pas par R1/R2 — la valeur n'est **jamais appliquée au boot**, elle attend 5 min
de temps de branche avec une erreur au-dessus du seuil de démarrage, état dans
lequel la boucle aurait de toute façon ouvert une session sur le seul terme
proportionnel. **Ce qui est restauré n'est pas une autorisation de doser, c'est
la vitesse d'une correction déjà décidée par la mesure courante.**

Sept garanties **cumulatives** : R1 double plafond `min(0,30 × integralMax ;
0,50 × équilibre observé)` — R2 CRC + schéma + empreinte de contexte — R3
péremption 6 h réévaluée en continu — R4 confirmation par la mesure — R5 cadence
d'écriture **conjonctive** (30 min **ET** 5 %) — R6 journalisation — R7
anti-boucle de démarrage.

⚠️ **Condition posée par le gate** : le terme `0,50 × (charge − kp·seuil)/ki` est
**LE PORTEUR de l'argument de sûreté** ; l'intégrité de la charge observée
(fraîcheur de l'historique, plausibilité du dénominateur) est une **propriété de
SÉCURITÉ**, pas un confort de diagnostic. Et le forfait `0,30 × integralMax`
**seul ne suffit pas** sous **1,88 mL/min** de charge — ce qui concerne les
bassins de la flotte moins chargés que celui de référence.

**Trois défauts fail-open trouvés en revue puis corrigés** (tous surévaluaient la
charge, donc le plafond) : (1) **fenêtre d'âge absente** sur l'anneau (7 journées
*archivées* ≠ 7 derniers *jours* : 1,5 × l'équilibre courant après un arrêt de
deux mois) ; (2) **dénominateur perdu** sur les chemins sans flush (flush 30 s
derrière quatre `return` ; charge 4 × trop grande) ; (3) **purge sèche
n'abandonnant pas la candidate** (la candidate rechargeait l'autorité que la
purge venait d'invalider).

### Sujet 3 bis — l'anneau de charge, qui n'existait pas

`phPersistRestoreCap()` consomme une charge observée que rien ne fournissait :
`days = 0` ⇒ persistance inerte. D'où `PhLoadHistory` (7 journées
`{date, mL, minutes}`, 64 o, `pool-daily`/`ph_load7`, CRC FNV-1a, une écriture
par jour, alimentée aux **deux** points de basculement de date existants,
déduplication sur toute l'étendue, **médiane**).

**Le dénominateur (`dailyFiltrationMinutes`) existe parce que la charge se mesure
en mL/min, pas en mL/jour.** Seuil de `dt` **distinct et plus large** (60 s contre
10 s) : le sens du fail-closed est **inversé** — un `dt` écarté tronque le
dénominateur alors que le numérateur est crédité sur le delta entier.

⚠️ **Usure NVS, chiffrée honnêtement** : `_dailyCountersDirty` posé une fois par
minute de circulation ⇒ **~420-600 transactions `pool-daily`/jour contre
~70-100**, soit un **facteur 5-6**. Choix **délibéré** : numérateur et
dénominateur persistés ensemble, donc perte bornée à ≤ 1 min sur **chacun** et
rapport préservé.

### Correction annexe

`constants.h` affirmait que les deux rapports cycliques étaient des « **MESURES**,
pas des estimations » — **faux** (0,29 = 7/24, le calcul de la spec privé de son
facteur 0,85), et écrit dans le paragraphe même qui interdit de recalibrer sur une
estimation. Ce sont des **estimations analytiques**, dont le sens d'erreur est
**conservateur** (alertes plus précoces, jamais de silence).

## Suivi / itérations

| Itération | Périmètre | Statut |
|---|---|---|
| 1 | Sujets 1 + 2 + 3 complets : cadence du tick hors conjonction, reset sec temporisé, persistance NVS conditionnelle R1-R7, anneau de charge sur 7 jours + dénominateur, recalibration des paliers, gel sur anti-cycling épuisé, points de flush | **livré — tests verts (932/932), revue OK** (3 défauts fail-open relevés puis corrigés), visa `pool-chemistry` **GO**. Fichiers : `src/ph_persist_logic.h` (**nouveau**), `src/ph_integral_logic.h`, `src/pump_controller.{h,cpp}`, `src/config.{h,cpp}`, `src/constants.h`, `src/web_server.cpp`, `src/web_routes_config.cpp`, `src/mqtt_manager.cpp`, `src/version.h`, `test/test_native_ph_persist/` (**nouveau**), `test/test_native_ph_integral/`, docs (`docs/adr/0053`, `docs/adr/0054`, `docs/adr/README.md`, `docs/README.md`, `docs/adr/0052` — annotation de statut, `docs/subsystems/pump-controller.md`, `docs/UPDATE_GUIDE.md`, `CHANGELOG.md`) |
| 2 | **Déploiement OTA + vérification terrain** : temps de convergence avant/après sur données réelles, absence de dépassement sous la consigne, comportement de la restauration après un redémarrage réel (journal `Intégrale pH restaurée : …`), profondeur d'historique atteinte après 3 jours | ⏳ **à faire** — c'est ce qui maintient la spec dans `doing/` |
| 2 — **constat terrain du 03/09/2026** | Redémarrage **réel** (PANIC 08:59:47, cf. bug-080) : le test de restauration prévu « vers le 4-5/09 » a eu lieu involontairement. Verdict : **`Intégrale pH : instantané NVS REJETÉ (instantané périmé) — la régulation repart de zéro`** (R3). Cause : le dernier instantané datait de la veille au soir — depuis, l'intégrale n'a varié que de 2 556 → 2 188 → 2 357 pH·s, soit **moins que le seuil d'écriture** (`kPhIntegralPersistMinDeltaFrac` 5 % × 16 200 = 810 pH·s), donc aucune écriture le matin ; à 08:59 l'âge dépassait `kPhIntegralPersistMaxAgeS` = 6 h. **Conséquence structurelle** : l'intégrale est gelée toute la nuit (pas d'évaluation hors filtration) et la fenêtre 19:03 → 06:57 dure 12 h ⇒ **tout redémarrage matinal avant la première écriture du jour rejette un instantané pourtant exact** — les R1-R7 ne peuvent jamais s'exercer dans le scénario de redémarrage le plus probable. Point à trancher à la clôture (pas de correctif sans `pool-chemistry`) : (a) mesurer l'âge en **temps de filtration** plutôt qu'en temps mural, (b) forcer une écriture à l'arrêt de filtration (sortie du planning) et/ou à la reprise, (c) allonger l'âge à > 12 h. Le compteur journalier (130 mL) et l'anneau `ph_load7` (3 j, FF 1,41 réactivé à 09:15:54) ont, eux, survécu. L'intégrale s'est reconstruite à 733 pH·s en 85 min (pH 7,59 → 7,47 à 10:40). | ⏳ à statuer à la clôture |
| 3 (hors périmètre, suivi) | Le problème de fond — l'installation est **limitée par l'actionneur** (~911 mL/j délivrables pour ~1 800 mL/j de besoin) | ⏳ **ouvert** — suivi dans [feature-086](../todo/feature-086-limite-actionneur-dosage-ph.md) ; relever `phInjectionLimitMinutes` par paliers, en tenant compte de l'excursion pendant le retard de mélange. [feature-085](../todo/feature-085-dose-de-base-apprise-feedforward.md) (dose de base apprise) reste la réponse côté correcteur |
