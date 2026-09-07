## Métadonnées
- **ID** : feature-110
- **Slug** : compteur-d-impulsions-de-dosage
- **Type** : feature (observabilité / métrologie)
- **Statut** : done
- **Priorité** : normale — **préalable** à toute conclusion sur le biais de comptage
- **Version cible** : 2.45.0
- **Dernière mise à jour** : 2026-09-06
- **Spec(s) liée(s)** : `feature-075` (P12, la mesure qui a révélé le manque), `feature-078` (le biais lui-même), `bug-dosage-fantome-duty-non-calibre`

## Domaines touchés
- [x] Régulation (`pump_controller`) → **`pool-chemistry` obligatoire** — le compteur vit dans le chemin de dosage
- [x] MQTT / Home Assistant — publication du compteur
- [x] Web — routes HTTP / WebSocket · [x] Frontend (`data/`) — affichage
- [ ] Capteurs — [ ] Filtration — [ ] OTA

## Agents requis
- [x] spec-reader · [x] architect · [x] pool-chemistry · [x] embedded-developer
- [x] **ux-ui-designer** · [x] **web-ui-developer** (décision utilisateur du 2026-09-06 : le compteur est affiché)
- [x] test-engineer · [x] code-reviewer · [x] doc-writer

## ⚠️ CORRECTION du 2026-09-06 — la piste indiquée par cette spec était FAUSSE

Cette spec désignait `plan.carryFired` comme point d'incrémentation. **C'est faux**, et
l'erreur est exactement celle que le mandat de `pool-chemistry` existe pour intercepter.

- `pulse_dosing_logic.h:104` : `carryFired = true` **uniquement** pour un pouls déclenché
  par l'**accumulateur** (régime sous-plancher, feature-073). En régime proportionnel
  normal, `planPulseWithCarry` délègue à `planPulse` et rend `carryFired = false` — alors
  qu'un pouls part bel et bien.
- Pire : aux deux sites cités, la condition réelle est
  `plan.carryFired && !_carryPulseLogged[i]` — le drapeau ne se lève donc **qu'une fois
  par séance**. Un compteur branché là compterait des **séances**, pas des tirs.
- Il existe **quatre** sites d'émission, pas deux : pH auto (`:3384`), pH **scheduled**
  (`:3759`), ORP auto (`:3930`), ORP **scheduled** (`:4203`). Les deux `scheduled` ne sont
  **pas pulsés du tout** — marche continue avec un kick en tête de session.

**Conséquence si l'erreur avait été suivie** : `N` massivement sous-estimé ⇒ `δ = 240/N`
absurdement grand ⇒ le modèle de perte fixe « validé » à tort ⇒ `feature-078` calant un
correctif faux, dans le sens qui **relâche les quotas**.

### La bonne grandeur : le DÉMARRAGE de pompe, pas l'impulsion
Le modèle de perte fixe se paie à chaque transition **OFF→ON** — relaxation du tube,
ré-occlusion, reprise d'amorçage. L'équation `V_compté = V_réel + N·δ` exige que `N` porte
sur **le même ensemble d'événements que `V`**. Compter les démarrages **crédités** couvre
les quatre sites d'un coup, y compris le kick unique d'une session `scheduled` et chaque
injection manuelle — ce que « impulsion » ne couvrirait pas.

**Point d'incrémentation retenu** : le front `!_acctPumpOn[chem] && onNow` dans
`accountPumpVolume()` (`pump_controller.cpp:2637`), **dans la branche même qui crédite le
volume**. Par construction : *un démarrage compté ⇔ un volume crédité par ce démarrage*.

Bénéfice structurel : le `return` de `pump_controller.cpp:2924` (eau absente, stabilisation,
assistant de mesure) précède l'accounting. Le compteur **hérite donc automatiquement du même
angle mort** que le volume — c'est ce qui garde `N` et `V` homogènes sans aucune garde
ajoutée, et c'est aussi pourquoi le **+4,76 % est un plancher**.

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
- [x] A1 — compteur de **DÉMARRAGES** par pompe, incrémenté sur le front OFF→ON dans
      `accountPumpVolume()` — ⛔ **PAS** sur `plan.carryFired` (voir la correction en tête)
- [x] A2 — persistance et remise à zéro **couplées** à celles du volume injecté
- [x] A3 — arbitrage des cas limites (impulsion interrompue, injection manuelle, purges)
- [x] A4 — publication dans le canal retenu, relisible à froid plusieurs jours après
- [x] D1 — test natif de la logique de comptage, **validé par mutation**
- [x] D2 — non-régression
- [x] C1 — `docs/subsystems/pump-controller.md`, `docs/MQTT.md`, `docs/API.md` si exposé en HTTP
- [x] C2 — `CHANGELOG.md`
- [x] B1 — **affichage du compteur** (décision utilisateur du 2026-09-06) : exposition
      HTTP puis rendu sur la page des dosages, à côté du volume cumulé.
- [x] B2 — **arbitrage `ux-ui-designer`** : où exactement ? La carte seule ne suffit
      peut-être pas. Le bouton « Nouveau bidon » **détruit précisément les deux valeurs
      à relever** — afficher `V` et `N` dans sa **confirmation** les présenterait à
      l'instant exact où elles vont disparaître, sans rien à mémoriser au préalable.
      Carte, confirmation, ou les deux : à trancher, avec le motif.
