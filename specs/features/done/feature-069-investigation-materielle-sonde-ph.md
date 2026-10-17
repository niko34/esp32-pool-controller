## Métadonnées
- **ID** : feature-069
- **Slug** : investigation-materielle-sonde-ph
- **Type** : investigation (matérielle + protocole humain ; firmware léger éventuel)
- **Statut** : **CLÔTURÉE le 2026-10-17** — ⛔ hors périmètre logiciel : investigation **matérielle**, du ressort de celui qui conçoit le PCB. ⚠️ Le phénomène reste **NON EXPLIQUÉ** (voir « Clôture » en fin de document).
- **Priorité** : haute
- **Version cible** : N/A (investigation ; un éventuel correctif fera l'objet d'une spec dédiée)
- **Dernière mise à jour** : 2026-09-04
- **Spec(s) liée(s)** : feature-091 (purge adaptative — traverse le transitoire sans dégât ; cette spec cherche à le **supprimer**), plan 2026-09 (N1, N8, N9), `bug-filtre-ph-verrouille-par-rejet-de-spike-sans-issue` (v2.20.3), [ADR-0041](../../../docs/adr/0041-autorite-mesures-non-representatives-et-sortie-de-secours-filtre.md), feature-024 (pente sonde pH), feature-022 (capteur figé)

## Historique de la spec

- **2026-08-21** — écrite comme investigation *conditionnelle* de l'excursion à ~4,8 pH du 16/08, avec pour hypothèse concurrente la « stagnation acide au point d'injection ».
- **2026-09-04** — refondue (« ménage » du plan 2026-09, décision utilisateur) : l'hypothèse de stagnation acide est **enterrée** (les sondes sont en amont des piquages d'injection dans le sens de circulation ; le brut est *normal* pendant toute la nuit d'eau stagnante). L'objet devient le **transitoire de reprise** observé 3 matins sur 3. Les relevés « sonde » (pente, étalons) de la première version sont conservés en seconde étape.

## Le phénomène à expliquer : le transitoire de reprise

Trois matins consécutifs, même signature (historique brut 5 min + `[STAB]` + enregistreur 1 min du 04/09) :

| Matin | Reprise filtration | Brut à la reprise (eau stagnante) | Brut minimum | Quand | Brut redevenu normal | Filtrée à 7,30 | ORP |
|---|---|---|---|---|---|---|---|
| 02/09 | 06:57 | 6,85 | 5,60 | t+5 min | ~t+35 min | ~t+35 | −74 mV même forme |
| 03/09 | 06:57 | 6,72 | 6,04 | t+15 min | ~t+33 min | ~t+35 | −60 mV |
| 04/09 | 06:57 | 6,78 | **5,71** | t+13 min (fin de purge 15 min) | ~t+48 min | t+50 | −56 mV (583 → 527) |

Faits établis :
- Le brut est **normal** à la reprise (6,72-6,85, soit ~7,28 avec l'offset +0,5) : l'eau stagnée toute la nuit dans la chambre lit juste. Le plongeon commence **quelques minutes après** le début de circulation.
- Le plongeon est dans le **brut** (module EZO), donc **aucun filtre logiciel ne peut le corriger** — c'est la sonde qui lit 5,7, ou l'eau qui passe devant elle qui est réellement à 5,7 pendant 30-45 min.
- L'ORP plonge en même temps (−56 à −74 mV) et remonte sur la même durée.
- Les 30-45 min de retour sont incompatibles avec un simple bouchon d'eau dans la tuyauterie (renouvelée en 1-2 min à ce débit).

### Hypothèses ouvertes (à départager par les relevés)

| # | Hypothèse | Ce qu'elle prédit | Ce qui la tuerait |
|---|---|---|---|
| H1 | **Chambre de sonde vidée ou désamorcée pendant la nuit** (bypass qui se vide, siphon) : la jonction de référence se ré-humidifie lentement à la remise en eau | De l'air sort à la buse de refoulement les premières minutes (chambre non transparente : indice indirect) ; bandelette du bassin **normale** pendant que la sonde lit 5,7 | Aucun air à la buse et brut néanmoins bas → H1 affaiblie (pas exclue : la chambre peut se vider sans que l'air atteigne la buse) |
| H2 | **Bulles d'air piégées** dans le bypass chassées à la reprise, passant sur les électrodes | Air à la buse ; brut erratique (pas seulement bas) sur la courbe 10 s | Aucun air, brut bas mais lisse |
| H3 | **Effet électrique de la pompe de filtration au démarrage** (masse, courant de fuite — cf. bruit permanent attribué à cette pompe) | Le plongeon commence exactement au démarrage moteur, pas quand l'eau arrive ; bandelette normale | Brut normal les 2 premières minutes de rotation (le 04/09 le brut est encore 6,78 à 07:05 alors que la pompe tourne depuis 8 min → H3 peu probable) |
| H4 | **Eau réellement acide** au refoulement pendant 30-45 min (poche d'eau dans le filtre à sable, l'échangeur, ou le circuit de dosage qui se vidange au démarrage) | Bandelette du **refoulement** à t+2/t+15 nettement plus acide que celle du bassin | Bandelettes bassin et refoulement identiques |
| H5 | Sonde vieillissante (pente effondrée), temps de réponse long après stagnation | Pente < 85 % et/ou réponse en étalon > 60 s | Pente et réponse en étalon correctes |

