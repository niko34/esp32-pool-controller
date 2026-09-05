## Métadonnées
- **ID** : feature-094
- **Slug** : decision-de-session-robuste-au-bruit
- **Type** : feature
- **Statut** : **livré non déployé** (v2.36.0 — code, tests et docs terminés ; déploiement groupé à venir avec 096, 095 et bug-089)
- **Priorité** : normale (inoffensif chimiquement ; coûte de la lisibilité et un peu d'efficacité près de la consigne)
- **Version cible** : à fixer au cycle — **pas avant la fin de la fenêtre d'observation 84/85/86 (~06/09/2026)** ; **intégrer les données de l'enregistrement du 04/09 matin** (scratchpad → à archiver dans `builds/`) avant le dimensionnement
- **Dernière mise à jour** : 2026-09-03
- **Spec(s) liée(s)** : feature-025/058 (chaîne de filtrage médiane + EMA), feature-071/072 (dispersion exposée à l'UI), feature-084/085 (PI + feedforward), feature-069 (investigation matérielle sonde pH — traitement à la racine du bruit)
- **Origine** : enregistrement `/data` toutes les 60 s le 03/09/2026 11:58-20:49, archivé avec son script d'analyse dans `builds/2.33.0/esp32dev_core3/ph_filtered_20260903_1158-2049.csv` + `ph_analyse.py` (dossier gitignoré) ; analyse des paires pause/session

## Domaines touchés
- [x] Capteurs (pH) — éventuel signal de décision dérivé de la filtrée
- [x] Régulation (pump_controller — ouverture/fermeture de session pH) → **passage obligatoire par `pool-chemistry`**
- [ ] Filtration / Éclairage
- [ ] Web — routes HTTP / WebSocket (a priori aucun nouveau champ ; si le signal de décision est exposé dans `/data`, le dire)
- [ ] MQTT / Home Assistant
- [ ] UART / écran LVGL
- [ ] OTA
- [ ] Authentification
- [ ] Historique / persistance
- [ ] Frontend (`data/`)

## Agents requis
- [x] spec-reader
- [x] architect
- [x] pool-chemistry — la position et la largeur de la bande d'hystérésis fixent le point d'équilibre réel du bassin
- [ ] ux-ui-designer
- [x] embedded-developer
- [ ] web-ui-developer
- [x] test-engineer — logique pure (`shouldStartDosingPure` / `shouldContinueDosingPure`) déjà en natif : étendre, valider par mutation
- [x] code-reviewer
- [x] doc-writer

## Description
Près de la consigne, l'ouverture et la fermeture des sessions de dosage pH doivent être décidées par l'état du bassin, pas par les fluctuations de la mesure : le régulateur ne doit plus enchaîner des sessions de 6-10 min coupées par des pauses de 15 min au rythme du bruit.

## Contexte

### Le constat (03/09/2026, après-midi, régulation à la consigne 7,30)

Enregistrement de la **filtrée** (celle que lit la régulation) toutes les 60 s, découpé en épisodes session (●) / pause (○) :

| Pause sans injection | Δ filtrée | Session suivante | volume | Δ filtrée |
|---|---|---|---|---|
| 12:16 → 12:30 | −0,078 | 33 min | 83 mL | −0,08 |
| 12:53 → 13:07 | +0,093 | 27 min | 70 mL | −0,06 |
| 13:35 → 13:49 | +0,113 | ~58 min | 56 mL | −0,01 |
| 14:48 → 15:03 | +0,079 | 8 min | 14 mL | −0,05 |
| 15:13 → 15:27 | +0,052 | 6 min | 10 mL | −0,04 |
| 15:35 → 16:24 | −0,036 | 9 min | 10 mL | **−0,13** |
| 16:35 → 16:49 | +0,041 | 8 min | 10 mL | −0,03 |
| 16:59 → 17:41 | −0,009 | 7 min | 10 mL | **−0,18** |
| 17:49 → 18:03 | +0,068 | 9 min | 12 mL | −0,05 |
| 18:14 → 18:41 | +0,028 | 10 min | 8 mL | −0,04 |

Deux faits :
1. **Aucune relation dose-réponse** : 10 mL « font » −0,13 et −0,18, 83 mL « font » −0,08. Dans 40 m³, 10 mL d'acide ne déplacent pas le pH de 0,18 en 8 min. Ce sont des fluctuations de mesure.
2. **La filtrée fluctue de ±0,08 à l'échelle de 10-15 min**, en l'absence de toute injection (pauses : −0,08 à +0,11). Or la bande de décision est de **0,05** au démarrage (`phStartThreshold`) et **0,01** à l'arrêt (`phStopThreshold`) — défauts d'usine `config.h` ~228, non exposés dans `/get-config` : **vérifier les valeurs vivantes sur l'appareil avant tout chiffrage**.

Le bruit est donc **plus large que la bande d'hystérésis**. Près de la consigne, c'est lui qui ouvre et ferme les sessions.

### Ce que ce n'est PAS (hypothèses écartées le 03/09, ne pas les rouvrir)
- Pas un « panache » d'injection vu par la sonde : les sondes sont montées **en amont** des points d'injection dans le sens de circulation (information utilisateur) ; l'acide ne revient à la sonde qu'après un tour complet du bassin.
- Pas une corrélation avec la phase de la pompe doseuse (2 cycles observés, non reproduite).
- Origine du bruit : courants de fuite de la **pompe de filtration** + bruit de mode commun (dispersion du brut ~0,36, était 0,6-0,7 avant changement d'alimentation). **Traitement à la racine = électrique, hors de cette spec** ; il faut concevoir la décision en supposant ce bruit permanent.

### Pourquoi c'est un problème (mesuré)
- Chaque fermeture de session arme une **pause de mélange de 15 min** (`kPhMixingDelayMs`) pendant laquelle le dosage est interdit. Une fermeture déclenchée par un creux de bruit alors que le bassin est réellement à 7,38 coûte 15 min de dosage ; enchaînées, ces pauses ramènent le rapport cyclique effectif près de la consigne à ~1/3.
- Le feedforward est **retenu à chaque fermeture** et réarmé à l'ouverture (`ph_ff_status` 0 ↔ 1 toutes les 15 min) : la dose de base, censée couvrir la charge en continu, n'est servie qu'un tiers du temps près de la consigne.
- Lisibilité : le journal et la carte pH montrent une régulation qui « hésite » ; l'utilisateur en a déduit à tort qu'elle ne fonctionnait pas.
- **Ce que ce n'est pas** : un risque chimique. Les volumes sont faibles (8-14 mL par session), toutes les gardes s'appliquent.

## Comportement attendu

1. **Une décision de session repose sur un signal qui ne fluctue pas plus que la bande d'hystérésis.** Soit la bande est élargie au-dessus du bruit, soit la décision s'appuie sur une version plus lente de la filtrée, soit les sessions/pauses ont une durée minimale qui absorbe le bruit — ou une combinaison ; **pool-chemistry choisit** (voir Pistes).
2. **La régulation continue (P, I, FF) lit toujours la filtrée actuelle** : le signal de décision, s'il existe, ne sert qu'à ouvrir/fermer les sessions. On ne ralentit pas la boucle, on stabilise sa porte.
3. **Le point d'équilibre reste la consigne** : la solution ne doit pas déplacer la valeur autour de laquelle le bassin se stabilise (aujourd'hui `consigne + phStopThreshold`, cf. `constants.h` ~1935) de plus que ce que pool-chemistry accepte explicitement.
4. **Les gardes de sécurité gardent la priorité absolue** : plancher, butées brutes, capteur instable, limites horaires/journalières, absence de filtration ferment une session immédiatement, quelle que soit la durée minimale.
5. Résultat observable : près de la consigne, des sessions plus longues et moins nombreuses, la FF servie de façon continue, et une filtrée qui tient la consigne dans une bande ≤ ±0,05.

### Cas limites
- Fluctuation du bruit plus lente que le filtre de décision (dérive lente des courants de fuite) : aucun filtre ne l'élimine — d'où l'intérêt d'une bande élargie **en plus**.
- Vrai changement rapide du bassin (ajout manuel de produit, remplissage) : le signal de décision doit suivre en < 15 min ; une durée minimale de session ne doit pas maintenir un dosage sur un bassin déjà revenu à la consigne au-delà de ce que la pause de mélange tolère déjà (~0,09 pH d'excursion prévue par conception, `constants.h` ~998).
- Matin, erreur 0,2-0,3 : le bruit est négligeable devant l'erreur, la solution ne doit rien changer à la phase de rattrapage (critère : temps de descente 04/09 inchangé ± bruit).
- Reprise de filtration / fin de purge : le signal de décision doit être ré-amorcé avec les filtres (pas de décision sur une moyenne contenant des valeurs de stagnation).
- Vitesses `slow` / `fast` : les seuils ou durées, s'ils dépendent de la vitesse, doivent être cohérents avec ki et Ti (static_assert).

## Comportement actuel
- Décision d'ouverture : `shouldStartDosing(error, phStartThreshold, …)` sur l'erreur **filtrée** (médiane 13 × 5 s ≈ 65 s + EMA α 0,10) — `pump_controller.cpp` ~328, ~3024.
- Décision de poursuite : `shouldContinueDosing(error, phStopThreshold, sessionOnMs, …)` ; la session ferme aussi sur `kPhSessionMaxOnMs` (60 min de pompe) et `kPhSessionMaxWallMs` (3 h).
- Chaque fermeture arme `kPhMixingDelayMs` = 15 min ; le FF est retenu jusqu'à la prochaine ouverture.
- Aucune durée minimale de session ; aucun signal de décision distinct de la filtrée.

## Contraintes techniques
- ESP32, offline first ; logique pure ADR-0017 (les décisions dans un `.h` natif, temps et mesures en paramètres)
- Pas de nouvelle allocation dynamique dans la boucle (un filtre de décision = quelques flottants d'état)
- Toute constante dans `constants.h`, avec `static_assert` liant seuils/durées aux constantes de filtre et de pause existantes
- Si le signal de décision est exposé : `/data` **et** WebSocket (parité, piège récurrent documenté)

## Pistes pour pool-chemistry et l'architecte (non prescriptives)

- **L1 — Signal de décision lent** : moyenne glissante (ou EMA) de la filtrée sur 10-15 min, utilisée uniquement pour ouvrir/fermer. Efficace contre une fluctuation de période < fenêtre ; inefficace contre une dérive lente. La période du bruit est **à mesurer sur l'enregistrement du 04/09** avant de fixer la fenêtre.
- **L2 — Bande d'hystérésis dimensionnée par le bruit** : démarrage à `consigne + max(0,05 ; k × bruit)` et arrêt sous la consigne d'une marge acceptée par pool-chemistry, où « bruit » est la dispersion déjà calculée pour l'UI (feature-071/072) ou une constante. Déplace le point d'équilibre : décision chimique explicite.
- **L3 — Durée minimale de session et de pause** : une session ouverte dure au moins N min (≥ une pause de mélange ?) sauf garde ; une pause dure au moins 15 min (déjà le cas). Simple, testable, mais tolère une sur-injection bornée (N × débit soutenable) : à chiffrer.
- **L4 — Ne pas retenir le FF à la fermeture de session** quand la fermeture est due à « erreur résorbée » près de la consigne (la charge n'a pas disparu) : traite le symptôme FF indépendamment des autres leviers. Interaction avec feature-085 (règles de rétention du FF) à vérifier.
- Recommandation de départ : L1 + L3 modéré, L2 seulement si pool-chemistry accepte le déplacement d'équilibre, L4 à étudier séparément.

## Fichiers potentiellement concernés
- `src/ph_integral_logic.h` ou nouveau `src/ph_session_logic.h` — décisions pures (signal de décision, durées minimales)
- `src/pump_controller.cpp` — câblage des décisions d'ouverture/fermeture (~3020-3030), rétention FF (L4)
- `src/constants.h` — fenêtre du signal de décision, durées minimales, bande ; `static_assert`
- `src/sensors.*` — si le signal de décision est produit côté capteur (à côté de la filtrée) plutôt que côté régulation
- `src/web_routes_data.cpp` + `src/ws_manager.cpp` — si exposé (parité)
- `test/test_native_*` — nouveaux cas rejoués sur des séries synthétiques ET sur la série réelle du 03/09 (à convertir en fixture)
- `docs/subsystems/pump-controller.md`, `docs/user/src/guide-regulation.html` (comportement de régulation visible), `CHANGELOG.md`

## Livrables — check-list
- [x] A1 — Décision d'ouverture/fermeture de session insensible à une fluctuation de ±0,08 sur 10-15 min — **L1 + L3 retenus** par `pool-chemistry` (signal lent 10 min + durée minimale 30 min murales), L2 refusé
- [x] A2 — Gardes de sécurité : priorité absolue conservée, prouvée par test (`minSessionMs` n'est consommé que dans la branche hystérésis ; toute garde ferme dans le même tour)
- [x] A3 — **Sans objet : L4 n'a pas été retenu.** En restaurant le rapport cyclique, L1+L3 suppriment la **cause** (les fermetures elles-mêmes) au lieu du symptôme. À rouvrir seulement si l'observation terrain montre encore des rétentions fréquentes de FF
- [x] A4 — Constantes dans `constants.h` avec **6 `static_assert`** de cohérence (bruit résiduel du signal lent, plafond mural, deux fenêtres de décision, identité avec `kPhFfAdverseErrorPh`, marge de fermeture, parcours d'intégrité du tas)
- [x] D1 — Tests natifs : série réelle rejouée (anti-chatter démontré), série synthétique « changement rapide » → ouverture en **< 4 min**, gardes, **8 mutations tuées sur 8**. ⚠️ Rejeu en **boucle ouverte** — voir Clôture, partie 2
- [x] C1 — `docs/subsystems/pump-controller.md`, guide régulation utilisateur + PDF v2.36, `CHANGELOG.md` (+ ADR-0062, amendement ADR-0037, API/MQTT/WS/page-ph, system-health, oom-guard, vue d'ensemble)

## Sécurité chimique
- Dosage impliqué : oui — pH- (acide)
- Limites journalières / horaires concernées : inchangées ; une durée minimale de session (L3) doit rester sous toutes les limites (elle ne les contourne pas, elle s'y soumet)
- Filtration requise avant action : oui, inchangé
- Calibration capteur modifiée : non
- Algorithme PID touché : non pour P/I/FF ; **oui pour la porte de session** (hystérésis)
- Risque identifié : (L2) déplacement du point d'équilibre ; (L3) sur-injection bornée si le bassin atteint la consigne pendant la durée minimale — à chiffrer en pH pour 30-50 m³ au débit soutenable ; (L1) retard de fermeture sur un vrai changement rapide. **pool-chemistry tranche le levier et les valeurs.**

## ADR attendu ?
- **Probable** : oui
- **Critère(s) candidat(s)** : 2, 3 (choix du levier parmi L1-L4, refus éventuel de L2)
- **Sujet pressenti** : « la porte de session pH est dimensionnée par le bruit de mesure, pas par la précision souhaitée »

## Éléments visibles (pour doc-writer)

### Endpoints HTTP concernés
- `/data` — si signal de décision exposé (sinon « aucun »)

### Champs WebSocket
- parité avec `/data` si champ ajouté

### Topics MQTT
- aucun

### Documentation à produire / mettre à jour
#### Docs vivantes
- `docs/subsystems/pump-controller.md` — porte de session, nouvelles constantes
#### Docs transverses
- `CHANGELOG.md` — « près de la consigne, la régulation pH n'hésite plus »
- `docs/user/src/guide-regulation.html` → `./tools/build_user_pdfs.sh` (comportement visible : sessions plus longues, FF continue)
#### Docs immuables
- ADR (cf. ci-dessus)

## Critères d'acceptation
- [x] Sur la série réelle du 03/09 15:00-19:00 rejouée en test natif, le nombre de sessions est divisé par au moins 2 et la filtrée reste dans [consigne − 0,05 ; consigne + 0,05] hors bruit
- [x] Sur une série synthétique où le bassin passe de la consigne à consigne + 0,3 en 5 min, une session s'ouvre en moins de 15 min — **mesuré : < 4 min**
- [x] Toute garde (plancher, butée brute, capteur instable, limite horaire/journalière, filtration OFF) ferme une session ouverte depuis moins que la durée minimale
- [ ] Le temps de descente du matin (erreur initiale ≥ 0,2) est inchangé à ± 10 % par rapport à la référence du 04/09 — **non prouvable hors appareil** (cf. Clôture, partie 2)
- [ ] Le FF (`ph_ff_status`) reste servi pendant au moins 80 % du temps de filtration une fois la consigne atteinte (contre ~1/3 le 03/09) — **0,0 % au rejeu, pour une raison de méthode, pas de code** (cf. Clôture, partie 2)
- [x] Tous les libellés utilisateur sont en français
- [x] `pio run` sans nouveau warning ; `pio test -e native` zéro régression ; mutations tuées (**8/8**)
- [ ] Comportement nominal testé sur l'appareil selon le plan ci-dessous — **livré non déployé** (déploiement groupé avec 096, 095 et bug-089)

## Plan de test manuel
1. Après-midi à la consigne : enregistrer `/data` toutes les 60 s pendant 4 h (script `record_ph_filtered.sh` archivé) ; comparer le nombre de sessions et la tenue de la filtrée au 03/09.
2. Matin : même enregistrement de la reprise de filtration à la consigne ; comparer le temps de descente au 04/09.
3. Provoquer une garde pendant une session (couper la filtration) : fermeture immédiate, journal explicite.

## Notes pour agent
- **Ne rien déployer avant la fin de la fenêtre d'observation 84/85/86 (~06/09/2026).**
- **Intégrer l'enregistrement du 04/09 matin** (période et amplitude du bruit à 7,5 vs 7,3, effet de la reprise de filtration) avant de chiffrer L1/L2.
- Lire la config vivante (`/get-config`, et les seuils `phStartThreshold`/`phStopThreshold` réels via le canal qui les expose) avant tout chiffrage — piège documenté « config d'usine ≠ config vivante ».
- Ne pas rouvrir les hypothèses « panache » et « phase pompe doseuse » (écartées, cf. Contexte).
- L'offset pH (+0,5) n'est pas une suspicion de sonde ; il ne fait pas partie du problème.
- Toute modification de la chaîne de filtrage elle-même (médiane, EMA) est **hors périmètre** : elle sert P/I/FF et a été validée par pool-chemistry (feature-058).

## Hors périmètre
- Réduction du bruit à la source (alimentation, câblage, mode commun) — matériel, feature-069
- Modification du filtre de mesure (médiane/EMA) servant à la régulation continue
- Gains kp/ki, vitesse de régulation
- Historisation de la filtrée (spec séparée possible, `specs/modele.md`)

---

## Notes d'implémentation (v2.36.0, 2026-09-05)

### Le levier retenu et ses valeurs (gate `pool-chemistry`)

**Caractérisation décisive** : le bruit de la filtrée est **τc ≈ 50 s, période dominante ≈ 5 min** — un processus **quasi blanc**, pas une oscillation lente. C'est cette mesure qui débloque le dossier : moyenner marche et coûte peu de retard. σ = 0,043 sur 60 s ; crête-à-crête 15 min ≈ 0,150.

| Paramètre | Valeur retenue | Justification |
|---|---|---|
| Fenêtre du signal lent (`kPhDecisionWindowMs`) | **10 min** | σ ramené à ≈ 0,017 ; retard ≈ 7 min ; ouverture sur échelon +0,30 en **< 4 min** |
| Marge de fermeture de porte (`kPhDoorCloseMarginPh`) | **0,03** | La porte ferme sous −0,02 ; **n'affecte pas** la bande d'actionnement |
| Durée minimale de session (`kPhSessionMinWallMs`) | **30 min murales** | Au rapport cyclique mesuré (3,4-5,3 %), 30 min de pompe-ON dureraient ~10 h murales — la borne serait décorative |
| Plancher de sous-dépassement (`kPhSessionUndershootFloorPh`) | **0,05** | Prime sur la durée minimale ; identique par `static_assert` à `kPhFfAdverseErrorPh` |
| Plafond de débit en prolongation | **feedforward seul** (2,60 mL/min sur la config vivante), puis **nul** sous la consigne | Sans lui, le FF serait servi aveuglément — seul vrai danger de L3 |
| **Déplacement du point d'équilibre** | **0,000 pH** | Condition d'acceptation non négociable du gate |

Sous-dépassement pire cas calculé : **−0,058 pH → 7,242**, au-dessus du plancher `kPhTargetMin` = 7,20.

**L2 (élargir la bande) REFUSÉ** : couvrir le bruit brut exigerait **0,17 pH**, soit parquer le bassin à +0,09 et le laisser descendre à −0,08 **sans moyen de remonter** (acide seul ; remontée passive ≤ 0,02 pH/h, ~4 h de récupération).

**Le droop expliqué** : le bassin était parqué à **7,38-7,40**, pas à l'équilibre théorique 7,31 — la régulation n'était pas à son équilibre, elle était **bridée par la perte des 2/3 du rapport cyclique**. Le restaurer la fait **descendre vers la consigne** : **−0,07 pH au maximum, dans le bon sens**. C'est une **correction**, pas un déplacement.

**Config vivante utilisée** (lue avant tout chiffrage, piège « config d'usine ≠ config vivante ») : `ph_limit_minutes` = **10 min/h**, pompe **26 mL/min** ⇒ soutenable **4,33 mL/min**, plafond FF **2,60 mL/min** ; charge mesurée **1,35 mL/min** ⇒ **l'appareil n'est pas limité par son actionneur** dans cette configuration.

### Verdicts

- `pool-chemistry` : **GO sous 14 conditions** (C1 priorité des gardes, C2 signal non prêt ⇒ fail-closed, C3 bande morte inchangée, C4 marge de fermeture, C5 durée minimale murale, C6 plancher de sous-dépassement prioritaire, C7 régime de débit en prolongation, C12 verrous de cohérence…).
- `code-reviewer` : **Request changes** puis correctifs, enfin **Approved** en re-revue. Correctifs portés : cycle comptabilisé au **premier débit réel** (témoin `_phCycleCounted`), `erreur_lente=` ajoutée à la ligne de démarrage, `isPhDosing()`/`isOrpDosing()` **retirés** de la garde d'intégrité du tas, `static_assert` de marge ramené aux **µs des deux côtés**.
- **Empreinte flash** : **1 811 321 / 1 835 008 o**, marge **23 687 o**.
- **Tests** : **30 suites / 1 154 cas**, **8 mutations sur 8 tuées**.

### Décision structurante consignée

[ADR-0062](../../../docs/adr/0062-porte-de-session-ph-dimensionnee-par-le-bruit-de-mesure.md) — « La porte de session pH est dimensionnée par le bruit de mesure, pas par la précision souhaitée ». Amendement porté à [ADR-0037](../../../docs/adr/0037-postmortem-oom-et-integrite-tas.md) (garde d'intégrité du tas fondée sur l'énergisation réelle, plus sur l'état de session).

### Correctif à porter (hors périmètre de ce lot)

Le gate a **réfuté un chiffre gravé dans le code** : le commentaire de `src/constants.h` (~ligne 2126) annonce « ~0,09 pH d'excursion pendant la pause de mélange de 15 min ». Les données donnent **0,008 pH** — la valeur 0,09 dérivait d'une charge de 2,86 mL/min alors que la charge réelle mesurée est de **1,35 mL/min**. **Ne plus utiliser 0,09 comme budget d'excursion** ; commentaire à corriger dans un lot ultérieur.

### Spec de suivi à créer (signalée, non créée)

Le **rollover journalier** est appelé **sous** `canDose()`, lui-même fermé par `cyclesToday >= maxCyclesPerDay` : à la butée, le blocage reste **définitif**. Défaut **pré-existant, identique côté ORP**, hors périmètre de ce lot. Le correctif (hisser le tick hors conjonction, comme le tick intégral) exige un passage `pool-chemistry` — ce n'est pas un refactor anodin.

---

## Clôture en deux temps

### 1. Ce qui est prouvé au code (clos)

- La porte et la bande morte sortent d'un **appel unique** — la divergence à l'origine de la régression v2.26.0 est structurellement impossible.
- **Anti-chatter démontré** sur la série réelle rejouée : nombre de sessions divisé par ≥ 2, pas de dépassement de consigne > 0,05.
- **Réactivité** à un vrai changement du bassin : ouverture en **< 4 min** sur échelon de +0,30 (exigence : < 15 min).
- **Priorité absolue des gardes** : `minSessionMs` n'est jamais consulté face à une fermeture de garde.
- **Cohérence des constantes** verrouillée à la compilation (6 `static_assert`).
- **8 mutations sur 8 tuées** ; `pio run` sans nouveau warning ; zéro régression native.

### 2. Ce qui n'est PAS prouvable hors appareil (ouvert — réserve principale du lot)

Le rejeu des CSV est en **boucle ouverte**. Il démontre que la porte cesse de hacher les sessions ; il **ne démontre pas que le bassin reste dosé**, parce qu'un rejeu ne referme pas la boucle : la série contient déjà l'effet de l'acide réellement injecté ce jour-là.

- **Critère « feedforward servi ≥ 80 % » : 0,0 % au rejeu.** Ce n'est **pas** un défaut du lot : le **signal lent y siège à la consigne** (moyenne **7,2999**) parce que **l'acide des 7 sessions réelles est déjà dans la série**. La porte n'a donc aucune raison de s'ouvrir, et le FF n'a rien à servir. Le critère est **inévaluable par rejeu** ; il ne peut être tranché que sur l'appareil.
- **Temps de descente du matin** : idem, à reconfirmer sur l'appareil.
- **Mesure due** : enregistrement `/data` toutes les 60 s pendant **4 h**, une après-midi à la consigne (script `record_ph_filtered.sh`), puis comparaison du nombre de sessions, de la tenue de la filtrée et du taux de service du FF au 03/09.

Ces deux points restent **ouverts** après la clôture de la spec, et sont à trancher lors du déploiement groupé (avec 096, 095 et bug-089).
