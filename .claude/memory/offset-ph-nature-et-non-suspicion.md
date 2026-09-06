---
name: offset-ph-nature-et-non-suspicion
description: "L'offset pH corrige un décalage de MESURE dû aux courants de fuite — ce n'est pas une dérive de sonde, et un offset élevé n'est pas un signal de risque"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 82048c5b-7e8f-4cec-8242-ad9b5ed01f91
  modified: 2026-08-28T07:49:15.840Z
---

**L'offset pH de cette installation corrige un décalage de MESURE causé par les
courants de fuite de la pompe de filtration. C'est tout.** Avec l'offset intégré,
la valeur de pH affichée est **juste**. Un offset à +0,50 n'est donc **pas** un
facteur de risque d'acidification, et ne doit pas être traité comme suspect.

**Why:** l'utilisateur a dû rappeler ce principe **trois fois** — la troisième
après que je l'aie laissé ressortir dans une analyse (« offset à la borne max =
scénario d'acidification »). Sa formulation la plus nette : « alors là, c'est
totalement faux comme principe. L'offset pH est là pour corriger un décalage de
mesure pH dû aux courants de fuite. C'est tout. »

Les preuves matérielles, établies et non contestées :
- diagnostic EZO : pente acide **101 %**, base **100 %**, **zéro −1,03 mV**. Un
  décalage interne de 0,50 pH donnerait un zéro à ≈ −30 mV (59 mV/pH). **La sonde
  est saine** ;
- l'offset a été établi avec **trois instruments indépendants** (PoolLab, réactif
  colorimétrique, pH-mètre autonome) ;
- campagne de bruit 22-23/08 : dispersion ×5 pompe en marche, asymétrie pH/ORP
  cohérente avec un couplage électrique via le verre (~100 MΩ) ; mesure bien
  meilleure sur batterie. Le mécanisme est **externe à la sonde**.

**How to apply:** ne jamais déduire d'un offset élevé un risque de sonde ou de
surdosage, ne pas proposer de « vérifier la sonde » au motif que l'offset est à
sa borne. Ne pas asservir un seuil de sécurité à l'offset — c'est le défaut
corrigé par la refonte des seuils (v2.25.0, [[parite-data-websocket-recidive]]
pour l'autre motif récurrent). Si un agent (gate compris) ressort ce
raisonnement, le corriger avant de relayer sa conclusion : ce fut le cas le
28/08, et c'est ce qui a rendu un troisième rappel nécessaire.

La correction physique du problème (liaison équipotentielle, alimentation
linéaire séparée type EI30) reste un chantier ouvert, hors code.
