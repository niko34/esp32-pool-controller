# feature-133 — Identifier le second mécanisme de fragmentation

## Métadonnées
- **ID** : feature-133
- **Slug** : identifier-le-second-mecanisme-de-fragmentation
- **Type** : bug
- **Statut** : todo
- **Priorité** : **normale** — relevée de « basse » le 2026-10-08. Elle était classée
  « curiosité » faute de matière ; elle a désormais une trace datée, un palier mesuré, trois
  candidats écartés **expérimentalement**, et un **instrument en place** (feature-144) qui
  datera la prochaine chute.
- **Version cible** : N/A — diagnostic
- **Dernière mise à jour** : 2026-09-18
- **Spec(s) liée(s)** : `specs/features/done/feature-129-…` · ADR-0081 (recensement + non-livraison de la lecture) · ADR-0082 · `docs/incidents/2026-09-10-recidive-fragmentation-v2500.md`

## Domaines touchés
- [ ] Capteurs (pH / ORP / température)
- [ ] Régulation (pump_controller, PID)
- [ ] Filtration / Éclairage
- [ ] Web — routes HTTP / WebSocket
- [ ] MQTT / Home Assistant
- [ ] UART / écran LVGL
- [ ] OTA
- [ ] Authentification
- [ ] Historique / persistance
- [ ] Frontend (`data/`)

> Domaine inconnu — c'est l'objet de la spec. À cocher une fois le coupable
> identifié.

## Agents requis
- [x] spec-reader
- [x] architect
- [ ] pool-chemistry
- [ ] ux-ui-designer
- [ ] embedded-developer — **à activer seulement si un correctif est décidé**
- [ ] web-ui-developer
- [ ] test-engineer
- [ ] code-reviewer
- [x] doc-writer

## Description

Identifier les quelques allocations de longue durée qui, en se posant au milieu
du tas pendant les premières heures, coûtent environ 14 Ko de contiguïté. Le
**diagnostic** est le livrable ; le correctif n'est pas acquis.

## Contexte — ce qui est DÉJÀ su, et qui évite de refaire le travail

Après feature-129, relevés des 17 et 18/09/2026 (détail dans la note d'incident) :

```
0x3FFF417C ┐
           │  TROU LIBRE                     13 100 o
0x3FFF74A8 ┘
0x3FFF74AC ▓  BLOC OCCUPÉ — 44 o de bloc, 32 o DEMANDÉS
0x3FFF74D8 ┐
           │  TROU LIBRE                     35 616 o
           ┘
```

**32 octets demandés détruisent 13 100 octets de contiguïté.** Sans ce bloc, le
plus grand bloc contigu vaudrait 48 812 o au lieu de 34 804.

Quatre autres blocs occupés de la même fenêtre sont également petits (40, 132,
28, 24 o demandés), groupés en `0x3FFF4030-0x3FFF4178`.

**Ce n'est plus le journal** : `log()` n'alloue plus rien depuis v2.52.1. Le
premier mécanisme masquait le second.

### Le mécanisme est STATIQUE — c'est le fait le plus utile

Deux relevés à 10,6 h d'intervalle (31 h et 41,6 h d'uptime) donnent une
géométrie **identique à l'octet et à l'adresse près** : mêmes 24 trous, mêmes
tailles, mêmes emplacements, et la mémoire libre a même légèrement monté.

Le mécanisme agit pendant les **~7,4 premières heures** puis s'arrête net. Ce ne
sont pas des allocations qui grignotent continûment, mais quelques allocations de
très longue durée qui se posent une fois et ne bougent plus.

> ⛔ **Corollaire opératoire : un recensement pris tardivement ne montrera
> JAMAIS rien.** Il faut **redémarrer l'appareil** et observer les 7,4 premières
> heures. Ce n'est pas une commodité, c'est une condition de la mesure.

### Hypothèses déjà testées et RÉFUTÉES — ne pas les refaire

Les marches valent 4 096 / 6 144 / 6 144 / **+6 144** / 12 288 o.
`kRotateKeepBytes` vaut 6 144 et `oom_reserve_bytes` vaut 12 288, ce qui invitait
à soupçonner :

- **La rotation du journal : NON.** Elle copie par blocs de 512 o
  (`src/logger.cpp:709-716`), elle n'alloue aucun tampon de 6 144 o.
- **Le parachute anti-OOM : non sur cette fenêtre.** `oom_probe_count = 0` et
  `oom_reserve_internal = true` : la réserve n'a pas été consommée, donc pas
  réarmée. Elle **reste suspecte si un OOM survient** — le réarmement réalloue
  12 288 o qui se poseront quelque part.

