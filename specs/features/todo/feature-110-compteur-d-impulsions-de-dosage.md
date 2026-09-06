## Métadonnées
- **ID** : feature-110
- **Slug** : compteur-d-impulsions-de-dosage
- **Type** : feature (observabilité / métrologie)
- **Statut** : todo
- **Priorité** : normale — **préalable** à toute conclusion sur le biais de comptage
- **Version cible** : à définir
- **Dernière mise à jour** : 2026-09-06
- **Spec(s) liée(s)** : `feature-075` (P12, la mesure qui a révélé le manque), `feature-078` (le biais lui-même), `bug-dosage-fantome-duty-non-calibre`

## Domaines touchés
- [x] Régulation (`pump_controller`) → **`pool-chemistry` obligatoire** — le compteur vit dans le chemin de dosage
- [x] MQTT / Home Assistant — publication du compteur
- [ ] Capteurs — [ ] Filtration — [ ] Web — [ ] OTA — [ ] Frontend

## Agents requis
- [x] spec-reader · [x] architect · [x] pool-chemistry · [x] embedded-developer · [x] test-engineer · [x] code-reviewer · [x] doc-writer

## Description
Compter les **impulsions de dosage réellement tirées**, par pompe, et publier ce
compteur — pour que la prochaine mesure de bidon puisse conclure sur la **nature** du
biais de comptage, ce que la mesure du 06/09 n'a pas pu faire.

## Contexte — pourquoi ce compteur, et pourquoi maintenant

Le 06/09, la première concordance bidon/compteur sur baseline propre a donné un
résultat net **en volume** :

| | |
|---|---|
| Baseline 31/08 | 8 750 mL, compteur remis à 0 |
| Compteur appareil | 5 290,44 mL |
| Mesuré physiquement (doseur 3 L à ras + 0,7 L) | 3 700 mL restants |
| Injecté réel | 5 050 mL |
| **Biais** | **+240 mL, +4,76 %** — l'appareil **sur-compte** |

Le sens est **conservateur** (quotas resserrés, alerte de bidon vide en avance) et le
biais était **déjà documenté** — `src/constants.h:2083-2089` annonce une perte fixe par
impulsion et renvoie à `feature-078`. La mesure ne l'a pas découvert : elle l'a **chiffré
pour la première fois**.

**Mais elle n'a pas pu conclure sur sa nature**, et c'est le problème que cette spec
résout. Deux modèles expliquent le même écart :

- **multiplicatif** — la comptabilité sur-estime proportionnellement au volume ;
- **perte fixe par impulsion** — à chaque tir, la pompe monte en régime et délivre moins
  que `temps × débit nominal`. Estimation : **0,21 mL par impulsion**, soit ~0,5 s de
  débit perdu au démarrage — l'ordre de grandeur exact d'un transitoire péristaltique.

Avec le **volume seul**, les deux modèles sont indiscernables : une inconnue, une
équation. Avec le **volume et le nombre d'impulsions**, ils se départagent : deux
inconnues, deux équations.

**Aucun compteur d'impulsions n'existe aujourd'hui** — `grep` ne trouve que les drapeaux
`_carryPulseLogged[]` (`pump_controller.cpp:3356` et `:3911`), qui servent à ne journaliser
qu'une fois, pas à compter.

Conséquence directe : **la prochaine mesure de bidon sera aussi peu concluante que
celle-ci**, et la suivante aussi, tant que ce compteur n'existe pas.

### Ce qui rend cette spec efficace : elle se déclenche toute seule
Le relevé n'exige **aucune tâche à mémoriser**. Le jour où le bidon est changé, il est
mesuré — c'est un geste déjà fait. Si le compteur existe et est publié, ce geste produit
une donnée **auto-suffisante**. C'est la différence entre une tâche dont il faut se
souvenir et une donnée qui se produit d'elle-même.

## Comportement attendu
- Un compteur **par pompe** (pH et ORP), incrémenté **une fois par impulsion réellement
  tirée** — pas par période d'accumulation, pas par tick.
- **Persistant** : il doit survivre à un redémarrage, sinon il ne couvre pas la fenêtre
  entre deux changements de bidon (plusieurs jours à plusieurs semaines). Le volume
  injecté l'est déjà (`orp_total_injected_ml`) — le compteur doit suivre **la même
  discipline et la même remise à zéro**, faute de quoi les deux grandeurs ne seraient pas
  comparables.
- **Remis à zéro avec le volume**, au même instant et par le même geste (changement de
  bidon), sans quoi le rapport volume/impulsions n'a aucun sens.
- **Publié** là où il sera relevé plusieurs jours après : le payload de diagnostic
  **retenu** est le bon canal (il survit à un reboot et se relit à froid). `architect`
  confirme ou propose mieux.

### Cas limites
- Impulsion **interrompue** (redémarrage en plein tir, refus de la garde `bug-100`,
  filtration coupée) : compte-t-elle ? Trancher explicitement — le volume partiel, lui,
  est compté.