## Domaines touchés
- [x] Capteurs (pH / ORP / température)
- [x] Régulation (pump_controller, PID) → **passage obligatoire par `pool-chemistry`** (la régulation pH doit être en `manual` pendant toute manipulation de sonde)
- [ ] Filtration / Éclairage
- [ ] Web — routes HTTP / WebSocket
- [ ] MQTT / Home Assistant
- [ ] OTA
- [ ] Authentification
- [ ] Historique / persistance
- [ ] Frontend (`data/`)

## Agents requis
- [x] spec-reader
- [ ] architect — seulement si un correctif firmware en découle
- [x] pool-chemistry — lecture électrochimique des relevés, verdict
- [ ] ux-ui-designer
- [ ] embedded-developer — seulement si l'investigation conclut à un correctif firmware
- [ ] web-ui-developer
- [ ] test-engineer
- [ ] code-reviewer — seulement si du code est touché
- [x] doc-writer — consignation du verdict

## Description

Expliquer le transitoire de reprise (pH brut 5,6-6,0 et ORP −60 mV pendant 30-45 min chaque matin) et conclure : **chambre/bypass à modifier**, **eau réellement acide au refoulement**, **câblage/masse**, ou **sonde à remplacer**. Le livrable est un **verdict documenté**, pas du code. Tant que la cause n'est pas supprimée, feature-091 la *traverse* (porte fermée sur stabilité) ; cette spec vise à faire tomber la purge à son plancher (~3 min) en supprimant la cause.

## Comportement attendu

### Étape 1 — Protocole de relevé à la reprise (humain, 1 à 2 matins)

Reprise programmée de la filtration : **06:57**. Matériel : bandelettes pH (suffisantes pour distinguer 6,3 de 7,3 ; l'offset à 0,1 près relève de N8), une montre, de quoi noter. **Ne rien toucher au firmware ni aux réglages** ; l'enregistreur externe (`record_morning.sh`, 1 point/10 s la première heure + vidage de `/debug/sensor_trace` à 07:17 et 07:42) tourne côté Mac.

| Heure | Action | Ce qu'on cherche |
|---|---|---|
| **06:50** (avant la reprise) | Noter le pH affiché par l'UI. Bandelette **bassin** (au bord, pas au refoulement) → référence de la nuit. | Valeur vraie du bassin avant tout mélange |
| **06:57-07:02** | Se placer devant une **buse de refoulement dans le bassin** et regarder les 5 premières minutes de circulation : de l'air sort-il (bulles, crachotements, combien de temps) ? *La chambre de sonde n'est pas transparente et le local technique n'est pas observable : l'air à la buse est le seul indice visible d'un circuit (et donc peut-être d'une chambre) qui s'est vidé ou désamorcé pendant la nuit.* | H1, H2 (indirect) |
| **t+2 (06:59)** | Bandelette **refoulement** (buse de refoulement, eau qui sort) **et** bandelette **bassin**. Noter le pH affiché par l'UI et l'heure exacte. | H4 : l'eau qui sort est-elle vraiment acide ? |
| **t+15 (07:12)** | Bandelette refoulement + bassin. Noter le pH UI (c'est l'heure où la sonde est au plus bas). | H4 vs H1/H5 : si la bandelette dit 7,3 pendant que la sonde dit 5,7 → c'est la **sonde/chambre**, pas l'eau |
| **t+35 (07:32)** | Bandelette bassin. Noter le pH UI. | Fin du transitoire : cohérence bandelette / sonde |
| Facultatif **t+60** | Bandelette bassin. | Référence « eau mélangée » du matin (utile à N8/N9) |

Consigner le tout dans un tableau (heure exacte ; bandelette bassin ; bandelette refoulement ; pH UI ; air à la buse de refoulement). Deux matins valent mieux qu'un si la première lecture est ambiguë.

