## Métadonnées
- **ID** : feature-105
- **Slug** : hygiene-numerotation-et-couverture-reelle
- **Type** : feature (outillage / tests)
- **Statut** : done — tests verts, revue `code-reviewer` **Approved**, `pool-chemistry` OK
- **Priorité** : normale
- **Version cible** : `N/A` — aucun changement de firmware, aucun bump de version
- **Dernière mise à jour** : 2026-09-06
- **Spec(s) liée(s)** : `bug-106` (défaut réel trouvé ici, **non corrigé** — voir Notes), `feature-103` (gardes CI — dont la règle 2 a révélé l'ambiguïté de numéro), `feature-102`, ADR-0017 (logique pure testable)

## Domaines touchés
- [x] Régulation (`pulse_dosing_logic`) → **`pool-chemistry` requis** : une branche
      *fail-closed* du dosage est concernée. Aucun code métier n'est modifié — seuls
      des tests sont ajoutés — mais ces tests **figent une sémantique de sécurité**.
- [ ] Capteurs — [ ] Filtration / Éclairage — [ ] Web — [ ] MQTT — [ ] OTA — [ ] Frontend

## Agents requis
- [x] spec-reader — [ ] architect — [x] pool-chemistry — [x] test-engineer
- [x] code-reviewer — [x] doc-writer
- [ ] embedded-developer — aucun fichier de logique métier n'est modifié

## Description
Refermer les trois constats laissés ouverts par `feature-102` et `feature-103` :
l'ambiguïté du numéro `feature-067`, la couverture de tests mesurée sur deux modules
seulement, et les branches non couvertes que cet élargissement révèle.

## Contexte
`tools/check_specs.sh` (feature-103) a signalé dès sa première exécution que
**`feature-067` désignait deux specs** : `todo/feature-067-etude-portage-esp32-s3.md`
et `done/feature-067-offset-mesure-ph.md`. Toute référence « feature-067 » dans un
commit, un ADR ou une conversation était ambiguë.

Surtout, la CI mesurait la couverture avec un **filtre nommant deux modules** —
`sensor_filter` et `dosing_logic`. Un module pur ajouté sans test n'apparaissait donc
nulle part et ne faisait pas bouger le chiffre : **la mesure ne surveillait que ce
qu'on avait pensé à lui nommer**. C'est le même angle mort que celui de la CI qui ne
compilait aucun firmware (feature-103) : un contrôle dont le périmètre est une liste
manuelle finit par mentir par omission.

Mesure au 2026-09-06 avec le filtre élargi à `src/` : **34 modules**, lignes **99,8 %**
(1806/1810), fonctions 100 %, branches 86,5 %. Le chiffre est excellent — mais les
**4 lignes manquantes étaient invisibles**, et l'une d'elles est une garde de sécurité
chimique.

## Comportement attendu
Aucun changement de comportement du firmware. Trois effets :
1. Un numéro de spec ne désigne plus qu'une seule spec.
2. La couverture porte sur **tout** module compilé par `[env:native]`, sans liste à
   tenir à jour, en local comme en CI (mêmes filtres des deux côtés).
3. Les 3 emplacements non couverts sont testés — ou explicitement écartés avec motif.

## Comportement actuel
- `feature-067` porte deux slugs (**corrigé par A1**).
- Filtres de couverture nommant 2 modules en CI, 4 dans `tools/coverage.sh` : les deux
  périmètres **divergeaient déjà entre eux** (**corrigé par A2**).
- 4 lignes non couvertes, jamais vues.

## Les 3 emplacements non couverts
| Emplacement | Nature | Verdict attendu |
|---|---|---|
| `src/pulse_dosing_logic.cpp:80` | **Garde *fail-closed* du dosage** : `if (!isfinite(vPulseMl) \|\| vPulseMl <= 0.0f) return {true, 0, false, 0.0f}` — référence de volume incalculable ⇒ on refuse de tirer | **À TESTER** — c'est une garde de sécurité chimique, et une garde non testée n'est pas une garde |
| `src/time_to_target_logic.cpp:289-290` | Compteur de filtration **en recul** ⇒ la tenue est relancée. Cas réel : remise à zéro du compteur, repliement, restauration de configuration | **À TESTER** — cas limite atteignable en production |
| `src/measure_validity_logic.cpp:207` | `default:` d'un `switch` sur énumération exhaustive, retourne « inconnue » | **À arbitrer par `test-engineer`** : branche défensive probablement inatteignable sans transtypage artificiel. L'écarter avec motif est une réponse acceptable — en fabriquer un test factice ne l'est pas |

## Contraintes techniques
- **La validation par mutation est ICI applicable et OBLIGATOIRE** — contrairement au
  cycle `feature-102`, où le code retiré n'était jamais compilé. Ces trois modules sont
  compilés par `[env:native]` : tout test neuf doit être vérifié en cassant
  volontairement le code qu'il protège, et en constatant qu'il meurt.
- Aucune modification de la logique métier : **on ajoute des tests, on ne corrige rien**.
  Si un test révèle un défaut réel, il ne se corrige PAS dans ce cycle — il devient une
  spec.
- Les filtres `gcovr` de `tools/coverage.sh` et de `tests-natifs.yml` doivent rester
  **identiques**, sans quoi le chiffre local et le chiffre CI divergent — et c'est le
  local qu'on croit.
- Conteneur LXC sans port série.

## Livrables — check-list
- [x] A1 — renuméroter `feature-067-etude-portage-esp32-s3` → **`feature-104`**, avec une
      bannière expliquant le motif. C'est l'étude qui change de numéro, pas la spec close :
      rien ne la référençait, alors que `feature-067-offset-mesure-ph` est citée par
      `CHANGELOG.md`, `data/app.js`, `src/main.cpp`, `src/constants.h` et
      `src/web_routes_debug.cpp`. Renuméroter la spec close aurait réécrit de l'historique.
- [x] A2 — élargir le filtre `gcovr` à `src/` dans `.github/workflows/tests-natifs.yml`
      **et** dans `tools/coverage.sh` (parité local / CI). Seuil `--fail-under-line 90`
      conservé : le réel est à 99,8 %.
- [x] A3 — test de la garde *fail-closed* de `pulse_dosing_logic.cpp:80`
- [x] A4 — test du compteur de filtration en recul, `time_to_target_logic.cpp:289-290`
- [x] A5 — arbitrer `measure_validity_logic.cpp:207` : test ou motif d'exclusion écrit
- [x] A6 — **valider chaque test neuf par MUTATION** et consigner quel mutant chaque test tue
- [x] A7 — `pio test -e native` : aucune régression sur les 1286 cas existants (**1291** après ajout)
- [x] C1 — `docs/BUILD.md` § CI : le périmètre de couverture n'est plus une liste nommée
- [x] C2 — consigner la couverture de référence dans `docs/BUILD.md` § Couverture de tests :
      **100 % lignes (1810/1810)**, 100 % fonctions (161/161), **86,8 % branches**, 34 modules,
      1291 cas — au 2026-09-06. L'avant-cycle (99,8 % / 86,5 %, sur 2 modules) y est conservé
      comme point de comparaison
- [x] C3 — **confirmé : pas d'entrée `CHANGELOG.md`** — voir la justification dans les Notes

## Sécurité chimique
- Dosage impliqué : **oui, indirectement** — un test fige la sémantique de la garde
  *fail-closed* de `pulse_dosing_logic`. Aucune limite, aucun seuil, aucun PID modifié.
- Risque identifié : qu'un test **entérine un comportement erroné**. `pool-chemistry` doit
  valider que « référence de volume incalculable ⇒ refus de tirer » est bien la sémantique
  voulue, et que le tuple retourné `{true, 0, false, 0.0f}` correspond à un refus complet.

## ADR attendu ?
- **Probable** : non — aucune contrainte structurante, aucun verrouillage.

## Critères d'acceptation
- [x] `./tools/check_specs.sh` → 0, **sans avertissement de numéro ambigu** (`149 spec(s), aucun doublon`)
- [x] Couverture ≥ 99,8 % de lignes, et les emplacements traités passent à couverts → **100 % (1810/1810)**
- [x] Chaque test neuf **tue un mutant** identifié, consigné dans les notes
- [x] `pio test -e native` : 1286 + 5 = **1291 cas**, aucun échec
- [x] Filtres `gcovr` identiques entre `tools/coverage.sh` et `tests-natifs.yml` (`--filter 'src/'`, seuil `--fail-under-line 90` des deux côtés)
- [x] Aucune modification de logique métier — `src/` **intact**, seuls `test/`, `tools/coverage.sh`, `.github/workflows/tests-natifs.yml` et `docs/` sont touchés

## Hors périmètre — LIMITE ASSUMÉE ET DATÉE (2026-09-06)
> **Statut de ce paragraphe** : ce n'est **pas** un constat encore ouvert, c'est une
> **limite arrêtée le 2026-09-06**, confirmée par `pool-chemistry` (recommandation
> **non bloquante**) et par `code-reviewer` (**Approved**) sur ce cycle. Elle ne se
> rouvre que par une spec dédiée. Reportée dans `docs/BUILD.md` § Couverture de tests.

**Le chemin d'échec de `ledcAttachChannel`** (`pump_controller.cpp:213`, qui pose
`pumpUsable[i] = false`) reste **non testé, et c'est un choix**. Le tester exigerait
d'extraire une indirection de `pump_controller.cpp` — fichier de sécurité chimique — au
seul bénéfice d'une branche de deux lignes déjà relue par `pool-chemistry` **et** par
`code-reviewer` lors de `feature-102`. Introduire un point de couture dans ce fichier
coûte plus de risque que le test n'en couvre. `pool-chemistry` l'avait d'ailleurs
qualifié de recommandation non bloquante.

