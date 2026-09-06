---
name: matin-0509-air-dans-le-circuit
description: "Le matin du 05/09/2026 est CONTAMINÉ par de l'air dans la filtration (niveau d'eau bas) — condition nouvelle, ne pas mélanger aux matins 02-04/09"
metadata: 
  node_type: memory
  type: project
  originSessionId: 82048c5b-7e8f-4cec-8242-ad9b5ed01f91
  modified: 2026-09-05T06:23:14.618Z
---

**Le 05/09/2026 au matin, le niveau d'eau du bassin était trop bas** : de l'air
entrait par les skimmers, on l'entendait ressortir aux buses de refoulement.
L'utilisateur a rempli le bassin dans la matinée.

**Condition NOUVELLE, pas permanente** (confirmé par l'utilisateur) : la piscine
tournait le 04/09 après-midi **sans air aux buses**. Le niveau a donc franchi le
seuil entre le 04/09 après-midi et le 05/09 matin.

**Conséquences pour l'analyse :**

1. **Les données du matin 05/09 sont contaminées** et ne doivent PAS être
   agrégées avec celles des 02, 03 et 04/09. Cela vaut pour : le fond du V à
   **5,45 brut à 07:01**, l'écart de **+0,11** entre la mesure de référence de
   l'utilisateur (appareil jaune, ~**7,35** entre 07:50 et 08:00) et le pH filtré
   de l'appareil (moyenne **7,239** sur la même fenêtre), et la durée de purge de
   ~43 min.
2. **L'air n'explique PAS le transitoire des matins précédents.** L'hypothèse
   « air dans le circuit » est écartée pour 02-04/09 par observation directe.
   Le transitoire de reprise (~45-50 min, dans le BRUT, insensible au filtrage)
   reste **inexpliqué** — feature-069 / N1 garde son objet.
3. **L'écart de +0,11 ne dit rien sur l'offset pH.** Mesure prise juste après le
   rinçage, avec de l'air dans le circuit — exactement les deux conditions que le
   protocole N8 dit d'éviter. Voir [[offset-ph-nature-et-non-suspicion]] : l'offset
   corrige un décalage de MESURE, ce n'est pas une suspicion de sonde. À refaire
   en régime stable, eau du bassin.

**Ce qui reste valide malgré la contamination** : le comportement de la purge
adaptative (v2.35.0, feature-091) a été **exemplaire** — porte maintenue fermée
pendant tout le V, ouverture à 07:40 sur brut 6,77 (le plateau) au lieu de 07:12
sur brut 5,97 (le fond) en v2.33.0 ; intégrale préservée à 0 contre −191 le
04/09 ; **zéro alerte à tort contre trois le 04/09**. Le dosage chlore n'a PAS
été emballé par l'air : 424 mL à 08:14 contre 470 mL à la même heure le 04/09.

**Test décisif : le matin suivant le remplissage.** Bassin à niveau, air chassé.
Si le transitoire s'effondre, la cause est mécanique (niveau) ; s'il persiste à
45-50 min, l'air n'était qu'un facteur du 05/09 et N1 reste entier.

Voir [[mesure-ph-perturbee-en-phase-pompe]] et
[[incident-bypass-sondes-ferme]] (précédent : la garde « présence d'eau » ne
voit pas ce qui se passe dans la chambre de mesure).
