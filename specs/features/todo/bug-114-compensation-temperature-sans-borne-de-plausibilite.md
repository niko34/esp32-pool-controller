# bug-114 — La compensation de température des sondes n'a pas de borne de plausibilité

## Métadonnées
| | |
|---|---|
| Type | bug (intégrité de mesure → sécurité chimique) |
| Statut | todo |
| Priorité | **Haute** — fausse le pH sans toucher la calibration, donc invisible aux gardes existantes |
| Version cible | à définir |
| Relevé par | `pool-chemistry`, 2026-09-07, question de complétude de l'audit des chemins d'intégrité de mesure |
| Specs liées | `bug-save-config…` (l'audit qui l'a fait remonter), `feature-070` (routes de débogage), `feature-068` (sonde de température figée) |

## Domaines touchés
- [x] Capteurs (`src/atlas_ezo.cpp:150`, commande `RT,<tempC>`)
- [x] Régulation — **indirectement, et c'est tout le sujet**
- [ ] Web — [ ] MQTT — [ ] Frontend — [ ] OTA

## Agents requis
- [x] architect · [x] **`pool-chemistry` obligatoire** · [x] embedded-developer
- [x] test-engineer · [x] code-reviewer · [x] doc-writer

## Description
La commande `RT,<tempC>` envoyée aux sondes EZO applique la **compensation de
température** de la mesure de pH. Aucune borne de plausibilité n'est visible à ce
niveau : **une température fausse fausse le pH**, sans toucher à la calibration.

## Pourquoi c'est sérieux, et différent des autres chemins
L'audit du 07/09 a établi que les filets de sécurité chimique du projet — plancher
acide 6,70 corrigé / 6,20 brut, inhibition ORP à 7,60 — sont calculés **sur la mesure**.
Ils sont dérivés de `kPhOffsetMax` et couvrent donc l'**offset**, pas les autres
falsifications de la mesure. Une compensation de température erronée est **exactement**
ce cas : la mesure sort fausse, et toutes les gardes valident la falsification.

Les seules bornes réelles restent `maxPhMlPerDay` (défaut 300 mL/j) et la limite
horaire : elles plafonnent le **débit**, mais le quota se réarme à minuit — la dérive
cumulative n'a **pas** de plafond.

## Lien avec `feature-068`
`feature-068` traite d'une sonde de température **figée** (relevée pendant l'incident
du 16/08) dont la valeur continue d'alimenter la compensation. Les deux specs se
touchent : `feature-068` décide quoi faire d'une température qu'on **sait** fausse,
celle-ci borne ce qu'on **accepte** d'envoyer. **À traiter dans cet ordre ou ensemble.**

## Comportement attendu
- Borner la température envoyée à un intervalle physiquement plausible pour un bassin
  (l'intervalle exact est à trancher par `pool-chemistry`, **pas** par le développeur).
- Refuser et **journaliser** une valeur hors bornes plutôt que de la transmettre.
- Décider explicitement du repli : dernière valeur plausible, valeur par défaut, ou
  suspension de la compensation — chacun a une conséquence différente sur le dosage,
  et c'est un arbitrage chimique.

## Sécurité chimique
- Dosage impliqué : **oui, indirectement** — la mesure pilote le dosage d'acide.
- **Risque identifié** : une borne mal choisie pourrait rejeter une température
  légitime (bassin très froid en hiver, très chaud en canicule) et dégrader la
  compensation au lieu de la protéger. `pool-chemistry` doit fixer l'intervalle **et**
  le repli.

## Critères d'acceptation
- [ ] Intervalle de plausibilité fixé par `pool-chemistry`, avec son motif écrit
- [ ] Une valeur hors bornes est refusée **et** journalisée, jamais transmise
- [ ] Le repli est explicite et validé chimiquement
- [ ] Test natif de la borne, **validé par mutation**
- [ ] Aucune régression sur la compensation en régime normal

## Notes pour agent
⚠️ Ne pas confondre avec l'**offset pH**, qui n'est **pas** une suspicion de sonde : il
corrige un décalage de **mesure** (courants de fuite), et la sonde est prouvée saine
(zéro à −1,03 mV). Ce principe a été rappelé plusieurs fois par l'utilisateur.
