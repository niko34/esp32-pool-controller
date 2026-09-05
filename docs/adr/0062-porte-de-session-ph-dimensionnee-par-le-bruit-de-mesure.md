# ADR-0062 — La porte de session pH est dimensionnée par le bruit de mesure, pas par la précision souhaitée

- **Statut** : Accepté
- **Date** : 2026-09-05
- **Décideurs** : `pool-chemistry` (gate « GO sous 14 conditions » du 05/09/2026), `architect`, `code-reviewer`, utilisateur
- **Spec(s) liée(s)** : feature-094 (`specs/features/done/feature-094-decision-de-session-robuste-au-bruit.md`)

## Contexte

Près de la consigne, ce n'est plus l'état du bassin qui ouvre et ferme les sessions
de dosage pH : c'est le **bruit de la mesure**.

Les chiffres, mesurés sur la **filtrée** (la valeur que lit la régulation, médiane
13 × 5 s + EMA), enregistrements des 03 et 04/09/2026 :

| Grandeur mesurée | Valeur |
|---|---|
| Écart-type σ de la filtrée sur 60 s | **0,043 pH** |
| Crête-à-crête sur 15 min | **≈ 0,150 pH** |
| Bande d'hystérésis de décision | **0,05** (démarrage) / **0,01** (arrêt) |

Le bruit est donc **plus large que la bande de décision**. Conséquence observée :
des sessions de 6-10 min hachées par des pauses de mélange de 15 min
(`kPhMixingDelayMs`), un **rapport cyclique effectif tombé à ~1/3** près de la
consigne, et un feedforward (dose de base apprise, [ADR-0055](0055-feedforward-appris-additif-plafonne.md))
retenu puis réarmé toutes les 15 min — donc servi un tiers du temps alors qu'il
couvre une charge **continue**. La série du 03/09 ne montre d'ailleurs **aucune
relation dose-réponse** : 10 mL « font » −0,18 pH, 83 mL « font » −0,08 ; dans
40 m³, ce sont des fluctuations de mesure, pas de la chimie.

**La mesure qui débloque le dossier** est la caractérisation temporelle du bruit
faite par le gate : **τc ≈ 50 s, période dominante ≈ 5 min**. Ce n'est pas une
oscillation lente mais un processus **quasi blanc** — donc **moyenner marche, et
coûte peu de retard**. Une fenêtre de 10 min ramène σ à **≈ 0,017** pour **≈ 7 min**
de retard de décision, et laisse une ouverture sur échelon de +0,30 pH se produire
en **< 4 min**.

La question à trancher n'était donc pas « comment mesurer plus finement » mais
**« sur quel signal, et avec quelle largeur de bande, décide-t-on d'ouvrir une
session »**. Trois leviers étaient sur la table (L1 signal lent, L2 bande élargie,
L3 durée minimale de session).

## Décision

**La porte de session pH est dimensionnée par le bruit de mesure caractérisé, pas
par la précision de régulation souhaitée.** Concrètement :

1. **Une fonction pure unique** — `phSessionGate()` ([`src/ph_session_logic.h`](../../src/ph_session_logic.h))
   — produit **la porte ET la bande morte du PI** à partir du même appel. Les deux
   décisions deviennent indissociables, ce qui rend structurellement impossible la
   régression v2.26.0 (bande morte et hystérésis écrites à deux endroits, PI parqué
   exactement au point de coupure).
2. **L1 retenu** — la décision d'ouvrir/fermer lit un **signal lent** : moyenne
   glissante du premier ordre de constante de temps `kPhDecisionWindowMs` = **10 min**.
   La **régulation continue (P, I, feedforward) continue de lire la filtrée
   instantanée** : on ne ralentit pas la boucle, on stabilise sa porte.
3. **L3 retenu** — **durée minimale de session `kPhSessionMinWallMs` = 30 min
   MURALES**. En temps mural et non pompe-ON : au rapport cyclique mesuré près de
   la consigne (3,4-5,3 %), 30 min de pompe-ON dureraient ~10 h murales et la borne
   serait décorative.