### La piste la plus discriminante restante

La marche de **+6 144** (une **remontée**, à 6,8 h d'uptime) : quelque chose
d'environ 6 Ko est alloué **puis libéré**. v2.51.0 n'avait montré aucune remontée
sur 28 relevés. C'est le fil le plus prometteur.

## Comportement attendu

Nommer les allocations en cause : **quoi, alloué par qui, à quel moment**. Comme
pour feature-129, une identification vaut par son adresse, sa taille **et son
contenu** — pas par une conjecture plausible.

Puis **décider explicitement** s'il y a lieu de corriger. Un « on ne corrige
pas, voici pourquoi » est un résultat acceptable et doit être écrit.

## Comportement actuel

Plateau stable à 34 804 o, 4,2× au-dessus du seuil de refus, sans dérive.
Aucun symptôme d'exploitation.

## Contraintes techniques

- **Build de diagnostic obligatoire** : la route de lecture de contenu est
  délibérément absente des binaires de production (ADR-0081, condition S12). La
  commande exacte n'est écrite **que dans ADR-0081** — `tools/check_no_peek_flag.sh`
  interdit de la recopier ailleurs, y compris dans cette spec.
- ⛔ **Cadence de recensement JAMAIS sous 60 s.** À 17 s, l'instrument devient
  lui-même la cause : un effondrement a été déclenché à 38 s au lieu de ~300 s
  (effet d'observateur, condition X4).
- ⛔ **Ne JAMAIS verser le contenu lu dans le dépôt** (condition S11) : l'adresse,
  la taille et l'identification — jamais les octets.
- Le build de diagnostic doit être archivé avec le suffixe `-diag`
  (`tools/archive_build.py`) pour ne pas être attaché à une release. Le suffixe
  seul ne suffit pas : c'est `check_release_artefacts.sh` qui mord.
- Après la fenêtre, **redéployer un binaire de production** et vérifier que
  `/debug/heap_peek` rend bien 404.

## Fichiers potentiellement concernés

Inconnus — c'est l'objet. `tools/compare_census.py` sert à la comparaison.

## Livrables — check-list
- [ ] Build de diagnostic, archivé `-diag`, déployé
- [ ] Redémarrage + recensement toutes les 20 min sur les 8 premières heures
- [ ] Attribution de l'allocation de 32 o et de celle de ~6 Ko allouée/libérée
- [ ] Note d'incident mise à jour (adresse, taille, identification — **jamais** les octets)
- [ ] Décision écrite : corriger ou non, et pourquoi
- [ ] Retour à un binaire de production, `/debug/heap_peek` → 404 vérifié

## ADR attendu ?
**Non** pour le diagnostic. Oui si un correctif structurant en découle.

## Éléments visibles (pour doc-writer)
Aucun. Note d'incident uniquement, sauf correctif.

## Critères d'acceptation
- [ ] Le bloc de 32 o à `0x3FFF74AC` (ou son équivalent après redémarrage) est **identifié**
- [ ] L'allocation d'environ 6 Ko allouée puis libérée vers 6,8 h est identifiée
- [ ] La décision de corriger ou non est écrite avec son motif
- [ ] Aucun octet de contenu mémoire dans le dépôt
- [ ] L'appareil est revenu sur un binaire de production

## Plan de test manuel
1. Déployer le build de diagnostic, **redémarrer**.
2. Recenser toutes les 20 min pendant 8 h (jamais sous 60 s).
3. Comparer les recensements avec `tools/compare_census.py` — il restreint à
   l'intersection des fenêtres, la fenêtre se déplaçant avec le plus gros trou.
4. Lire le contenu des candidats bordant le plus gros trou.
5. Redéployer la production, vérifier le 404.

## Notes pour agent

⚠️ **Rappel de proportion.** Ce lot récupérerait ~14 Ko de contiguïté sur un
système qui n'en manque pas, et qui est stable depuis plus de 40 h. Il coûte un
build de diagnostic, un déploiement, un redémarrage et une nuit d'observation.
**Il est légitime de ne jamais l'ouvrir.** Il existe pour que le diagnostic à
moitié fait ne soit pas perdu, pas pour être fait.

⚠️ Le redémarrage interrompt la filtration et la régulation le temps du reboot :
le choisir à une heure sans dosage en cours.

## Hors périmètre
- Toute optimisation mémoire par ailleurs (feature-130, 131, 132).
- Tout correctif : cette spec **diagnostique**. Un correctif ferait l'objet d'une
  spec de suivi, avec ses propres agents.


---

## 🔴 FENÊTRE DE MESURE OUVERTE — capture installée le 2026-10-08 à 19:47

### La fenêtre est ouverte MAINTENANT, et elle se referme vers 03:30 locale
L'appareil a redémarré le **2026-10-08 à 20:06 locale** (déploiement v2.60.0). La spec est
formelle : *« un recensement pris tardivement ne montrera JAMAIS rien. Il faut redémarrer
l'appareil et observer les 7,4 premières heures. Ce n'est pas une commodité, c'est une condition
de la mesure. »*

### ⚡ Le mécanisme est EN TRAIN D'AGIR — mesuré
| Moment | uptime | `maxBloc8` |
|---|---|---|
| au démarrage | 0,01 h | **55 736** |
| | 1,67 h | **50 816** |
| | 1,68 h | **46 856** |

**Près de 9 000 octets de contiguïté perdus en moins de deux heures**, alors que le palier
précédent (26 612) était resté figé toute la journée. C'est exactement le régime transitoire que
la spec décrit, et c'est la première fois qu'on l'observe depuis son origine.

⚠️ Les deux derniers relevés sont séparés de quelques minutes : **une partie de l'écart pourrait
venir de mes propres requêtes** (condition X4, effet d'observateur — à 17 s de cadence
l'instrument est devenu la cause lors d'un épisode antérieur). Relevés manuels **arrêtés** ; la
série de référence est celle du cron ci-dessous.

### Capture installée et VÉRIFIÉE
`crontab` : `*/20 * * * * /root/pool-captures/capture-logs.sh`
- cadence **20 min** — jamais sous 60 s (condition X4) ;
- `flock -n` partagé avec les passages 04:00 / 05:30 UTC existants : **aucune collision** — le
  recensement est une machine à instantané **unique**, deux lectures simultanées se détruisent ;
- capture `census_free_*.json` **ET** `census_used_*.json` + journal + `sysinfo` ;
- **éprouvée** : exécution manuelle à 19:47, quatre fichiers produits, recensement lisible.

⚠️ **CETTE BANDE FINE EST À RETIRER APRÈS LA FENÊTRE.** Elle n'a d'intérêt qu'au démarrage, et
laissée en place elle userait le journal de l'appareil pour rien.

### Ce que cette capture donnera — et ce qu'elle NE donnera PAS

**Elle donnera** : la **géométrie** (adresses, tailles, trous), l'**instant** où chaque bloc de
longue durée se pose, et la marche de **+6 144** (allocation puis libération vers 6,8 h) que la
spec désigne comme *« le fil le plus prometteur »*.

**Elle ne donnera PAS l'identification** des blocs : lire leur **contenu** exige
`/debug/heap_peek`, **délibérément absent des binaires de production** (ADR-0081, condition S12,
gardé par `tools/check_no_peek_flag.sh`).

### ⛔ Le build de diagnostic entre en CONFLIT avec le calendrier en cours
Déployer un binaire de diagnostic supposerait de **redémarrer l'appareil**, donc :
1. **fermer la fenêtre actuelle** sans l'avoir exploitée — elle ne rouvrira qu'au redémarrage
   suivant ;
2. mettre un binaire **hors production** sur l'appareil pendant 8 h et plus ;
3. interférer avec la **surveillance de feature-136**, ouverte depuis le 2026-10-08 à 20:06 pour
   3 jours pleins, et dont le gate `pool-chemistry` exige qu'elle soit **strictement sérielle**
   avec celle de bug-115.

➜ **Décision à prendre par l'utilisateur**, pas par un agent. La séquence raisonnable :
exploiter **d'abord** la fenêtre de ce soir avec le binaire de production ; décider **ensuite**,
sur les données obtenues, si l'identification du contenu vaut un build de diagnostic — et le
placer **après** les surveillances de feature-136 et bug-115, c'est-à-dire **pas avant le
2026-10-19**.

### Livrables — état réel
- [x] Fenêtre identifiée comme **ouverte**, capture installée et éprouvée
- [ ] Recensements des 8 premières heures — **en cours de collecte**
- [ ] Attribution de l'allocation de 32 o et de celle de ~6 Ko allouée/libérée
      ⚠️ **Partiellement hors de portée sans build de diagnostic** (contenu non lisible)
- [ ] Décision écrite : corriger ou non
- [ ] Retrait de la bande fine `*/20` du `crontab`