Ce constat cesse donc d'être « ouvert » : il devient une **limite assumée, datée du
2026-09-06 et confirmée par `pool-chemistry`**. S'il devait être levé un jour, la voie
serait l'extraction d'un module pur au sens de l'ADR-0017, dans une spec dédiée — pas un
ajout de test opportuniste.

De même, les **branches — 86,5 % avant le cycle, 86,8 % après** — ne sont pas un objectif
de ce cycle : aucun seuil de branches n'est ajouté à la CI, l'élargissement du filtre
suffisant à rendre le chiffre visible. Ce sont des demi-branches défensives, plusieurs
inatteignables par entrée réaliste (motifs écrits dans les tests). Le relever serait une
décision séparée.


---

## Notes d'implémentation (2026-09-06)

### Fichiers touchés
- `specs/features/todo/feature-067-etude-portage-esp32-s3.md` → `feature-104-…` (`git mv` + bannière de motif)
- `.github/workflows/tests-natifs.yml` — filtre `gcovr` élargi à `src/`, commentaire de motif + chiffres à jour
- `tools/coverage.sh` — filtre élargi à `src/` (parité CI) + garde `--fail-under-line 90`
- `test/test_native_dosing/test_dosing_decision.cpp` — A3 (2 cas)
- `test/test_native_time_to_target/test_time_to_target.cpp` — A4 (2 cas)
- `test/test_native_measure_validity/test_measure_validity.cpp` — A5 (1 cas)
- `docs/BUILD.md` — C1 + C2
- **`src/` : intact.** Aucun octet de logique métier modifié.