**Grille de lecture** :
- Bandelettes bassin ET refoulement à ~7,3 pendant que la sonde lit 5,7 → **la sonde lit faux** → H1/H2/H5 : passer à l’étape 2 (et ouvrir la chambre lors de la sortie de sonde : niveau, dépôts).
- Refoulement nettement plus acide que le bassin à t+2/t+15 → **l'eau est vraiment acide** (H4) → chercher la poche (filtre, échangeur, circuit d'injection) ; la purge sur stabilité de 091 est alors la bonne réponse et sa durée ne baissera pas.
- Chambre vide ou basse à 06:50 → H1 quasi certaine → correctif hydraulique (clapet, position du bypass, purge d'air) : décision utilisateur.

### Étape 2 — Relevés « sonde » (si l'étape 1 conclut « la sonde lit faux »)

| Mesure | Source | Attendu sur une sonde saine |
|---|---|---|
| Pente acide / basique | `GET /data` `phSlopeAcid` / `phSlopeBase` (feature-024) | ≈ 95-105 % |
| Décalage zéro | `phSlopeZero` | proche de 0 mV (relevé 08/2026 : −1,03 mV — sonde saine) |
| Âge de la dernière calibration | `phSlopeAgeMs`, `phCalPoints` | calibration récente, 2-3 points |
| Réponse en solution étalon pH 7 puis pH 4 | écran de calibration (readouts sur le **brut**) | valeur stable, temps de réponse < 60 s |
| **Réponse après 8 h hors circulation** : sonde laissée dans un bécher d'eau du bassin stagnante une nuit, puis eau renouvelée | readouts bruts | reproduit ou non le plongeon hors de l'installation (isole H1/H5 de H3/H4) |
| Dispersion du brut, filtration en marche | `GET /debug/sensor_trace` | sous `kPhFilterMaxDispersion` = 0,5 pH |

### Étape 3 — Différenciation finale

| Signature | Cause | Suite |
|---|---|---|
| Bandelettes normales, air à la buse | **Implantation hydraulique** (H1/H2) | Correctif matériel (décision utilisateur) ; 091 garde la porte fermée en attendant |
| Bandelettes normales, pas d’air, pente effondrée ou réponse lente | **Sonde en fin de vie** (H5) | Remplacement |
| Bandelettes normales, plongeon calé sur le démarrage moteur | **EMI / masse** (H3) | Renvoi diagnostic feature-057 / latch EMI feature-058 ; blindage, masse |
| Refoulement réellement acide | **Poche d'eau acide** (H4) | Localiser ; 091 est la réponse firmware définitive |
| Rien de reproductible | Non reproductible | Clore, 091 seule |

### Cas limites
- Bandelette lue trop tôt/tard (les couleurs virent) : lire à la durée indiquée sur le tube, à la lumière du jour.
- Refoulement inaccessible à la main : prélever dans un gobelet à la buse.
- Sonde neuve mal rodée, étalon périmé, module EZO en défaut : mêmes réserves que la version d'origine (vérifier les dates d'étalon ; comparer `Slope,?` / `Cal,?`).

## Comportement actuel

Le firmware expose tout le nécessaire (`phRaw`, `orpRaw`, `phSlope*`, `phCalPoints`, `/debug/sensor_trace` anneau 25 min à 5 s, `/debug/sensor_filter_state`, lignes `[STAB]` début/fin de purge, `measures_stab_remaining_s`). Aucun relevé de référence **humain** n'a encore été fait à la reprise : on ne sait pas si l'eau ou la sonde est en cause. La mitigation actuelle (purge fixe 15 min) est insuffisante (incident du 04/09 identique à celui du 02/09) — traitée par feature-091.

## Contraintes techniques
- Étape 1 : **aucune** manipulation du firmware ni des réglages ; ne pas réarmer les filtres ; ne pas passer en manuel (on veut la trace normale).
- Étape 2 : régulation pH en `manual` avant de sortir la sonde, retour explicite après ; raisonner **exclusivement sur le brut** ([ADR-0039](../../../docs/adr/0039-offset-ph-applique-en-sortie-des-accesseurs.md)) ; ne jamais réarmer un filtre pendant les relevés (efface figé/latch EMI).
- Firmware léger *éventuel* : si l'anneau 25 min de `/debug/sensor_trace` s'avère trop court pour couvrir la reprise sans vidage externe, une spec `modele.md` pourra l'allonger ou l'historiser (rejoint N4).