- [x] C3 — **mode d'emploi du relevé** : que noter au changement de bidon, et comment en
      déduire la nature du biais (les deux équations). Sans ça, la donnée sera produite
      mais pas exploitée.

## Sécurité chimique
- Dosage impliqué : **oui** — le compteur s'incrémente dans le chemin de dosage.
- Limites, seuils, PID, présence d'eau : **aucune modification**.
- **Risque identifié** : qu'un compteur mal placé fasse croire à un tir qui n'a pas eu
  lieu, ou en manque un — auquel cas la conclusion sur la nature du biais serait fausse,
  et un correctif de `feature-078` serait calé de travers. `pool-chemistry` doit valider
  le **point d'incrémentation**, pas seulement son innocuité.

### Verdict `pool-chemistry` du 2026-09-07 : **GO**
- **Innocuité confirmée** : les 6 champs ne sont relus que par `/get-config`, la trame WS
  et le diagnostic MQTT. Aucune garde, aucun quota, aucun PID. Rien du dosage ne bouge.
- **Point d'incrémentation validé** : le front `!_acctPumpOn && onNow`, évalué avant
  l'affectation et sous la même garde `productTrackingEnabled()` que le crédit du volume,
  est la seule définition qui rende `V = V_réel + N·δ` dimensionnellement honnête.
- **Nommer le phénomène « déficit de démarrage », pas « amorçage »** : δ ≈ 0,21 mL vaut
  **0,19 s de plateau** à 68 mL/min — signature d'un déficit de **rampe** (montée LEDC,
  inertie rotor, relaxation du tube), qui se paie à chaque front. Un désamorçage réel
  coûterait bien plus et n'arrive qu'après un arrêt long ; un péristaltique ne se
  désamorce pas entre deux pouls d'une même séance. Si c'était pourtant le désamorçage,
  `N` surcompterait les fronts et **δ ajusté serait un minorant**.
- **Critère de disqualification d'un relevé** (jugé contre le signal de 4,76 %, pas
  contre le volume du bidon) : `anom_ml / V_bidon` **< 0,5 %** → concluant ·
  **0,5–1,5 %** → concluant seulement après soustraction explicite de `anom_ml` et
  publication de l'incertitude · **> 1,5 %** → **NON CONCLUANT sur δ**.
  Co-critère : `anom_count / N ≤ 2 %`.
- **Réserve dirigée vers `feature-078`, pas vers cette spec** : une campagne **unique**
  ajuste les deux modèles exactement (un paramètre, une équation) — ils sont
  indiscernables. Exiger **≥ 2 campagnes dont le mL/démarrage moyen diffère d'un
  facteur ≥ 2**. Le relevé du 06/09 n'est **pas** requalifiable après coup : les
  compteurs n'existaient pas, donc `anom_ml = 0` n'y prouve rien.

### Constat de revue à traiter hors périmètre (spec de suivi à ouvrir)
Asymétrie `V`/`N` à la **reprise**, **pré-existante** et révélée par cette feature : les
sorties anticipées de `update()` (eau absente, stabilisation, `flowAssistSuspend`,
capteurs non initialisés) coupent les pompes mais **reviennent avant la comptabilité**.
`_acctPumpOn[]` et `st.lastSafetyTimestamp` restent rémanents → à la reprise, **toute la
durée de la coupure** est créditée au débit plateau (5 min → ~5,7 mL · 1 h → ~68 mL ·
6 h → ~408 mL) tandis que le redémarrage physique **n'est pas compté**. Les 240 mL du
06/09 valent **3,5 min** de crédit fantôme. Les deux biais poussent δ dans le **même
sens** (`V` surcompte *et* `N` sous-compte) : un déficit fixe est systématiquement
favorisé par l'asymétrie. **Ne pas corriger ici** — toucher à la comptabilité du volume
invaliderait la campagne de référence en cours et sortirait du périmètre de cette spec.

## ADR attendu ?
- **Confirmé : non** (`doc-writer`, 2026-09-07) — ajout d'observabilité suivant un
  pattern existant (`hdef`/`ldef`/`xdef` sur le même topic retenu), aucune contrainte
  imposée ailleurs, aucune alternative écartée qui ne soit déjà tracée dans le code.

## Critères d'acceptation
- [x] Le compteur reflète exactement le nombre d'impulsions tirées (test natif + mutation)
- [x] Il survit à un redémarrage et se relit à froid
- [x] Il se remet à zéro **avec** le volume, jamais séparément
- [x] Aucune modification de la comptabilité du volume ni des limites de dosage
- [ ] **Critère réel, différé** : au prochain changement de bidon, le couple
      (volume, impulsions) permet de **trancher entre biais multiplicatif et perte fixe
      par impulsion**. C'est la seule chose qui compte ; le reste n'en est que le moyen.

## Plan de test manuel
1. Provoquer quelques impulsions, vérifier que le compteur avance d'exactement une par tir.
2. Redémarrer, vérifier qu'il n'a pas bougé.
3. Simuler un changement de bidon, vérifier que volume **et** compteur repartent ensemble.

## Notes pour agent
- ⚠️ Une première version de ce bloc désignait `plan.carryFired` comme chemin de tir.
  C'était **faux** — voir la CORRECTION en tête de spec : la grandeur comptée est le
  **front OFF→ON de la pompe**, dans `accountPumpVolume()`. La note a été supprimée
  plutôt qu'annotée : un lecteur qui n'ouvre que la fin du fichier refaisait l'erreur.
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
