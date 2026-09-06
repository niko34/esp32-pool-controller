---
name: mesure-ph-perturbee-en-phase-pompe
description: "Bruit de mesure pH (EZO) : dispersion ~0,36 pH sur la fenêtre médiane, due aux courants de fuite de la POMPE DE FILTRATION et au bruit de mode commun (pas la pompe doseuse) ; réduite de 0,6-0,7 → 0,35 par une nouvelle alimentation (2026). Le brut oscille 6,75-7,15 ; la régulation lit la FILTRÉE (médiane 13 × 5 s + EMA 0,10), l'historique le BRUT+offset."
metadata: 
  node_type: memory
  type: project
  originSessionId: 82048c5b-7e8f-4cec-8242-ad9b5ed01f91
  modified: 2026-09-03T19:19:58.601Z
---

Constat du 03/09/2026 (relevé `/data` toutes les 3 s pendant 2 min, dosage pH en
cours) : `phRaw` saute entre 6,75 et 7,15 d'un échantillon à l'autre ; le
`phMedian` bouge de 7,38 à 7,54 ; le `ph` filtré dérive lentement (7,43 → 7,51).
L'historique (`history.cpp` : `point.ph = sensors.getPh()` = brut + offset,
instantané, 1 point / 5 min) montre donc des dents de scie 7,3 / 7,6 pendant les
sessions, alors que le régulateur voit une valeur lisse.

**Origine (donnée par l'utilisateur, qui a instrumenté le sujet)** : courants de
fuite de la **pompe de filtration** + bruit de mode commun sur la chaîne EZO.
Dispersion passée de 0,6-0,7 à ~0,35 grâce à une nouvelle alimentation ;
amélioration encore prévue, mais il faut considérer ce bruit comme permanent.
J'avais d'abord cru à une corrélation avec la phase de la pompe DOSEUSE — sur
2 cycles seulement, non reproduite au 2e cycle, hypothèse de fenêtre ON non
vérifiée : **retirée**. Ne pas la ressortir sans une acquisition longue
(≥ 10 cycles) qui la prouve.

**Hypothèse « panache d'injection » également ENTERRÉE (03/09 soir)** : les sondes
sont montées EN AMONT des points d'injection dans le sens de circulation
(information utilisateur) ; l'acide injecté ne revient à la sonde qu'après un
tour complet du bassin. Les sessions courtes près de la cible s'expliquent par
le bruit : la filtrée fluctue de ±0,08 à l'échelle de 10-15 min (enregistreur
`/data` 60 s, après-midi du 03/09 : pauses sans injection de −0,08 à +0,11,
aucune relation dose-réponse — 10 mL font −0,18, 83 mL font −0,08), soit PLUS
que le seuil d'hystérésis de 0,05. Les démarrages/arrêts de session près de la
consigne sont donc pilotés par le bruit (inoffensif : sessions 6-10 min, 8-14 mL).
Leviers éventuels (pool-chemistry, pas pendant une fenêtre d'observation) :
filtre de décision plus long, hystérésis élargie, durée minimale de session.

Faits de code vérifiés le 03/09 : P, intégrale, feedforward, hystérésis, canDose
et gardes chimiques lisent TOUS `getPhFiltered()` (une seule erreur signée par
tour, `pump_controller.cpp` ~2966). Seuls l'historique et la trace de
diagnostic lisent le brut + offset.

**Why :** l'offset +0,5 corrige le décalage moyen dû à ces fuites (cf.
[[offset-ph-nature-et-non-suspicion]]) ; la question ouverte n'est pas « quelle
valeur lit la régulation » mais « le bruit est-il symétrique » : s'il est
unilatéral, la médiane est biaisée et l'offset (calibré sous un niveau de bruit
donné) doit être re-vérifié après chaque changement électrique.

**How to apply :** devant une régulation « plus lente qu'attendu », vérifier
dans l'ordre : demande vs soutenable (`/data`), gains (`regulation_speed`),
puis validité de l'offset par mesure de référence — pas le chemin de mesure,
déjà propre. Toute modification du filtre = chemin régulation → pool-chemistry.
