# ADR-0100 — Un seuil de santé est servi, jamais réécrit côté client

- **Statut** : Accepté
- **Date** : 2026-10-11
- **Décideurs** : `architect` (feature-154, critère 1 — contrainte sur d'autres parties du système), confirmé par `pool-chemistry`
- **Spec(s) liée(s)** : [`feature-154`](../../specs/features/done/feature-154-pastilles-de-sante-par-critere-sonde-ph.md)

## Contexte

Avant cette feature, **deux jugements de santé différents** coexistaient dans
le projet pour les mêmes trois critères (pente acide, pente base, décalage de
zéro) :

| | Firmware — `isPhProbeHealthy()` (`src/ph_guard_logic.h:138`) | Interface — `classifyPhProbe()` (`data/app.js:6374`, avant ce lot) |
|---|---|---|
| Pente basse | `< 90 %` ⇒ non sain | `< 95 %` orange, `< 90 %` orange foncé, `< 85 %` rouge |
| **Pente haute** | `> 110 %` ⇒ **non sain** | 🔴 **jamais testée** |
| Zéro | `\|zéro\| > 15 mV` ⇒ non sain | `> 15` orange, `> 30` rouge |

Les seuils de l'interface étaient des **littéraux écrits en dur** dans
`data/app.js`, sans référence à `constants.h`. Conséquence concrète : une
pente de 115 % était classée « bonne » côté UI alors que le firmware la
considère hors norme — un cas réel relevé le 2026-10-11 (verdict « sonde à
remplacer » sur une sonde dont les deux pentes étaient en réalité parfaites,
seul le zéro était en cause). La divergence s'était installée **en silence** :
aucun test, aucune revue n'avait signalé que la borne haute n'était jamais
vérifiée côté client, ni que `constants.h` ne servait ces trois constantes à
personne.

Rien dans l'architecture n'empêchait cette divergence de revenir une fois
corrigée une première fois : un futur critère de santé (par exemple sur
l'ORP, si un module le permettait un jour) pourrait être ajouté à l'écran par
un simple littéral JS, sans que personne ne remarque qu'il diverge
silencieusement de la constante firmware qui décide réellement.

## Décision

**Tout seuil de santé affiché à l'utilisateur est une valeur SERVIE par le
firmware depuis `constants.h` (route HTTP dédiée), jamais un littéral recopié
ou maintenu côté client.**

Concrètement pour ce lot : `kPhSlopeHealthyMinPct`, `kPhSlopeHealthyMaxPct`,
`kPhZeroHealthyMaxMv` sont servis par `GET /get-config`
(`ph_slope_healthy_min_pct`, `ph_slope_healthy_max_pct`,
`ph_zero_healthy_max_mv`) et consommés par `classifyPhCriterion()` côté
client — fonction **pure**, sans aucun littéral numérique dans son corps hors
`0` et `1` (bornes de boucle / arithmétique triviale). Tant qu'aucune réponse
valide n'a jamais été reçue, le store de seuils reste `null` et
`classifyPhCriterion()` rend structurellement `'indisponible'` pour les trois
critères : **le vert est inatteignable sans seuils reçus du serveur**, par
construction de la fonction, pas par discipline de relecture.

**Contrainte mécanique qui rend la règle opposable** : le harnais
`test/ph_health_vectors.js` extrait le texte réel de `classifyPhCriterion()`
(patron ADR-0099) et **asserte qu'il ne contient aucun littéral numérique
hors `0` et `1`**. Un futur contributeur qui réintroduirait un seuil en dur
(« pour aller vite ») ferait échouer la CI, pas seulement la revue de code.

## Alternatives considérées

- **Dupliquer les seuils en JS, en veillant par discipline de revue à les
  garder synchrones avec `constants.h`** (situation antérieure, de fait) —
  **rejetée** : c'est exactement le défaut que cette feature corrige. La
  discipline de revue a déjà échoué une fois (borne haute jamais ajoutée en
  18 mois d'existence du chip, feature-024 → feature-154) ; rien ne dit
  qu'elle ne faillira pas une seconde fois sur un autre critère.
- **Servir un verdict booléen déjà calculé par le firmware** (`sain` / `hors`
  par critère, calculé côté C++) plutôt que les seuils bruts — **rejetée pour
  ce lot** : exigerait d'ajouter des champs à `/data` ou à la trame WebSocket,
  ce que la spec interdit explicitement (budget de trame serré, pire cas
  coïncidant avec l'escalade de sécurité pH). Aurait fermé plus strictement la
  divergence d'arrondi (condition C4 du gate), mais au prix d'un champ
  supplémentaire sur un canal déjà contraint. Le repli retenu (seuils bruts +
  vecteurs de test aux bornes exactes sur la valeur **servie**) ferme la
  divergence réelle (< 0,05 % / < 0,005 mV) à un coût de trame nul.
- **Ne rien verrouiller, se fier à la vigilance de la revue de code future**
  — **rejetée** : c'est la situation qui a produit la divergence initiale.
  Une contrainte mécanique (grep de littéraux dans le texte extrait) ne
  dépend d'aucune mémoire humaine.

## Conséquences

### Positives
- Un seul jugement de santé pH existe désormais dans l'application : plus
  aucun risque qu'un seuil affiché diverge silencieusement de celui qui
  décide réellement une bascule de sécurité.
- Élargir ou resserrer la fenêtre de diagnostic (`constants.h`) se répercute
  automatiquement sur l'affichage, sans retouche de `data/app.js`.
- Le vert est structurellement inatteignable sans seuils reçus : un bug de
  réseau ou un firmware antérieur ne peut pas produire un faux vert par
  défaut.

### Négatives / dette assumée
- Les trois seuils sont absents de la trame WebSocket (budget de trame) :
  le client doit conserver la dernière valeur connue entre deux appels à
  `/get-config`, ce qui est une règle supplémentaire à respecter pour tout
  champ de ce type (cohérente avec les six bornes `ph_target_*` / `ph_offset_*`
  déjà servies en v2.25.0, mais c'est une exception de plus à la parité
  `/data` ↔ WebSocket habituelle du projet).
- La divergence d'arrondi entre la valeur interne et la valeur servie
  n'est pas supprimée, seulement bornée et testée (condition C4 du gate
  `pool-chemistry`) — fermer cela totalement exigerait l'alternative écartée
  ci-dessus.

### Ce que ça verrouille
- **Tout nouveau critère de santé** (pH, ORP ou futur capteur) affiché à
  l'utilisateur passe par `constants.h` + une route servie — jamais par un
  littéral client. Un contributeur qui voudrait ajouter un critère rapide
  « juste pour cet écran » doit d'abord créer la constante firmware et la
  route qui la sert.
- La contrainte est vérifiée mécaniquement : toute fonction de classement de
  santé pH doit rester **extractible** (pure, nommée, sans DOM) et **sans
  littéral numérique hors 0/1** dans son corps, sous peine d'échec CI.

## Références

- Code : [`src/constants.h`](../../src/constants.h) (`kPhSlopeHealthyMinPct`, `kPhSlopeHealthyMaxPct`, `kPhZeroHealthyMaxMv`), [`src/web_routes_config.cpp`](../../src/web_routes_config.cpp) (bloc `PARITE-CONFIG`), [`data/app.js`](../../data/app.js) (`classifyPhCriterion`, `_applyPhHealth`), [`src/ph_guard_logic.h`](../../src/ph_guard_logic.h) (`isPhProbeHealthy`)
- Test : [`test/ph_health_vectors.js`](../../test/ph_health_vectors.js), [`tools/check_ph_health_vectors.sh`](../../tools/check_ph_health_vectors.sh) (job CI `ph-health-vectors`)
- Spec : [`specs/features/done/feature-154-pastilles-de-sante-par-critere-sonde-ph.md`](../../specs/features/done/feature-154-pastilles-de-sante-par-critere-sonde-ph.md)
- Doc externe : [`docs/API.md`](../API.md), [`docs/features/page-ph.md`](../features/page-ph.md)
- Précédent amendé : [ADR-0099](0099-tester-par-extraction-de-texte-jamais-par-duplication.md) (patron d'extraction réutilisé pour `classifyPhCriterion`)
