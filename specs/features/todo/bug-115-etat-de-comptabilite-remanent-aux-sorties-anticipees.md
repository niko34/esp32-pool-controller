# bug-115 — L'état de comptabilité reste rémanent aux sorties anticipées : la durée d'une coupure est créditée au débit plateau

## Métadonnées
| | |
|---|---|
| Type | bug (comptabilité des volumes → fiabilité d'une mesure de terrain) |
| Statut | todo |
| Priorité | Haute **sur le fond**, mais **NE PAS IMPLÉMENTER avant la fin de la campagne de bidon** (voir ci-dessous) |
| Version cible | à définir — **après** la campagne |
| Relevé par | `code-reviewer`, 2026-09-07, pendant le cycle `feature-110` |
| Specs liées | `feature-110` (les compteurs qui l'ont révélé), `feature-078` (la campagne qu'il pollue), `bug-116` (même chaîne de sorties) |

## ⚠️ VERROU DE CALENDRIER — lire avant tout
**Ne pas implémenter tant que la campagne de concordance jerricane n'est pas close.**
Toucher à la comptabilité des volumes **invaliderait la campagne de référence en
cours**, qui démarre au prochain changement de bidon et doit produire le couple
`(V, N)` permettant de départager les deux modèles de biais de comptage.
C'est un verrou **délibéré**, pas un oubli. Voir `feature-110` § Sécurité chimique.

## Description
Les sorties anticipées de `PumpControllerClass::update()` — eau absente, stabilisation
en cours, assistant de débit (`flowAssistSuspend`), capteurs non initialisés — forcent
`applyPumpDuty(*, 0)` mais **reviennent avant la comptabilité**.

Or `_acctPumpOn[]` et `st.lastSafetyTimestamp` ne sont écrits **que** dans
`accountPumpVolume()`. Ils restent donc **rémanents** pendant toute la fenêtre aveugle.
À la reprise :
- `deltaMs` vaut **la durée entière de la coupure** ;
- `pulseDeltaVolumeMl` crédite **tout** ce delta au **débit plateau** ;
- côté compteur de démarrages, `prevOn` est resté `true` → le redémarrage physique
  **n'est pas compté**.

## Contexte — les ordres de grandeur, mesurés
Au débit plateau relevé (68 mL/min) : **5 min de coupure → ~5,7 mL · 1 h → ~68 mL ·
6 h → ~408 mL**.

Les **+240 mL** (+4,76 %) mesurés à la concordance du 06/09/2026 valent **3,5 minutes**
de crédit fantôme cumulé. Quelques reprises suffiraient donc à expliquer *tout* le
biais observé, **sans qu'aucun déficit de démarrage n'existe** — et l'assistant de débit
a été utilisé sur la fenêtre 23/08-31/08 de cette campagne.

**Les deux biais poussent δ dans le même sens** : `V` surcompte **et** `N` sous-compte.
Un déficit fixe par démarrage est donc **systématiquement favorisé** par l'asymétrie.

## Ce qui atténue déjà — et pourquoi ça ne suffit pas
Les compteurs d'hygiène livrés par `feature-110` (`*_anom`, `*_anom_ml`) **captent** ce
cas : le tick de reprise a un `deltaMs` de plusieurs ordres au-dessus de
`kAcctDeltaAnomalyMs = 5000`. Ils permettent de **disqualifier** un relevé pollué
(critère `pool-chemistry` : `anom_ml / V_bidon > 1,5 %` → non concluant).
Mais ils **signalent** le défaut ; ils ne le corrigent pas.

## Comportement attendu
Purger l'état de comptabilité dans les branches de sortie anticipée : à la reprise, le
delta doit repartir de la reprise, pas de l'arrêt, **et** le redémarrage physique doit
être compté comme un démarrage.

## Sécurité chimique
- Dosage impliqué : **oui** — c'est la comptabilité des volumes injectés.
- `pool-chemistry` **obligatoire** : la correction change les volumes rapportés, donc
  la base de toute conclusion ultérieure sur le biais.

## Critères d'acceptation
- [ ] La campagne de concordance en cours est **close** avant toute implémentation
- [ ] À la reprise après chaque sortie anticipée, aucun volume fantôme n'est crédité
- [ ] Le redémarrage physique est compté comme un démarrage
- [ ] Test natif de chaque branche de sortie, **validé par mutation**
- [ ] Les compteurs d'hygiène retombent à zéro sur les cas ainsi corrigés

## Notes pour agent
Un commentaire au-dessus de `composeDose()` (`src/pump_controller.cpp`, livré en
v2.46.0) nomme déjà les trois sorties anticipées pour le défaut voisin `bug-116`.
**Les deux traces doivent rester au même endroit** : quiconque touche à `update()` doit
trouver les deux avertissements côte à côte, sinon il rejouera l'un ou l'autre.