4. **L2 refusé** (voir Alternatives).
5. **Le verrou de session change de sens** : `phDosingState.active` suit désormais
   le **verdict de porte**, et non plus « un débit a été commandé ». Sans ce
   changement, ni L1 ni L3 n'ont d'effet — la session se refermait au premier tour
   où le PI retombait dans sa bande morte.
6. **Condition d'acceptation du gate, non négociable : déplacement du point
   d'équilibre = 0,000 pH.** `phStartThreshold` / `phStopThreshold` gardent leur
   position **et** leur rôle (seuils d'hystérésis, bande morte du PI, cible de
   l'intégrande `erreur − phStopThreshold`). L'équilibre du bassin reste
   `consigne + phStopThreshold`.
7. **Trois garde-fous encadrent la prolongation** :
   - marge de fermeture `kPhDoorCloseMarginPh` = **0,03** (la porte ferme sous
     −0,02, l'ouverture reste inchangée à `> phStartThreshold`) ;
   - plancher de sous-dépassement `kPhSessionUndershootFloorPh` = **0,05**, qui
     **prime sur la durée minimale** (identique par `static_assert` au seuil du
     latch défavorable du feedforward `kPhFfAdverseErrorPh` : deux organes qui
     constatent le même événement physique ne doivent pas le constater à deux
     valeurs différentes) ;
   - **régime de débit** pendant la prolongation : total plafonné au feedforward
     seul dès que le signal lent passe sous le seuil d'arrêt, puis **débit nul**
     dès qu'il passe sous la consigne. Sans ce régime, le feedforward serait servi
     aveuglément pendant toute la prolongation — le seul vrai danger de L3.
8. **Les gardes de sécurité conservent la priorité absolue** : plancher acide,
   butées brutes, capteur instable, filtration absente, limite horaire, quota
   journalier, `maxCyclesPerDay`, anti-rafale, OTA, maintenance, watchdog ferment
   la session **dans le même tour**, arment la pause de mélange et journalisent,
   **quelle que soit la durée minimale**. Elles ne passent jamais par la porte.

Sous-dépassement pire cas calculé par le gate : **−0,058 pH → 7,242**, au-dessus
du plancher `kPhTargetMin` = 7,20.

## Alternatives considérées

- **L2 — élargir la bande d'hystérésis au-dessus du bruit (REJETÉE).** C'était la
  réponse « évidente » : si le bruit fait 0,043 σ, on ouvre plus haut et on ferme
  plus bas. Le chiffrage la tue : couvrir le bruit brut de façon robuste exigerait
  une bande de **0,17 pH**, c'est-à-dire **parquer le bassin à +0,09 au-dessus de
  la consigne et le laisser descendre à −0,08 en dessous**. Or l'installation ne
  dispose que d'**acide** : sous la consigne, il n'existe **aucun moyen de
  remonter** — la remontée passive plafonne à **≤ 0,02 pH/h**, soit ~4 h de
  récupération après une excursion basse. Un levier qui déplace l'équilibre dans
  la direction où l'actionneur est **absent** est refusé.
- **Garder le latch de session sur le débit commandé, et renoncer aux deux
  leviers (REJETÉE).** C'est l'alternative crédible, et la plus conservatrice :
  ne rien changer à la sémantique de `isPhDosing()` (donc ne rien changer à ce que
  voient l'UI, Home Assistant et l'historique). Mais tant que la session se referme
  au premier tour de bande morte, **L1 et L3 sont inopérants** : le signal lent
  déciderait d'ouvrir une session que le PI refermerait dans la minute, et la durée
  minimale ne s'appliquerait à rien. Le prix payé est un **changement de sens
  observable** (voir Conséquences négatives) ; il est assumé et documenté.
- **L4 — ne pas retenir le feedforward à la fermeture « erreur résorbée »
  (ÉCARTÉE de ce lot, pas rejetée).** Traite le symptôme feedforward
  indépendamment de la porte. Devenue **sans objet** ici : en restaurant le rapport
  cyclique, L1+L3 suppriment la cause (les fermetures elles-mêmes). À rouvrir
  seulement si l'observation terrain montre encore des rétentions fréquentes.
- **Un filtre à α fixe pour le signal lent (ÉCARTÉE).** La cadence d'appel (tour de
  loop) et la cadence de rafraîchissement de la sonde (5 s) diffèrent : à α fixe, la
  fenêtre effective dépendrait de la charge CPU. Le pas de temps est donc **fourni
  par l'appelant** et un `dt` aberrant (> 10 s) **ré-amorce** le filtre — sinon
  α ≈ 1 rendrait la « moyenne » égale au dernier échantillon avec `primed` encore
  vrai, c'est-à-dire exactement la décision sur bruit que le module supprime.
- **Traiter le bruit à la source (HORS PÉRIMÈTRE).** L'origine est électrique
  (courants de fuite de la pompe de filtration, bruit de mode commun) et relève de
  feature-069. La décision doit être conçue en supposant ce bruit **permanent**.

## Conséquences

### Positives

- Le rapport cyclique perdu (2/3) est restauré près de la consigne : sessions plus
  longues, pauses de mélange plus rares, **feedforward servi en continu** au lieu
  d'un tiers du temps.
- **Le droop observé se corrige.** Le bassin était parqué à **7,38-7,40**, pas à
  l'équilibre théorique 7,31 : la régulation n'était pas à son point d'équilibre,
  elle était **bridée par la perte des 2/3 du rapport cyclique**. Restaurer ce
  rapport le fait **descendre vers la consigne** — mouvement de **−0,07 pH au
  maximum**, dans le bon sens. C'est une **correction**, pas un déplacement.
- Un seul point de vérité pour la porte et la bande morte : la régression v2.26.0
  ne peut plus être réintroduite par divergence.
- Trois motifs d'arrêt de session distincts au journal là où « erreur résorbée »
  couvrait tout : un arrêt normal, un sous-dépassement franc et un ré-amorçage du
  signal ne se lisent plus de la même façon.
- Sur la configuration vivante (`ph_limit_minutes` = 10 min/h, pompe 26 mL/min ⇒
  soutenable **4,33 mL/min**, plafond feedforward **2,60 mL/min**, charge mesurée
  **1,35 mL/min**), **l'appareil n'est pas limité par son actionneur** : le lot ne
  consomme pas une marge qui manquerait ailleurs.

### Négatives / dette assumée

- **`isPhDosing()` est désormais vrai pompe à l'arrêt** pendant une prolongation.
  `/data ph_dosing`, le champ WebSocket `ph_dosing`, le topic MQTT
  `{base}/ph_dosing`, le champ `ph_dosing_active` du JSON de statut et
  `history.phDosing` **restent ON**. **La sémantique vue par l'UI et par Home
  Assistant change** : ce n'est plus « la pompe tourne » mais **« une session est
  ouverte »**. Aucun champ n'est ajouté, aucun renommé. Les clients qui veulent
  « la pompe tourne maintenant » disposent déjà du **calendrier d'impulsion**
  ([ADR-0045](0045-etat-impulsion-expose-en-calendrier.md) : pompe alimentée ⇔
  `phase < pulse_ms`).
- **Retard de décision ≈ 7 min** sur la fermeture comme sur l'ouverture. Borné, et
  très en deçà de la pause de mélange de 15 min déjà tolérée par conception.
- **Le signal lent ne voit pas une dérive plus lente que sa fenêtre** (dérive lente
  des courants de fuite). L2, qui aurait couvert ce cas, a été refusé ; le plancher
  de sous-dépassement et les gardes restent les seuls filets.
- Le warning « Limite cycles atteinte » devient possible **session ouverte** : les
  effets de bord journaliers de la porte de démarrage (rollover 24 h de
  `cyclesToday`, warning throttlé) ont été extraits pour rester atteignables par le
  pH, dont la porte n'est plus `shouldStartDosing`.
- **La preuve terrain reste due.** Le rejeu des séries CSV en test natif est en
  **boucle ouverte** : il démontre l'anti-chatter, **pas** que le bassin reste dosé.
  Voir « Limites connues » ci-dessous.

### Ce que ça verrouille

- **Le point d'équilibre du bassin.** Toute évolution ultérieure de la porte doit
  reprouver un déplacement de **0,000 pH**, ou repasser un gate `pool-chemistry`.
- **La largeur de la bande est liée au bruit résiduel du signal lent**, par
  `static_assert` : `phStartThreshold − phStopThreshold ≥ 0,04` (2σ du signal
  moyenné). Quiconque resserre l'hystérésis casse la compilation.
- **`kPhStartThresholdDefault` et `kPhStopThreshold` sont promues `constexpr`** dans
  [`constants.h`](../../src/constants.h) — vérification faite : elles n'étaient
  jamais relues depuis la NVS ni exposées par `/get-config` ou `/save-config`, donc
  de facto des constantes de compilation. **Si un jour elles devenaient réglables,
  ces `static_assert` cesseraient de faire foi** et il faudrait leur substituer un
  bornage à l'enregistrement (piège documenté « config d'usine ≠ config vivante »).