### Résultats
- `pio test -e native` : **1291 cas**, 0 échec (1286 avant).
- Couverture (`--filter 'src/'`, 34 modules) : **lignes 100 % (1810/1810)**,
  **fonctions 100 % (161/161)**, **branches 86,8 %**. Avant : 99,8 % / 86,5 % sur 2 modules.

### A6 — les 3 mutants tués, et pourquoi chacun meurt

| # | Mutation appliquée | Meurt sur | Pourquoi |
|---|---|---|---|
| M1 | `pulse_dosing_logic.cpp` — **garde supprimée** (`if (!isfinite(vPulseMl) \|\| vPulseMl <= 0.0f)` retiré) | `refused` et `pulseMs` | sans la garde, la fonction poursuit avec une référence de volume nulle : elle ne refuse plus, donc `refused == false` |
| M2 | `time_to_target_logic.cpp` — **garde de recul supprimée** (`if (in.filtTotalMin < st.holdStartFiltMin)` retiré) | tenue déclarée atteinte immédiatement après le recul | la soustraction `filtTotalMin - holdStartFiltMin` se fait en **non signé** : elle sous-déborde en une valeur énorme, franchit `kTimeToTargetHoldMin` d'un coup, et `nReached` passe à 1 alors que le test exige **30 minutes pleines depuis le recul** |
| M3 | `measure_validity_logic.cpp` — `default:` rendant `nullptr` (variante : chaîne vide) | `TEST_ASSERT_NOT_NULL` puis `strlen(l) > 0` | l'appelant fait `String(outageDenyLabel(v.deny))` dans un chemin de journalisation : un retour nul ou vide y produit une ligne de log **muette sur le motif du refus** |

### Complémentarité des mutants (analyse `code-reviewer`)
M1 est un mutant **grossier** : il tombe sur `refused`/`pulseMs` et **ne teste pas**
l'assertion sur `carryMl`. Le mutant qui la teste est plus **fin** :
`return {true, 0, false, carryMl}` — un refus qui **conserverait la dette** au lieu de
l'écraser. C'est *cette* assertion (`carryMl == 0`) qui le tue, et elle seule : le refus
est correct, seul le devenir de l'accumulateur diffère. C'est le point le plus facile à
rater, car un refus qui garde la dette produirait une **rafale de rattrapage** au retour à
la normale. Les deux mutants sont donc **complémentaires** : ni l'un ni l'autre ne rend
l'autre superflu.

