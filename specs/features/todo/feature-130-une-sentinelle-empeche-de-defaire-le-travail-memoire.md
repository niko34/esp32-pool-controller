# feature-130 — Une sentinelle empêche de défaire le travail mémoire

## Métadonnées
- **ID** : feature-130
- **Slug** : une-sentinelle-empeche-de-defaire-le-travail-memoire
- **Type** : feature
- **Statut** : todo
- **Priorité** : haute
- **Version cible** : 2.53.0
- **Dernière mise à jour** : 2026-09-18
- **Spec(s) liée(s)** : `specs/features/done/feature-129-…` (l'acquis à protéger) · feature-131 (le prochain gros chantier mémoire)

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

> Aucun domaine fonctionnel : cette spec ne change **aucun comportement**. Elle
> ajoute de l'outillage et de la règle.

## Agents requis
- [x] spec-reader
- [x] architect
- [ ] pool-chemistry
- [ ] ux-ui-designer
- [ ] embedded-developer
- [ ] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description

Rendre **vérifiable par la CI** la discipline mémoire que feature-129 vient
d'établir, pour qu'une fonctionnalité future ne la défasse pas sans que personne
ne le voie.

## Contexte — le constat qui motive cette spec

Relevé du 18/09/2026. Voici **l'intégralité** de ce que `CLAUDE.md` impose
aujourd'hui en matière de mémoire (ligne 140) :

> « **Mémoire** : surveiller le heap, éviter allocations dynamiques récurrentes
> dans la loop »

Une ligne, **sans seuil, sans vérification, sans sanction**. Et `tools/` contient
**six** sentinelles — `check_doc_links`, `check_specs`, `check_fopen_guard`,
`check_no_peek_flag`, `check_heap_routes_guard`, `check_release_artefacts` —
dont **aucune ne porte sur la mémoire**.

Conséquence directe : le travail de feature-129 n'est protégé par rien. Un agent
qui ajoute demain un `std::vector<String>` dans le journal, ou un `JsonDocument`
de plus sur un chemin périodique, ne rencontrera **aucun garde-fou**. La CI sera
verte.

Ce n'est pas théorique : le projet a déjà reconduit deux fois des défauts de ce
type (le parachute anti-OOM en PSRAM, feature-066 ; la métrique heap fausse,
15/08/2026), et dans les deux cas la régression était **invisible** aux
contrôles existants.

## Comportement attendu

Trois cliquets, de force croissante. Ils sont **indépendants** : si l'un est jugé
trop coûteux ou trop bruyant, les deux autres valent d'être livrés seuls.

### 1. `tools/check_memory_discipline.sh`

Sur le modèle **exact** de `check_fopen_guard.sh` — même structure, même style de
sortie. Motifs à refuser dans `src/` :

- `String` **membre** d'une structure stockée en conteneur (le motif précis de
  feature-129 : un `String` par entrée d'un tableau de N entrées) ;
- `std::vector<String>` (le motif de `_persistBuffer`, retiré par construction) ;
- toute allocation dynamique sur le **chemin de journalisation** (`logger.cpp`,
  et la fonction `log()` en particulier).

⚠️ **Le message d'erreur est le livrable, pas le refus.** Les sentinelles
existantes expliquent *pourquoi* et renvoient à l'ADR — `check_doc_links.sh` dit
« Cause la plus fréquente : une spec a été PROMUE… », `check_fopen_guard.sh`
renvoie à ADR-0070. Une sentinelle qui dit seulement « interdit » se fait
contourner ; une qui explique se fait comprendre. Renvoyer à ADR-0082.

⚠️ **Prévoir un échappatoire documenté** (commentaire d'exemption avec motif
obligatoire, comme le fait `check_fopen_guard.sh` pour le helper unique) : une
interdiction sans porte de sortie légitime pousse à écrire du code tordu pour la
contourner, ce qui est pire que l'allocation d'origine.

### 2. Verrouiller le dimensionnement par `static_assert`

`kLogArenaBytes`, `kLogMessageMaxBytes` et `kLogPersistLineMax` portent déjà des
`static_assert` (dont `kLogMessageMaxBytes >= 640`, qui rend impossible le retour
à 256 — le défaut que `pool-chemistry` avait refusé). Étendre le principe : un
agent qui double l'arène doit **devoir s'en justifier** en touchant une assertion,
pas le faire en changeant un nombre.

### 3. Une section « Mémoire » dans `CLAUDE.md`

Remplacer la ligne unique par les trois règles réellement apprises :

1. **Le chemin de journalisation n'alloue pas.** Ni `String`, ni `new`, ni
   conteneur qui croît. C'est l'acquis de feature-129 (ADR-0082).
2. **Toute allocation de plus de quelques kilooctets sur un chemin périodique
   doit être justifiée, ou diffusée.** C'est l'objet de feature-131.
3. **La métrique qui compte est `max_alloc_8bit`, pas la mémoire libre.**

> ⚠️ La règle 3 mérite d'être écrite noir sur blanc : **le projet s'est déjà
> trompé dessus**. La mémoire libre peut monter pendant que la contiguïté
> s'effondre — c'est très exactement ce qui s'est produit pendant la fenêtre
> v2.51.0, et la cause racine du 15/08/2026 était une métrique heap qui mesurait
> la mauvaise chose. Une spec future qui « surveille le heap » sans préciser
> *lequel* ne surveille rien.

## Comportement actuel

Une ligne de consigne non vérifiable, zéro sentinelle mémoire. Une régression
passe la CI sans laisser de trace.

## Contraintes techniques

- La sentinelle doit être **lançable en local** avant commit, comme les trois
  jobs de `hygiene-specs.yml`, et s'y intégrer.
- **Zéro faux positif toléré** sur l'arbre actuel : une sentinelle qui crie pour
  rien est désarmée en une semaine. La valider en la lançant sur `main` **et**
  sur plusieurs commits antérieurs.
- **Prouver qu'elle mord** : la valider par mutation — réintroduire
  volontairement un `std::vector<String>` dans `logger.h` et vérifier qu'elle
  échoue. Une sentinelle qui ne refuse rien ne prouve rien (c'est la leçon du
  suffixe `-diag` de feature-128 : le suffixe seul ne faisait rien, seule
  l'exclusion dans `check_release_artefacts.sh` mordait).

## Fichiers potentiellement concernés

`tools/check_memory_discipline.sh` (créé) · `.github/workflows/hygiene-specs.yml` ·
`CLAUDE.md` · `src/constants.h` (assertions) · `docs/BUILD.md` (section CI)

## Livrables — check-list
- [ ] `tools/check_memory_discipline.sh`, avec message pédagogique et échappatoire
- [ ] Validation par mutation de la sentinelle (elle doit refuser un vrai motif)
- [ ] Zéro faux positif sur `main` et sur ≥ 3 commits antérieurs
- [ ] Intégration au workflow `hygiene-specs.yml`
- [ ] `static_assert` de dimensionnement complétés
- [ ] Section « Mémoire » dans `CLAUDE.md`
- [ ] `docs/BUILD.md` mis à jour (la CI compte désormais quatre contrôles d'hygiène)

## ADR attendu ?

**Non.** Aucun arbitrage : ADR-0082 porte déjà la décision de fond. Cette spec
l'outille.

## Éléments visibles (pour doc-writer)
Aucun endpoint, aucun champ WS, aucun topic MQTT. Aucun changement de
comportement utilisateur, donc **pas d'entrée CHANGELOG**.

## Critères d'acceptation
- [ ] `./tools/check_memory_discipline.sh` passe sur `main` sans aucun faux positif
- [ ] Elle **échoue** sur un `std::vector<String>` réintroduit dans `src/logger.h`
- [ ] Son message d'erreur explique le motif et renvoie à ADR-0082
- [ ] La CI la lance sur push et PR
- [ ] `CLAUDE.md` nomme `max_alloc_8bit` comme la métrique de référence

## Plan de test manuel
1. Lancer la sentinelle sur l'arbre propre → passe.
2. Ajouter `std::vector<String> tmp;` dans `src/logger.h` → elle échoue, avec un
   message compréhensible sans lire son code source.
3. Retirer la ligne → elle repasse.
4. Vérifier que l'échappatoire documenté fonctionne et exige un motif.

## Notes pour agent

Cette spec ne change aucun comportement : **`embedded-developer` n'est pas
requis**. Le risque n'est pas technique, il est ergonomique — une sentinelle
bruyante ou muette ne sert à rien. Soigner le message autant que la détection.

## Hors périmètre
- Toute optimisation mémoire (c'est feature-131).
- Toute modification de `logger.*` : l'acquis se protège, il ne se retouche pas.