- Injection **manuelle** et purges de l'assistant de mesure : elles consomment du liquide.
  Sont-elles comptées comme impulsions ? Le volume qu'elles consomment est-il compté ?
  ⚠️ Une purge non comptée **masque** une partie du sur-comptage — c'est pourquoi
  **+4,76 % est un plancher, pas une valeur**.
- Débordement du compteur sur plusieurs saisons.

## Comportement actuel
Le volume injecté cumulé est compté et persistant. **Le nombre d'impulsions ne l'est pas.**

## Contraintes techniques
- Le compteur vit dans le **chemin de dosage** : aucune allocation, aucun `String`, aucun
  log par impulsion, rien qui allonge `loopTask`.
- **Ne rien changer à la comptabilité du volume** : cette spec **observe**, elle ne corrige
  pas. Le correctif éventuel appartient à `feature-078`.
- ⛔ **Ne pas toucher à la marge délibérée du kick** : il est crédité à
  `kPumpFlow255MlPerMin = 74,0` contre **68,0 mesurés** (`constants.h:2114-2136`). Cet
  écart est **volontaire**, il représente ~19 % du sur-comptage, et le supprimer
  reviendrait à retirer une sécurité en croyant gagner en précision.

## Livrables — check-list
- [ ] A1 — compteur d'impulsions par pompe, incrémenté au tir confirmé (`plan.carryFired`)
- [ ] A2 — persistance et remise à zéro **couplées** à celles du volume injecté
- [ ] A3 — arbitrage des cas limites (impulsion interrompue, injection manuelle, purges)
- [ ] A4 — publication dans le canal retenu, relisible à froid plusieurs jours après
- [ ] D1 — test natif de la logique de comptage, **validé par mutation**
- [ ] D2 — non-régression
- [ ] C1 — `docs/subsystems/pump-controller.md`, `docs/MQTT.md`, `docs/API.md` si exposé en HTTP
- [ ] C2 — `CHANGELOG.md`
- [ ] C3 — **mode d'emploi du relevé** : que noter au changement de bidon, et comment en
      déduire la nature du biais (les deux équations). Sans ça, la donnée sera produite
      mais pas exploitée.

## Sécurité chimique
- Dosage impliqué : **oui** — le compteur s'incrémente dans le chemin de dosage.
- Limites, seuils, PID, présence d'eau : **aucune modification**.
- **Risque identifié** : qu'un compteur mal placé fasse croire à un tir qui n'a pas eu
  lieu, ou en manque un — auquel cas la conclusion sur la nature du biais serait fausse,
  et un correctif de `feature-078` serait calé de travers. `pool-chemistry` doit valider
  le **point d'incrémentation**, pas seulement son innocuité.

## ADR attendu ?
- **Probable** : non — ajout d'observabilité, aucune contrainte structurante. À confirmer.

## Critères d'acceptation
- [ ] Le compteur reflète exactement le nombre d'impulsions tirées (test natif + mutation)
- [ ] Il survit à un redémarrage et se relit à froid
- [ ] Il se remet à zéro **avec** le volume, jamais séparément
- [ ] Aucune modification de la comptabilité du volume ni des limites de dosage
- [ ] **Critère réel, différé** : au prochain changement de bidon, le couple
      (volume, impulsions) permet de **trancher entre biais multiplicatif et perte fixe
      par impulsion**. C'est la seule chose qui compte ; le reste n'en est que le moyen.

## Plan de test manuel
1. Provoquer quelques impulsions, vérifier que le compteur avance d'exactement une par tir.
2. Redémarrer, vérifier qu'il n'a pas bougé.
3. Simuler un changement de bidon, vérifier que volume **et** compteur repartent ensemble.

## Notes pour agent
- Le chemin de tir confirmé est `plan.carryFired` (`pump_controller.cpp:3356` et `:3911`).
  Les drapeaux `_carryPulseLogged[]` voisins servent à **ne journaliser qu'une fois** :
  ne pas les confondre avec un compteur, et ne pas les détourner.
- Le protocole de banc qui trancherait **sans attendre** le prochain bidon est déjà écrit
  dans `constants.h:2088` : **24 pouls de 5 s contre 12 de 10 s**, à temps de pompe égal.
  Volumes égaux ⇒ multiplicatif ; le premier plus faible ⇒ perte fixe. Cette spec est le
  chemin **sans effort humain** ; le banc est le chemin **rapide**. Les deux restent valides.

## Hors périmètre
- **Le correctif du biais** : il appartient à `feature-078`, et il ne doit pas être tenté
  avant que sa nature soit établie. Un facteur multiplicatif calé sur un point unique
  corrigerait **mal** une perte par impulsion, et **relâcherait les quotas** sur les pouls
  longs — verdict `pool-chemistry` du 06/09.
- La marge délibérée du kick (74,0 contre 68,0).
- La réduction du nombre d'impulsions comme moyen de réduire le biais : elle a déjà eu
  lieu comme **effet de bord** (`kPulseMinMs` 5 → 10 s a divisé les impulsions par deux,
  donc réduit d'autant le sur-comptage **et** le coussin de sécurité qu'il constituait).
  Si cela doit se reproduire, que ce soit une décision, pas une conséquence.
