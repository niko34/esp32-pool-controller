# feature-137 — Étude de la perturbation de mode commun sur les électrodes pH/ORP

## Métadonnées
- **ID** : feature-137
- **Slug** : perturbation-de-mode-commun-sur-les-electrodes
- **Type** : **étude matérielle** — pas un lot logiciel
- **Statut** : **CLÔTURÉE le 2026-10-17** — ⛔ hors périmètre logiciel : étude **matérielle**, du ressort de celui qui conçoit le PCB. ⚠️ La perturbation reste **ENTIÈRE** et le logiciel la **compense** au lieu de la corriger (voir « Clôture »).
- **Priorité** : normale — aucune urgence, mais c'est la **seule voie** qui assainirait le signal à la source
- **Version cible** : N/A (matériel)
- **Dernière mise à jour** : 2026-09-20
- **Spec(s) liée(s)** : `specs/features/done/feature-097-…` (le bruit y est mesuré et caractérisé) · bug-135 (la butée brute, même cause) · ADR-0084 · `parametres-bassin-utilisateur`

## Domaines touchés
- [x] **Capteurs (pH / ORP / température)** — c'est le sujet
- [x] **Régulation** — indirectement : tout ce qui lit le brut en dépend
- [ ] Filtration / Éclairage — *sauf si la cause est la pompe, cf. ci-dessous*
- [ ] Web — routes HTTP / WebSocket
- [ ] MQTT / Home Assistant
- [ ] UART / écran LVGL
- [ ] OTA
- [ ] Authentification
- [ ] Historique / persistance
- [ ] Frontend (`data/`)

## Agents requis
- [x] spec-reader
- [x] architect — **pour l'instruction, pas pour concevoir du firmware**
- [x] **pool-chemistry** — toute modification touchant les électrodes ou leur masse
- [ ] ux-ui-designer
- [ ] embedded-developer — *a priori aucun code*
- [ ] web-ui-developer
- [ ] test-engineer
- [ ] code-reviewer
- [x] doc-writer

⚠️ **Les sources KiCad de `kicad/` sont GELÉES sauf demande explicite** (règle 5
de `CLAUDE.md`). Cette spec **instruit**, elle ne modifie ni schéma ni routage.

## Description

Le pH brut de ce bassin n'est pas du bruit de mesure ordinaire : c'est un signal
**anti-corrélé, presque à deux niveaux**, et **la voie ORP voit la même
perturbation**. Les deux électrodes sont donc affectées par un **mode commun**.

Corriger cette perturbation **à la source** est la seule voie qui rendrait le
signal réellement propre — et elle conditionne à la fois la qualité de la
régulation et la pertinence de deux autres specs.

## Contexte — les mesures, toutes datées du 2026-09-20

Fixture versionnée : [`test/fixtures/f097_purge_noise_20260920.csv`](../../test/fixtures/f097_purge_noise_20260920.csv)
— 300 échantillons **uniques** à 5 s (25 min), eau établie, porte de mesure
ouverte, firmware 2.53.0.

### La signature du signal

| Indice | Mesuré | Attendu si bruit blanc |
|---|---|---|
| autocorrélation au lag 1, pH | **−0,440** | 0 |
| autocorrélation au lag 1, ORP | **−0,396** | 0 |
| **corrélation instantanée pH ↔ ORP** | **+0,462** | 0 |
| saut moyen entre deux lectures (5 s) | 0,173 = **1,47 σ** | 1,128 σ |
| changements de signe autour de la médiane | **197 / 299** | ~150 |
| écart-type du pH brut | 0,1175 | — |

Bloc réel de la minute 22, tel quel :
`7.051 · 6.747 · 7.083 · 6.752 · 7.083 · 6.743 · 7.085 · 6.775 · 7.079 · 6.749 · 7.079 · 7.068`

Deux niveaux séparés d'environ **0,21**, en alternance rapide.

**L'alternance n'est pas verrouillée en phase** : les moyennes des échantillons
pairs et impairs sont identiques à 0,0002 près. Ce n'est donc **ni un bug de
firmware, ni un entrelacement du tampon** — c'est une perturbation physique.

### Ce que la corrélation pH↔ORP établit

Deux électrodes indépendantes, dans deux chambres différentes, avec deux
transmetteurs EZO distincts, voient **la même perturbation au même instant**
(+0,462), et chacune est anti-corrélée avec elle-même. C'est la signature d'un
**mode commun** — un potentiel parasite qui se superpose aux deux mesures par un
chemin partagé : la masse, l'eau, ou l'alimentation.

### L'hypothèse de travail du projet

Le projet attribue de longue date ce bruit aux **courants de fuite de la pompe de
FILTRATION**. Elle est cohérente avec tout ce qui précède, et avec le fait que
l'offset pH corrige un décalage de **mesure** et non une sonde défaillante.

⚠️ **Mais elle n'est pas prouvée.** C'est l'objet de cette spec.

### Ce qui écarte la sonde