### Nuances consignées dans les tests eux-mêmes
1. **La garde de dosage testée est morte en production aujourd'hui** :
   `kPulseKickMs = 400 ms` < `kPulseMinMs = 10 000 ms`, la branche est inatteignable. Le
   test est un **filet contre un futur changement de constante** — exactement la classe de
   défaut déjà rencontrée par ce projet (propagation d'une constante de pouls, trouvée par
   mutation).
2. **Le sous-terme `!isfinite(vPulseMl)` reste volontairement non couvert** :
   `pulseDeltaVolumeMl` assainit déjà NaN/±inf en 0 en amont ; il faudrait un
   `flow255 ≈ 1e30` pour faire déborder le produit. Une telle sentinelle ne prouverait que
   l'**exécution** de la ligne, pas un comportement que l'appareil puisse produire.
3. **`OutageDeny` a un type sous-jacent fixé (`: uint8_t`)** : le domaine réel de
   `outageDenyLabel` est [0, 255], toute valeur y est licite — tester `default:` n'est donc
   **pas** un transtypage artificiel. Mais le test **n'attrape pas** l'ajout futur d'un
   motif sans `case` : il tomberait dans `default:` et serait journalisé « inconnue » sans
   échec. C'est le tableau `all[]` de `test_outage_deny_labels` qu'il faudrait étendre.

### Nits `code-reviewer` corrigés (3)
1. **Chiffres périmés** dans le commentaire de couverture de `tests-natifs.yml` (il citait
   la mesure d'avant les tests neufs) → remplacés par 100 % / 161 fonctions / 86,8 %.
2. **Assertion tautologique retirée** d'un test de `time_to_target` : vraie par
   construction du harnais, elle ne pouvait pas échouer. Une assertion qui ne peut pas
   mourir n'est pas une assertion — le travail est fait par celle qui la précède.
3. **Garde de seuil manquante en local** : `tools/coverage.sh` affichait un chiffre sans
   jamais échouer → `--fail-under-line 90` ajouté, identique à la CI, code de sortie rendu
   **après** production du rapport HTML.

### Défaut réel trouvé et NON corrigé → `bug-106`
En écrivant le test A4, `test-engineer` a trouvé un défaut **voisin** de la branche
testée, pas dedans : `time_to_target_logic.cpp` l. **295-297**. Le ternaire
`holdStartFiltMin >= t0FiltMin` protège correctement du sous-débordement non signé, mais
il rend **0** — valeur **déjà légitime** de l'indicateur (« la cible était atteinte dès le
départ »). Une journée dont le compteur a reculé devient donc **indiscernable** d'une
journée atteinte d'emblée, une fois archivée.

**Pourquoi il n'est pas corrigé ici** : la contrainte du cycle est explicite — *on ajoute
des tests, on ne corrige rien*. Surtout, **aucune assertion n'a été posée sur ce
comportement**, délibérément : un test dessus l'aurait **entériné** comme voulu. Le défaut
part donc en spec : `specs/features/todo/bug-106-journee-archivee-a-zero-sur-compteur-en-recul.md`
(priorité basse — indicateur informatif, aucun chemin de dosage : `pump_controller.cpp`
n'inclut pas `time_to_target*`, flux à sens unique en lecture seule, constat
`pool-chemistry` du 2026-09-06).

### C3 — pourquoi aucune entrée `CHANGELOG.md`
Décision : **non**, et c'est un choix, pas un oubli. Le `CHANGELOG.md` de ce projet
consigne ce qui change **pour l'appareil et son utilisateur** (sections *Firmware*,
*Fonctionnalités*, *Hardware*, *Documentation* d'un livrable versionné). Or ce cycle :
`src/` intact, aucun bump de `src/version.h` (reste `2.42.1`), aucun binaire à flasher,
aucun comportement observable modifié — ni endpoint, ni topic, ni message WS, ni log, ni
écran. Ce qui a changé est l'**outillage de vérification** (`test/`, `tools/`, CI) et la
documentation d'ingénierie. Y ajouter une entrée diluerait le signal que le fichier porte
pour l'utilisateur. La trace du cycle vit dans cette spec, dans `docs/BUILD.md` et dans le
message de commit.
