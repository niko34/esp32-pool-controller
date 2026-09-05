# bug-082 — `flow_assist` absent des trames WebSocket (5ᵉ récidive du motif de parité)

## Métadonnées

| Champ | Valeur |
|---|---|
| Type | bug — parité `/data` ↔ WebSocket |
| Statut | todo |
| Priorité | Moyenne — fausse une estimation affichée, sans effet sur le dosage réel |
| Découvert | 29/08/2026, en auditant les autres blocs après un bug d'affichage |
| Version en cours | 2.26.0 (déployée) |

## Domaines touchés

- `src/ws_manager.cpp` — objet `flow_assist` à ajouter à la trame `sensor_data`
- `data/app.js` — `flow255For()` et l'assistant de mesure
- `docs/subsystems/ws-manager.md`, `docs/API.md`

## Sécurité chimique

**À faire viser par `pool-chemistry`.** Le champ touche l'estimation de durée
d'injection présentée à l'utilisateur. Aucune décision de dosage n'en dépend
côté firmware — mais un utilisateur qui se fie à une durée affichée fausse peut
mal juger une injection manuelle.

## Le défaut

L'objet `flow_assist` — qui porte notamment `flow255`, le débit majorant de
flotte — n'existe que dans `GET /data`. Il est **absent des trames WebSocket**.

Or l'interface tourne **quasi exclusivement au WebSocket** : `latestSensorData`
est remplacé à chaque trame. `latestSensorData.flow_assist` redevient donc
`undefined` quelques secondes après chaque chargement, et `flow255For()` retombe
sur son repli — **61 / 67 mL/min au lieu des 74 servis** depuis la v2.25.2.

Conséquence : la durée d'injection annoncée est calculée avec un débit inférieur
d'environ 15 % à celui que le firmware utilise réellement. L'utilisateur voit une
durée **plus longue** que la marche réelle de la pompe.

Ironie du repli : il est correct **en soi** (61/67 sont bien les valeurs des
firmwares antérieurs à la fusion) — c'est son déclenchement permanent qui est
fautif, pas sa valeur.

## Le motif, cinquième occurrence

| # | Version | Champ servi d'un seul côté |
|---|---|---|
| 1 | v2.21.2 | pentes de sonde (`phSlope*`) absentes de `/data` |
| 2 | v2.23.0 | 6 champs de calibration absents de la trame config WS |
| 3 | v2.25.0 | causes de refus (`ph|orpDoseBlockedReason`) absentes de `/data` |
| 4 | v2.25.2 | miroir client `FLOW_255_ML_PER_MIN` en dur (variante du motif) |
| **5** | **v2.26.0** | **`flow_assist` absent des trames WS** |

La leçon est déjà écrite en mémoire projet : *un champ servi d'un seul côté ne
disparaît pas franchement, il **clignote*** — et le défaut se manifeste par
intermittence, précisément quand le réseau flanche.

Ici la variante est plus vicieuse : le champ ne clignote pas, il est
**structurellement absent** de la source dominante. Le repli fonctionne donc en
permanence, et le défaut est **invisible** — aucune valeur manquante, aucune
erreur, juste un chiffre faux.

## Correctif attendu

1. Publier `flow_assist` dans la trame WS `sensor_data`, avec **exactement** les
   mêmes règles null/arrondi que `/data` (le précédent de la v2.21.2 est le
   modèle).
2. Vérifier la taille de trame après ajout (`kWsFrameBufBytes` = 3584 ; pire cas
   mesuré 2 552 o en v2.26.0 — l'objet `flow_assist` ajoutera ~120 o). Une
   troncature silencieuse serait pire que le défaut corrigé.
3. **Prouver la parité par grep des deux côtés**, pas par relecture.

## Prévention — à instruire au-delà du correctif

Cinq occurrences du même motif justifient mieux qu'un cinquième correctif
ponctuel. Piste : un test natif ou un script de build qui compare la liste des
clés produites par `_buildSensorJson` (WS) et par le handler `/data`, et échoue
sur toute divergence non déclarée dans une liste d'exceptions documentée.

Une exception légitime existe déjà et devra y figurer : les six bornes
`ph_target_*` / `ph_offset_*` sont servies par `/get-config` **et pas** par le
WS, délibérément (constantes de compilation, budget de trame) — c'est documenté
dans `ws-manager.md`.

## Critères de clôture

- `flow_assist` présent dans les deux sources, parité prouvée par grep.
- L'assistant de mesure et l'estimation de durée affichent le débit réellement
  utilisé par le firmware (74 mL/min à ce jour).
- Le repli 61/67 reste en place pour les firmwares antérieurs, **et n'est plus
  jamais atteint** sur un firmware à jour.
- Taille de trame vérifiée, aucune troncature.
- Mécanisme de prévention de la divergence instruit (implémenté ou explicitement
  reporté avec justification).