`EZO pH slope : acide=101,0 % base=100,0 % zéro=−1,03 mV` (19/09). Les deux
électrodes sont **saines**. Le problème n'est pas dans la chimie de mesure.

## Pourquoi cette étude compte

Trois conséquences concrètes, toutes chiffrées ailleurs :

1. **feature-097** a dû remplacer la médiane par une moyenne interquartile
   **uniquement** parce que le signal est bimodal. Un signal propre rendrait ce
   correctif inutile — et permettrait de revenir à un plancher de purge de 10 min
   au lieu de 16 min 40 s : *« diviser σ par 2 rendrait l'horizon de 9 min de
   nouveau faisable »*.
2. **bug-135** existe parce qu'une butée de sûreté compare un seuil dur à une
   lecture isolée d'un signal qui balaye. Un signal propre la rendrait largement
   sans objet.
3. La **régulation** elle-même travaille sur une mesure dont la dispersion brute
   atteint 1,49 unité pendant une purge — avec tout ce que cela implique sur la
   confiance qu'on peut lui accorder.

## Ce que cette spec doit produire

**Une instruction, pas une modification.** À la fin, on doit savoir :

- [ ] **La cause est-elle bien la pompe de filtration ?** Le discriminant existe
      et il est simple : **relever une trace pompe à l'arrêt** et la comparer à
      une trace pompe en marche, sur la même eau. Si l'anti-corrélation et la
      corrélation pH↔ORP disparaissent, la cause est établie.
- [ ] **Par quel chemin** la perturbation arrive-t-elle : masse commune,
      couplage capacitif par l'eau, alimentation partagée, boucle de masse entre
      le coffret et la pompe ?
- [ ] **Quelles corrections sont envisageables**, avec leur coût et leur risque :
      mise à la masse de l'eau, isolation galvanique des transmetteurs, filtrage
      d'entrée EZO, séparation des alimentations, ferrites, reprise du routage.
- [ ] **Quelle amélioration attendre** de chacune, exprimée en **σ** — c'est la
      grandeur qui commande les seuils de feature-097 et bug-135.
- [ ] **Faut-il modifier le PCB ?** Si oui, cela rejoint `/release-pcb`, un cycle
      de fabrication, et une décision qui engage plusieurs mois.

## Protocole de mesure proposé

La trace du 20/09 est le **point de comparaison**, et elle est versionnée. Toute
nouvelle trace doit être prise **dans les mêmes conditions** pour être
comparable : eau établie, porte de mesure ouverte, 300 échantillons uniques.

```bash
# Capture — ⚠️ le paramètre est ?offset=N, PAS ?page=N
# (web_routes_debug.cpp — un ?page=N est IGNORÉ EN SILENCE et renvoie
#  six fois la même page : 50 échantillons présentés comme 300)
for o in 0 50 100 150 200 250; do
  curl -sS -u "admin:$POOL_PASSWORD" \
    "http://192.168.1.48/debug/sensor_trace?offset=${o}&count=50"
done
# Vérifier que le champ `offset` de la réponse a bien progressé avant de concaténer.
```

**Indices à recalculer sur chaque trace** : autocorrélation au lag 1 des deux
voies, corrélation instantanée pH↔ORP, écart-type, saut moyen entre lectures.

⚠️ **L'arrêt de la pompe de filtration ferme la porte de mesure** (les mesures
deviennent non représentatives) et **suspend le dosage**. La trace « pompe à
l'arrêt » doit donc être prise en connaissance de cause, sur une fenêtre courte,
et **jamais** en laissant l'eau stagner longtemps.

## Contraintes

- **Les sources KiCad sont gelées** : aucune modification de schéma ou de routage
  sans demande explicite de l'utilisateur.
- Toute intervention sur la masse ou l'alimentation d'un bassin touche à la
  **sécurité électrique**. Ce n'est pas un sujet de firmware.
- Le bassin est en **service** : les mesures doivent être non intrusives, et la
  régulation ne doit pas être suspendue longtemps.
- Ne rien conclure d'une trace unique : les deux traces (pompe en marche, pompe à
  l'arrêt) doivent être prises **le même jour**, à température comparable.

## Livrables — check-list
- [ ] M1 — Trace **pompe en marche** (référence : celle du 20/09 existe déjà)
- [ ] M2 — Trace **pompe à l'arrêt**, même jour, mêmes conditions
- [ ] M3 — Comparaison des quatre indices, et **verdict sur la cause**
- [ ] M4 — Si la pompe est confirmée : identification du **chemin** de couplage
- [ ] M5 — Options de correction, avec coût, risque et gain attendu **en σ**
- [ ] C1 — Consigner les traces et l'analyse ; référencer depuis feature-097 et bug-135
- [ ] C2 — ADR **seulement** si une décision matérielle structurante est prise

## Sécurité chimique
- Dosage impliqué : **non directement**, mais la qualité de la mesure conditionne
  toute la régulation.
