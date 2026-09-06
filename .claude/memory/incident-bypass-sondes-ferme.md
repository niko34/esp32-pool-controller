---
name: incident-bypass-sondes-ferme
description: "Incident 23/08/2026 — bypass des sondes fermé pendant le premier dosage réel (v2.21.0), mesures non représentatives, 765/802 ml comptés ; la garde de présence d'eau ne voit pas le bypass"
metadata: 
  node_type: memory
  type: project
  originSessionId: 82048c5b-7e8f-4cec-8242-ad9b5ed01f91
  modified: 2026-08-23T11:51:36.729Z
---

Incident du 23/08/2026, ~30 min après la mise en service du proportionnel de
temps (v2.21.0, premier dosage RÉEL du système) : le **bypass qui alimente le
tuyau des sondes pH/ORP était fermé** (vanne manuelle). Filtration EN MARCHE,
donc la garde de présence d'eau était satisfaite — mais l'eau autour des sondes
stagnait et recevait/concentrait le produit injecté. Lectures extrêmes : pH brut
6,37 (corrigé 6,87), ORP 958 mV. La régulation, pilotée par ces mesures non
représentatives, a compté **765 ml de pH− et 802 ml de chlore** (~22 min
pompe-ON chacune, débit réel ≈ 540/620 ml) sous les ANCIENNES limites
(2000 ml/j — les limites P3 à 300 n'ont été posées qu'après coup). Impact
bassin borné : ~−0,1/−0,2 pH et +1,2-2 mg/L de chlore sur 30-50 m³. Résolution :
réouverture du bypass par l'utilisateur → retour des lectures en ~2 min, un pic
rejeté par filtre (normal).

**Why:** la garde « filtration active avant injection » vérifie la pompe de
filtration, PAS le débit réel devant les sondes — un bypass fermé est invisible
au firmware (la porte d'alimentation v2.20.3 ne couvre que filtration ARRÊTÉE).
Leçon jumelle : toujours poser les limites P3 AVANT la première mise en service
d'un nouveau modèle de dosage, jamais après.

**How to apply:** au moindre comportement de mesure aberrant pendant un dosage,
vérifier physiquement le bypass des sondes AVANT de suspecter le firmware.
Candidat de spec (non créée) : garde de plausibilité « la mesure bouge trop vite
pendant un dosage → abort de session » — aurait borné cet incident. Voir
[[parametres-bassin-utilisateur]].