## Fichiers potentiellement concernés
- Aucun fichier code a priori — investigation
- `docs/subsystems/sensors.md` — consignation du verdict
- `docs/user/src/guide-utilisation.html` — si un geste d'entretien (purge d'air, niveau de chambre) en découle
- `CHANGELOG.md` — seulement si un correctif est livré

## Livrables — check-list
- [ ] A1 — Tableau de relevés de l'étape 1 (au moins 1 matin, 2 si ambigu) : heures, bandelettes bassin/refoulement, pH UI, air à la buse
- [ ] A2 — Courbe brute 10 s de la première heure + vidages `sensor_trace` (enregistreur externe) archivés dans `builds/2.33.0/esp32dev_core3/`
- [ ] A3 — Relevés « sonde » de l'étape 2 si nécessaire (pente, zéro, étalons, test bécher)
- [ ] A4 — Verdict explicite : implantation / eau acide / EMI / sonde / non reproductible, avec l'hypothèse retenue et celles écartées
- [ ] A5 — Décision sur la suite : correctif matériel (utilisateur), spec firmware, ou « 091 seule suffit »
- [ ] C1 — Verdict consigné (`docs/subsystems/sensors.md`) ; plan 2026-09 mis à jour (N1 → done, N6 alimenté pour l'ORP)

## Sécurité chimique
- Dosage impliqué : **indirectement** — la sonde pH est l'entrée de la régulation pH et de la garde pH/chlore (feature-067) ; pendant l'étape 1 la régulation reste en automatique et ses gardes actuelles (butée brute 6,20, plancher 6,70) bloquent déjà toute injection sur le transitoire
- Limites journalières / horaires concernées : aucune
- Filtration requise avant action : oui pour l'étape 1 (c'est l'objet) ; `manual` pour l'étape 2
- Calibration capteur modifiée : possiblement (étape 2)
- Algorithme PID touché : non
- Risque identifié : **doser sur une sonde manipulée hors de l'eau** (étape 2) → `manual` avant, retour explicite après

## ADR attendu ?
- **Probable** : non
- **Critère(s) candidat(s)** : seulement si l'investigation conduit à une décision structurante (déplacement de la chambre de mesure, changement de type de sonde)
- **Sujet pressenti** : aucun

## Éléments visibles (pour doc-writer)

### Endpoints HTTP concernés
- aucun (consommation de routes existantes)

### Champs WebSocket
- aucun

### Topics MQTT
- aucun

### Documentation à produire / mettre à jour

#### Docs vivantes
- `docs/subsystems/sensors.md` — verdict et signature du transitoire de reprise

#### Docs transverses
- `CHANGELOG.md` — uniquement si un correctif est livré

#### Docs immuables
- aucun a priori

## Critères d'acceptation
- [ ] Les relevés de l'étape 1 existent pour au moins un matin, avec bandelette **bassin** et **refoulement** à t+15 (le point décisif)
- [ ] Un **verdict explicite** est écrit et l'hypothèse retenue est cohérente avec les 3 matins déjà observés (brut normal à la reprise, plongeon différé, retour en 30-45 min, ORP synchrone)
- [ ] La suite est décidée et tracée (correctif matériel, spec firmware, ou clôture « 091 suffit »)
- [ ] Le critère resté ouvert dans `bug-filtre-ph-verrouille-…` (« origine de l'excursion à ~4,8 tranchée ») est cochable ou explicitement déclaré hors de portée
- [ ] `pio run` compile sans nouveau warning (si du code est touché)

## Plan de test manuel
1. Exécuter l'étape 1 un matin (tableau ci-dessus), l'enregistreur externe armé la veille.
2. Confronter les bandelettes à la courbe brute 10 s : identifier la fenêtre où sonde et bandelette divergent.
3. Selon la grille de lecture, faire l'étape 2 ou l'inspection hydraulique.
4. Écrire le verdict ; mettre à jour le plan et `sensors.md`.

## Notes pour agent
- Le protocole de l'étape 1 est fait pour être exécuté **par l'utilisateur seul** avec une bandelette : ne pas l'alourdir.
- L'hypothèse « stagnation acide au point d'injection » est **enterrée** (sondes en amont, brut normal la nuit) : ne pas la rouvrir.
- L'offset pH +0,5 n'est **pas** une suspicion de sonde ([mémoire projet]) ; sa re-vérification relève de N8, pas de cette spec.
- Le transitoire ORP (N6) se lit sur les mêmes relevés : noter aussi l'ORP UI aux mêmes heures.

## Hors périmètre
- Correctif firmware : spec dédiée si nécessaire (091 traite déjà la traversée du transitoire)
- Modification de l'implantation hydraulique (travaux) : décision utilisateur
- Vérification de l'offset pH à 0,1 près (N8, photomètre)


## Observation du 05/09/2026 — de l'air dans le circuit, mais CE MATIN SEULEMENT

Le niveau d'eau du bassin était trop bas : de l'air entrait par les skimmers et
ressortait audiblement aux buses de refoulement. Remplissage effectué dans la
matinée.

**Cette condition est NOUVELLE** — la piscine tournait le 04/09 après-midi sans
air aux buses (observation directe de l'utilisateur). L'hypothèse « air dans le
circuit » est donc **ÉCARTÉE comme explication des transitoires des 02, 03 et
04/09**. Elle explique peut-être l'amplitude de celui du 05/09 (fond à 5,45 brut
contre 5,60 le 02/09 et 5,97 le 04/09), pas les précédents.

**Ce que cela impose au protocole :**

1. **Les relevés du 05/09 sont à isoler**, jamais à agréger avec les autres
   matins — y compris l'écart de +0,11 entre la mesure de référence (~7,35,
   appareil jaune, 07:50-08:00) et le pH filtré de l'appareil (7,239).
2. **Ajouter le niveau d'eau à la grille de lecture** : il n'y figurait pas. À
   vérifier et à noter à chaque relevé matinal, au même titre que l'air aux buses.
3. **Le matin suivant le remplissage est le test décisif.** Bassin à niveau, air
   chassé : si le transitoire s'effondre, la cause est mécanique ; s'il tient
   45-50 min, N1 reste entier et les hypothèses H1-H5 gardent leur objet.
4. La chambre de sonde n'étant pas observable, l'air aux buses reste le seul
   témoin accessible de ce qui traverse la mesure. Précédent à garder en tête :
   la garde « présence d'eau » ne voit pas ce qui se passe dans la chambre
   (incident du bypass fermé, 23/08/2026).


---

## Clôture — 2026-10-17 : du ressort de CELUI QUI CONÇOIT LE PCB, pas du logiciel

**Décision de l'utilisateur**, ce jour : *« ces investigations sont du ressort de l'humain qui
conçoit le PCB »*.

⚠️ **Et la spec le disait déjà sans qu'on le lise** : ses agents requis sont `spec-reader`,
`pool-chemistry` et `doc-writer` — ⛔ **aucun développeur**. Il n'y a **rien à coder** ici. Ce
qu'elle demande, ce sont des **relevés physiques** : bandelettes au refoulement *et* au bassin
pendant le transitoire, observation d'air à la buse, étalons, mesure de pente. ⇒ Cela exige
**une personne au bord du bassin avec des instruments**, ⛔ pas une session de travail.

### 🔴 CE QUI RESTE NON EXPLIQUÉ — ⛔ ne pas lire cette clôture comme une résolution

Le transitoire de reprise **n'a reçu aucune explication**. Trois matins (02, 03, 04/09), même
signature : brut **normal** à la remise en circulation, puis plongeon à **5,60-5,71** quelques
minutes plus tard, retour à la normale en **30 à 48 min**, ⚠️ **l'ORP plongeant en parallèle**
(−56 à −74 mV).

⛔ **Les cinq hypothèses H1→H5 restent toutes ouvertes.** ⚠️ Aucun relevé départageant n'a été
fait.

⚠️ **Un élément est arrivé APRÈS la rédaction** et n'a jamais été reporté ici : le matin du
**05/09 a été contaminé par de l'air dans la filtration** (niveau d'eau bas) — condition
**nouvelle**, absente le 04/09 après-midi. ⇒ Cela **renforce H1/H2** (chambre désamorcée, bulles
piégées) ⛔ **sans les démontrer**, et ⛔ **interdit d'agréger ce matin-là aux trois autres**.

### Ce qu'il faudrait pour trancher, le jour où quelqu'un reprend le sujet
⛔ **Deux** bandelettes simultanées — **bassin** et **refoulement** — à t+2 et t+15 min après la
reprise. C'est le seul relevé qui sépare « l'eau est réellement acide » (**H4**) de « la sonde
lit faux » (**H1/H2/H5**). ⚠️ Tout le reste en découle.

⚠️ **Préalable de sécurité** : régulation pH en **manuel** pendant toute manipulation de sonde.

### Où le sujet continue de vivre
⇒ `kicad/README.md` (point d'entrée matériel) · [[feature-137]] (même famille : perturbation de
mode commun) · bug-135 (la butée brute se déclenche sur ces lectures isolées).