- ⚠️ **L'arrêt de la filtration pour les besoins de la mesure suspend le
  dosage** et ferme la porte de mesure. Fenêtre courte, et jamais d'eau stagnante
  prolongée.
- Toute intervention sur la masse ou l'alimentation relève de la **sécurité
  électrique**, hors du périmètre de `pool-chemistry` comme de ce dépôt.

## ADR attendu ?
- **Seulement** si une décision matérielle structurante est prise (modification
  du PCB, isolation galvanique). L'étude seule n'en justifie pas.

## Éléments visibles (pour doc-writer)
- Aucun endpoint, champ, topic ni comportement logiciel.
- Les traces et l'analyse sont à consigner, et à **référencer** depuis
  feature-097 (qui explique pourquoi la médiane a dû être abandonnée) et bug-135.

## Critères d'acceptation
- [ ] Deux traces comparables existent, versionnées, avec leur date et leur méthode
- [ ] La cause est **établie ou écartée**, pas supposée
- [ ] Si établie : le chemin de couplage est identifié
- [ ] Les options de correction sont chiffrées en **σ**, pas en intentions

## Notes pour agent

⚠️ **Cette spec est une étude. Elle ne produit pas de firmware.** Résister à la
tentation d'ajouter un filtre logiciel : le projet en a déjà, et feature-097 a
montré qu'on peut passer des jours à choisir un estimateur parce que le signal
est mauvais. Ici on regarde **pourquoi** il est mauvais.

⚠️ **Vérifier la pagination avant d'analyser quoi que ce soit.** Une première
trace du 20/09 était invalide — le script envoyait `?page=N` au lieu de
`?offset=N`, paramètre **ignoré en silence**, et 50 échantillons uniques ont été
présentés comme 300. Les chiffres qui en découlaient ont porté une conception
complète, un verdict de sécurité chimique et une implémentation, **tous annulés**.
Le signal d'alarme était dans la réponse : le champ `offset` renvoyait 0 six fois.

⚠️ **Ne pas conclure d'une seule trace.** Le plancher de bruit n'est pas prouvé
stationnaire sur l'année — température, état de la pompe, vieillissement des
électrodes. C'est une réserve écrite dans feature-097, elle vaut ici aussi.

## Hors périmètre
- Tout correctif **logiciel** : c'est feature-097 (livrée) et bug-135.
- La modification effective du PCB : elle relève de `/release-pcb` et d'une
  décision de l'utilisateur, pas de cette étude.
- L'offset pH : il corrige un décalage de mesure, **il n'est pas en cause**.


---

## Clôture — 2026-10-17 : du ressort de CELUI QUI CONÇOIT LE PCB, pas du logiciel

**Décision de l'utilisateur**, ce jour : *« ces investigations sont du ressort de l'humain qui
conçoit le PCB »*.

⚠️ **La spec le déclarait déjà elle-même** : *« étude matérielle — pas un lot logiciel »*,
*« chantier hors firmware »*, `embedded-developer` **non coché** (*« a priori aucun code »*), et
l'architecte convoqué *« pour l'instruction, pas pour concevoir du firmware »*. ⇒ La clôture ne
change pas sa nature, elle **cesse de la faire attendre dans un backlog logiciel**.

### 🔴 CE QUI RESTE ENTIER — et ce que ça coûte tous les jours

La perturbation de **mode commun** sur les deux électrodes **n'est ni expliquée ni corrigée**.
⚠️ C'est elle qui rend le pH brut **anti-corrélé et presque à deux niveaux**, et c'est la **seule
voie** qui assainirait le signal **à la source**.

⚠️ **Conséquence concrète, mesurée le 2026-10-16** : l'eau réelle était à **pH 8**, la sonde
lisait **6,9** dans le circuit de filtration. ⇒ Une correction de mesure de **+1,00** a été posée
(v2.82.0, [[offset-ph-nature-et-non-suspicion]]) — ⚠️ **le double de l'ancienne borne de
l'appareil**, qui a dû être **retirée** pour la rendre possible (ADR-0114).

⛔ **Le logiciel ne corrige donc PAS le défaut : il le compense.** ⚠️ Et il le compense sur une
grandeur qui **décide des injections d'acide**. Tant que la cause est là, la justesse du dosage
repose sur l'exactitude d'une correction saisie à la main.

### Ce qu'il faudrait instruire, le jour où une révision du PCB est envisagée
⚠️ Les pistes sont dans le corps de la spec. ⛔ Elles ne sont **pas** départagées — la campagne de
bruit (feature-097) **caractérise** le phénomène, elle ne l'**explique** pas.
⚠️ Rappel : ⛔ **les sources KiCad sont GELÉES sauf demande explicite** (règle 5 de `CLAUDE.md`).

### Où le sujet continue de vivre
⇒ `kicad/README.md` (point d'entrée matériel) · [[feature-069]] (le transitoire de reprise,
clôturée le même jour pour la même raison) · bug-135 (la butée brute, **même cause**) ·
feature-097 (le bruit y est mesuré) · ADR-0084.
