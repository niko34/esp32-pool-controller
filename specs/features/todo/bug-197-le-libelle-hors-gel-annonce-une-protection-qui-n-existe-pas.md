## Métadonnées
- **ID** : bug-197
- **Slug** : le-libelle-hors-gel-annonce-une-protection-qui-n-existe-pas
- **Type** : bug
- **Statut** : todo
- **Priorité** : haute
- **Version cible** : à arrêter
- **Dernière mise à jour** : 2026-11-18
- **Spec(s) liée(s)** : [feature-195](../done/feature-195-langage-visuel-du-tableau-de-bord.md) (trouvé en vérifiant un point laissé ouvert par son étude)

## Domaines touchés
- [x] Filtration / Éclairage
- [x] Frontend (`data/`)

## Agents requis
- [x] spec-reader
- [x] architect
- [x] pool-chemistry — **l'eau qui gèle fait éclater les canalisations** ; arbitrage produit
- [ ] embedded-developer — selon l'option retenue
- [x] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description
La carte Filtration affiche la pastille **« Hors gel »**, qui laisse croire que l'appareil
protège l'installation du gel. **Il ne la protège pas.** Le libellé ne fait que *décrire*
une coïncidence : l'eau est sous 5 °C **et** la pompe se trouve déjà tourner.

## Contexte
Trouvé le **2026-11-18** en vérifiant un point que l'étude d'ergonomie de feature-195
avait laissé ouvert (« que fait l'arrêt forcé à la pause hors gel ? »). La réponse est
qu'il n'y a **rien à interrompre**.

**Vérifié dans le code, les deux côtés :**
- `data/app.js:5391` — `getFiltrationState()` : si `temperature < 5.0` **ET**
  `filtration_running`, la pastille devient « Hors gel ». C'est un **libellé calculé dans
  le navigateur**. Il ne commande rien, et il disparaît si la pompe s'arrête — c'est-à-dire
  **exactement quand le risque est maximal**.
- `src/filtration.cpp:281` — sous `kFiltrationRefTempMinC`, le firmware journalise
  « Hivernage » et **conserve le planning**. Aucun démarrage de pompe n'est commandé.
- Aucune autre logique de gel n'existe dans `src/` : les occurrences de « freeze » /
  « gel » concernent le **gel de l'intégrale du PID**, sans rapport.

⚠️ **Pourquoi c'est grave et pas cosmétique.** L'appareil est **vendu à des clients**.
Un propriétaire qui lit « Hors gel » comprend « l'appareil s'en occupe » et ne fait rien.
Si la pompe est à l'arrêt — mode Arrêt, Auto hors créneau, hivernage — l'eau immobile gèle
et **les canalisations éclatent**. Le coût est matériel et élevé, et le libellé y a
contribué en rassurant à tort.

## Comportement attendu
À trancher par `pool-chemistry` et l'utilisateur — **deux options, pas une**.

1. **Dire la vérité** (correctif minimal, sans firmware) : retirer ou reformuler le
   libellé pour qu'il décrive un **constat**, pas une protection. Par exemple « Eau très
   froide ». Et prévenir le propriétaire que la protection du gel est **à sa charge**.
2. **Faire ce que le libellé promet** (demande de fonction) : faire tourner la filtration
   sous un seuil de température, avec les gardes qui vont avec.

⛔ **Ce qui n'est pas acceptable, c'est l'état actuel** : un mot qui promet une protection
absente.

## Comportement actuel
Pastille « Hors gel » (ambre) quand `temperature < 5.0 && filtration_running`.
Aucun effet sur la commande de la pompe. Le libellé **s'efface** dès que la pompe s'arrête.

## Contraintes techniques
- ⛔ **Aucune solution ne doit exiger d'équipement ni de configuration chez le client.**
- L'option 2 touche la commande de la pompe ⇒ `pool-chemistry` obligatoire, et il faut
  statuer sur l'interaction avec le dosage (filtration forcée ⇒ dosage autorisé ?).
- L'option 1 est purement frontend.

## Fichiers potentiellement concernés
- `data/app.js` — `getFiltrationState()` (l. ~5381)
- `data/app.css` — l'exception `--warn` de la pastille (l. ~2499)
- `src/filtration.cpp` — seulement si l'option 2 est retenue
- `src/constants.h` — seuil éventuel
- `docs/user/src/guide-utilisation.html` et `guide-regulation.html`

## Livrables — check-list
- [ ] A1 — Arbitrage `pool-chemistry` + décision utilisateur entre les deux options
- [ ] B1 — Correctif retenu
- [ ] C1 — Guide utilisateur : dire clairement à qui incombe la protection du gel
- [ ] C2 — `CHANGELOG.md`
- [ ] D1 — Essai : eau sous 5 °C simulée (`ui_shot.sh -f temperature=3`), pompe en marche
      **et** à l'arrêt

## Sécurité chimique
- Dosage impliqué : **oui si l'option 2 est retenue** (une filtration forcée rouvre le
  dosage — à statuer)
- Risque identifié : ⚠️ **gel des canalisations** — risque matériel, pas chimique, mais
  c'est le plus coûteux que porte cette fiche.

## Critères d'acceptation
- [ ] Aucun libellé de l'interface n'annonce une protection que l'appareil n'assure pas
- [ ] Le cas « eau froide **et** pompe arrêtée » — le plus dangereux — est traité, alors
      qu'aujourd'hui il n'affiche **rien**
- [ ] Le guide utilisateur dit à qui incombe la protection du gel

## Notes pour agent
- ⚠️ **feature-195 touche DÉJÀ la couleur de cette pastille** : sa puce passe d'ambre à
  **grise**, texte et logique inchangés. Motif écrit dans cette fiche : l'ambre
  d'aujourd'hui ne s'allume que quand la pompe **tourne** — donc dans le cas favorable.
  ⇒ Ne pas lire ce changement comme un traitement du présent défaut : il reste entier.
- ⚠️ Le libellé actuel disparaît **précisément dans le cas dangereux** (pompe arrêtée).
  C'est l'inverse du comportement attendu d'une alerte : traiter ce cas d'abord.
- ⚠️ **Ne pas confondre** avec le « gel d'intégrale » du PID (`kPhIntegralFreezeDailyFraction`),
  qui n'a aucun rapport.