- **Durée minimale et fenêtre de décision sont couplées** : `kPhSessionMinWallMs ≥
  2 × kPhDecisionWindowMs` (une session doit durer au moins deux fenêtres, sinon
  elle se refermerait avant que le signal lent ait constaté l'effet du dosage), et
  `kPhSessionMinWallMs + kPhMixingDelayMs ≤ kPhSessionMaxWallMs`.
- **Le plancher de sous-dépassement est identique au seuil défavorable du
  feedforward** (`static_assert` d'identité) : les deux ne peuvent plus diverger.
- **Le détecteur de corruption du tas ne dépend plus de l'état de session** :
  `isPhDosing()` / `isOrpDosing()` ont été **retirés** de la garde de
  `oomGuardIntegrityTick()` — ils seraient restés vrais jusqu'à 3 h
  (`kPhSessionMaxWallMs`). Amendement porté à
  [ADR-0037](0037-postmortem-oom-et-integrite-tas.md).

## Limites connues

- **Le critère « feedforward servi ≥ 80 % du temps » n'est pas prouvable hors
  appareil.** Au rejeu de la série réelle, il ressort à **0,0 %** — non par défaut
  du lot, mais parce que le **signal lent y siège à la consigne** (moyenne 7,2999) :
  l'acide des 7 sessions réellement injectées ce jour-là est **déjà dans la série**.
  Un rejeu en boucle ouverte ne peut pas répondre à cette question. Mesure due sur
  l'appareil : enregistrement `/data` sur **4 h** une après-midi à la consigne.
- **Le temps de descente du matin** (erreur initiale ≥ 0,2) est également à
  reconfirmer sur l'appareil ; le bruit y est négligeable devant l'erreur et le lot
  n'est pas censé le changer.
- La dérive lente évoquée plus haut n'est couverte par aucun des deux leviers
  retenus.

## Références

- Code : [`src/ph_session_logic.h`](../../src/ph_session_logic.h) (module pur —
  `PhDecisionFilter`, `phDecisionTick()`, `phSessionGate()`),
  [`src/pump_controller.cpp`](../../src/pump_controller.cpp) (câblage, verrou de
  session, motifs d'arrêt, journal), [`src/constants.h`](../../src/constants.h)
  (constantes + `static_assert`), [`src/main.cpp`](../../src/main.cpp) (garde
  d'intégrité du tas)
- Tests : `test/test_native_ph_session/`
- Spec : [`specs/features/done/feature-094-decision-de-session-robuste-au-bruit.md`](../../specs/features/done/feature-094-decision-de-session-robuste-au-bruit.md)
- Doc : [pump-controller.md](../subsystems/pump-controller.md#porte-de-session-ph-robuste-au-bruit-feature-094-v2360),
  [regulation-vue-d-ensemble.md](../subsystems/regulation-vue-d-ensemble.md)
- ADR liés : [ADR-0016](0016-regulation-p-temporisee-vs-pid.md) (boucle temporisée),
  [ADR-0045](0045-etat-impulsion-expose-en-calendrier.md) (commandé vs effectif),
  [ADR-0052](0052-terme-integral-ph-dimensionne-par-autorite.md) (intégrale),
  [ADR-0055](0055-feedforward-appris-additif-plafonne.md) (feedforward, latch
  défavorable), [ADR-0037](0037-postmortem-oom-et-integrite-tas.md) (amendé)
