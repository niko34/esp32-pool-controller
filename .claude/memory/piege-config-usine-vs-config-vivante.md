---
name: piege-config-usine-vs-config-vivante
description: "Toute analyse doit lire /get-config sur l'appareil, jamais les valeurs d'usine de config.h — une chaîne de diagnostic entière (ADR-0053, feature-086) a été bâtie sur une limite horaire d'usine (5 min/h) alors que l'appareil était à 25"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 82048c5b-7e8f-4cec-8242-ad9b5ed01f91
  modified: 2026-08-30T19:46:13.079Z
---

Le 30/08/2026, le gate pool-chemistry a diagnostiqué « installation limitée par
l'actionneur : 5 min/h × 26 mL/min = 911 mL/j délivrables pour 1 800 requis ».
Ce diagnostic a piloté l'ADR-0053, la feature-086 entière, le CHANGELOG des
v2.27.0-2.29.0 et la justification de l'ADR-0056. Or `ph_limit_minutes` valait
**25** sur l'appareil (relevé par Nicolas entre le 24 et le 30/08) : capacité
réelle ~8 450 mL/j, installation PAS limitée par l'actionneur. Le « 5 min/h »
venait de la valeur d'usine `config.h:88`, jamais relue sur l'appareil.

C'est la feature de visibilité (v2.29.0, champs `ph_sustainable_ml_per_min` /
`ph_daily_max_ml`) qui a exposé l'écart à sa première lecture.

**Why :** les agents lisent le code (constants.h, config.h) — les valeurs
d'usine y sont visibles et citables, la config NVS vivante ne l'est pas. Un
diagnostic chiffré hérite silencieusement de cette substitution, et chaque
étape aval le consolide (ADR, specs, CHANGELOG).

**How to apply :** avant tout diagnostic quantitatif sur le comportement de
l'appareil, lire `curl -u admin:… http://poolcontroller.local/get-config` et
citer les valeurs RÉELLES (limites horaires, quotas, débits calibrés, planning
filtration). Toute valeur tirée de config.h/constants.h doit être étiquetée
« valeur d'usine, non vérifiée sur l'appareil ». Vérifier aussi la cohérence
avec les volumes observés (un délivré > max théorique prouve une config
différente). Cf. [[parametres-bassin-utilisateur]].
