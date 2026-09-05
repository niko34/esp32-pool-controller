# feature-083 — Repères des graphiques d'historique pH et ORP

## Métadonnées

| Champ | Valeur |
|---|---|
| Type | feature (UX + cohérence des sources de vérité) |
| Statut | doing (depuis le 30/08/2026) |
| Priorité | Moyenne — un repère faux induit en erreur plus qu'une absence de repère |
| Origine | Demande utilisateur du 30/08/2026, après analyse contradictoire |
| Version cible | 2.28.1 |

## Domaines touchés

- [x] Frontend (`data/app.js`) — `createLineChart`, constantes `PH_MIN/MAX`, `ORP_MIN/MAX`
- [x] Web — `/get-config` si les bornes idéales sont servies (arbitrage ci-dessous)
- [ ] Régulation — **aucun changement de comportement de dosage**
- [x] `docs/features/page-ph.md`, `page-orp.md`

## Sécurité chimique

**Non.** Purement de l'affichage : aucun seuil de sécurité, aucune décision de
dosage n'est touchée. La plage de **consigne** reste 7,20-7,50, les seuils de
garde restent 6,70 / 7,60, l'inhibition ORP reste 7,60.

## Le constat

Les graphiques de détail portent des bornes **codées en dur** dans le navigateur :

```js
const PH_MIN  = 7.0;    const PH_MAX  = 7.4;     // data/app.js:435-436
const ORP_MIN = 600;    const ORP_MAX = 800;     // data/app.js:442-443
```

Trois défauts distincts :

### 1. La bande ORP est trompeuse

La consigne ORP de l'installation est à **550 mV** ; la bande affiche **600-800**.
Le graphique annonce donc « hors plage basse » en permanence, alors que 550 a été
choisi délibérément après analyse — et que c'était la bonne décision.

Le fond du problème : **l'ORP n'a pas de plage universelle.** Il dépend du pH, du
stabilisant, de la température et de l'état de l'électrode. À 35 mg/L de CYA,
580 mV était déjà hors d'atteinte. Une bande fixe affirme une norme qui n'existe
pas. Le dossier technique du 28/08 le formulait ainsi : « traiter ~554 mV comme
une observation plausible et utile en tendance, pas comme une preuve de justesse
absolue ; ne pas chercher à atteindre arbitrairement 650-700 mV ».

### 2. La bande pH ne correspond à aucune référence

7,0-7,4 ne correspond ni à la plage idéale reconnue (**7,2-7,6**, optimum 7,4 —
source ci-dessous), ni à la plage de consigne de l'appareil (7,20-7,50).

### 3. Incohérence interne — les mini-graphiques font déjà mieux

```js
updateMiniChart(phMiniChart,  …, window._config?.ph_target  ?? 7.2, 0.2);
updateMiniChart(orpMiniChart, …, window._config?.orp_target ?? 700, 50);
```

Les mini-graphiques du tableau de bord consomment **déjà** les consignes servies.
Seuls les graphiques de détail utilisent des constantes figées. Cette demande
revient donc largement à aligner le détail sur ce que le tableau de bord fait
correctement.

## Ce qui est demandé

### Graphique pH

- **Bande de 7,2 à 7,6** — la plage idéale de l'EAU, pas la plage de consigne.
- **Plus une ligne de consigne** à la valeur configurée (`ph_target`, déjà servi),
  pour cohérence avec le graphique ORP.

**Distinction à respecter absolument** — c'est l'arbitrage central de cette spec :

| | Valeur | Sens |
|---|---|---|
| Bande du graphique | **7,2 – 7,6** | plage idéale de l'eau (littérature) |
| Réglage de la consigne | **7,20 – 7,50** | ce que l'appareil autorise à viser |
| Seuils de garde | 6,70 / 7,60 corrigés | où le dosage est refusé |

Ces trois plages sont **différentes et le restent**. La consigne est délibérément
plus resserrée que la plage idéale : elle laisse une marge avant l'inhibition du
chlore à 7,60. **Ne pas les unifier.**

Une première version de la demande proposait de caler la bande sur la plage de
consigne servie. Écartée après analyse : un pH à 7,10 serait apparu « hors
plage » alors qu'il est acceptable, et 7,50 aurait semblé confortable alors que
l'inhibition commence juste au-dessus.

### Graphique ORP

- **Supprimer la bande.**
- **La remplacer par une ligne de consigne** à la valeur configurée
  (`orp_target`, déjà servi).

### Les deux

- **Mise à jour automatique** quand la configuration change, sans rechargement
  de page (la consigne peut être modifiée depuis l'UI, MQTT ou l'API).
- **Repli** : si la donnée n'est pas servie (firmware antérieur), **ne rien
  tracer** plutôt qu'inventer une valeur. Piège maison : clé absente ≠ zéro.
- Conserver le calcul dynamique des bornes d'axe (`calculateAxisLimits`) : la
  ligne de consigne doit toujours être visible dans le cadre.

## Arbitrage à trancher : où vivent les bornes 7,2 / 7,6 ?

- **Option A — servies par le firmware** (nouvelles constantes dans
  `constants.h`, exposées par `/get-config`). Cohérent avec la doctrine du
  projet : l'appareil est la source de vérité, et un firmware mis à jour sans son
  système de fichiers ne présente pas de repères périmés. Coût : 2 champs,
  ~40 octets.
- **Option B — constantes d'affichage côté client.** Moins de code, mais fige une
  connaissance de domaine dans le navigateur.

**Recommandation : option A.** Le projet a corrigé **six** occurrences du motif
« constante recopiée dans le client » ; `PH_MIN`/`PH_MAX` en sont deux de plus.
Autant les traiter à la source. À documenter dans `constants.h` avec la
référence, comme les seuils de `pool-chemistry`.

> **Arbitrage tranché le 30/08/2026 — Option B retenue.**
> La bande pH 7,2-7,6 devient une **constante d'affichage côté client**,
> commentée dans `data/app.js` avec la référence (poolplanet, plage idéale
> 7,2-7,6, optimum 7,4). L'option A (bornes servies par le firmware via
> `constants.h` + `/get-config`) est **écartée pour ce cycle** — reportable
> dans une spec future. Motif : déploiement rapide sans toucher au firmware
> pendant l'observation terrain des features 084/085.
>
> **Conséquences :**
> - Le critère d'acceptation « plus aucune borne de graphique codée en dur
>   dans `data/` » est **amendé** : plus de borne ORP ni de consigne en dur ;
>   la bande pH 7,2-7,6 reste la **seule** constante, documentée avec sa
>   référence.
> - Périmètre **strictement frontend** : `data/app.js` (+ `index.html` /
>   `app.css` si besoin) et `src/version.h` → **2.28.1**. Aucun autre fichier
>   `src/`.

## Référence

Plage idéale **7,2 – 7,6**, optimum **7,4** :
<https://www.poolplanet.com/blogs/conseils/quel-est-le-meilleur-ph-de-piscine>
Sous 7,2 : irritation des yeux et de la peau, corrosion des parties métalliques
et des joints. Au-dessus de 7,6 : dépôts calcaires, eau trouble, et **le chlore
devient moins efficace** — c'est le même fondement chimique que notre seuil
d'inhibition ORP à 7,60, et ce n'est pas une coïncidence.

## Critères d'acceptation

- [x] Graphique pH : bande 7,2-7,6 **et** ligne de consigne à `ph_target`
- [x] Graphique ORP : plus de bande, ligne de consigne à `orp_target`
- [x] Changer la consigne (UI, MQTT ou API) met les lignes à jour **sans
      rechargement**
- [x] Firmware ne servant pas la donnée ⇒ repère absent, jamais inventé
- [x] `grep` (**critère amendé — Option B, arbitrage du 30/08**) : plus de
      borne ORP ni de consigne codée en dur dans `data/` ; la bande pH 7,2-7,6
      (`PH_IDEAL_MIN`/`PH_IDEAL_MAX`) reste la **seule** constante, commentée
      avec sa référence
- [x] La plage de **consigne** reste 7,20-7,50 et le curseur est inchangé
- [x] `node --check` OK, `./build_fs.sh` sous 320 Ko (+258 o gzippé)

## Notes d'implémentation (30/08/2026 — livré, tests verts, revue Approved)

Périmètre effectif : `data/app.js` + `src/version.h` (→ 2.28.1) uniquement.
970/970 tests natifs verts, `./build_fs.sh` OK (+258 o gzippé).

- **Option B appliquée** : `PH_IDEAL_MIN = 7.2` / `PH_IDEAL_MAX = 7.6`,
  constantes d'affichage commentées dans `app.js` avec la référence
  (poolplanet, optimum 7,4) et le rappel de la distinction des trois plages.
  `ORP_MIN`/`ORP_MAX` supprimées. Seuls subsistent les défauts d'axe
  (`*_AXIS_MIN/MAX_DEFAULT`), qui ne sont pas des repères.
- **`createLineChart` généralisé** : nouvelles options `referenceLines`
  (bande — pH seul), `targetLine` (consigne — pH et ORP) et `axisMin/axisMax`.
  Ordre des séries : x, [4 séries de zones], [consigne], mesure — les bands
  uPlot restent figées sur `[1,2]`/`[3,4]`, et l'index de la série de mesure
  (curseur + tooltip) est **générique** :
  `measureIdx = 1 + (hasBands ? 4 : 0) + (hasTarget ? 1 : 0)`.
- **Setter `setTarget(v)`** : les options uPlot étant capturées par closure,
  la consigne est **mutable** via l'état partagé `st.target` puis re-render
  par `setData` — pas de recréation du graphe. **Normalisation à 2 décimales**
  avant comparaison (`/get-config` sert un arrondi, le WS le float brut —
  sans elle, chaque trame provoquerait un re-render cosmétique). Valeur
  absente/non finie → **ignorée** : une trame partielle n'efface jamais une
  ligne existante ; `st.target` initialisé à `null` → rien tracé tant que la
  consigne n'est pas servie (clé absente ≠ 0).
- **Point de passage unique** : `loadConfig()` appelle
  `phChart?.setTarget(cfg.ph_target)` / `orpChart?.setTarget(cfg.orp_target)`
  — couvre UI, API et MQTT (chaque sauvegarde recharge `/get-config`, et les
  trames WS `config` passent aussi par `loadConfig`).
- **Visibilité garantie** : `calculateAxisLimits` reçoit `values ∪ target` —
  la ligne de consigne reste toujours dans le cadre.
- Docs livrées : sections « Repère(s) du graphique d'historique » dans
  `docs/features/page-ph.md` / `page-orp.md`, entrée `CHANGELOG.md` 2.28.1.
  Pas d'ADR (arbitrage consigné ici, portée locale à l'affichage).

## Hors périmètre

- Toute modification des seuils de sécurité ou de la plage de consigne
- Le mini-graphique du tableau de bord (déjà correct)
- La justesse métrologique de la sonde ORP — voir le dossier du 28/08
