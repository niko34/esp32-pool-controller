# Matériel — point d'entrée

Sources KiCad du PCB du contrôleur de piscine.

⚠️ **Les sources de ce dossier sont GELÉES sauf demande explicite** (règle 5 de
[`CLAUDE.md`](../CLAUDE.md)) : on ne modifie ni le schéma ni le routage au fil
d'une tâche logicielle.

## Fichiers

| Fichier | Rôle |
|---|---|
| `esp32-pool-controller.kicad_sch` · `.kicad_pcb` · `.kicad_pro` | Sources, **versionnées** |
| `SymLib.kicad_sym` · `sym-lib-table` | Bibliothèque de symboles du projet |
| `BOM.xlsx` | **Nomenclature de référence** — ⛔ toute correction se fait ICI |

⛔ **`BOM.csv` est un DÉRIVÉ**, généré par `python3 tools/bom_export.py` depuis le
`.xlsx`, donc ignoré par git. ⚠️ **Ne jamais le corriger à la main** : la prochaine
exécution du script l'écrase.

Les fichiers de fabrication (gerbers, perçage, zips) ne sont **pas** versionnés :
ils se régénèrent avec `./scripts/build-gerbers.sh` et deviennent les assets d'une
release `pcb-vX` via `/release-pcb`.

---

## 🔴 Deux défauts MATÉRIELS non résolus — à instruire avant toute révision du PCB

**Ils ont été sortis du backlog logiciel le 2026-10-17**, sur décision de
l'utilisateur : *« ces investigations sont du ressort de l'humain qui conçoit le
PCB »*. ⛔ **Clôturées ne veut pas dire résolues** — elles attendent une personne
avec des instruments, pas une session de travail.

### 1. Perturbation de mode commun sur les électrodes pH/ORP
[`specs/features/done/feature-137-perturbation-de-mode-commun-sur-les-electrodes.md`](../specs/features/done/feature-137-perturbation-de-mode-commun-sur-les-electrodes.md)

Le pH brut est **anti-corrélé et presque à deux niveaux**, et **la voie ORP voit la
même perturbation** : les deux électrodes subissent un **mode commun**. Mécanisme
suspecté : couplage électrique via le verre (~100 MΩ) avec les **courants de fuite
de la pompe de filtration** — mesure nettement meilleure sur batterie.

⚠️ **Ce que ça coûte aujourd'hui** : le 2026-10-16, l'eau réelle était à **pH 8**
et la sonde lisait **6,9**. Une correction de **+1,00** a dû être posée — **le
double de l'ancienne borne**, retirée pour la rendre possible (ADR-0114, v2.82.0).
⛔ **Le logiciel ne corrige pas ce défaut, il le COMPENSE** — sur la grandeur même
qui décide des injections d'acide.

⇒ C'est la **seule voie** qui assainirait le signal **à la source**.

### 2. Transitoire de reprise de filtration
[`specs/features/done/feature-069-investigation-materielle-sonde-ph.md`](../specs/features/done/feature-069-investigation-materielle-sonde-ph.md)

Trois matins consécutifs (02-04/09/2026), même signature : brut **normal** à la
remise en circulation, plongeon à **5,60-5,71** quelques minutes après, retour à la
normale en **30 à 48 min**, ⚠️ **l'ORP plongeant en parallèle** (−56 à −74 mV).

⛔ **Les cinq hypothèses restent ouvertes** (chambre désamorcée · bulles piégées ·
effet électrique au démarrage · eau réellement acide · sonde vieillissante).

⭐ **Le relevé qui tranche** : **deux bandelettes simultanées**, une au **bassin** et
une au **refoulement**, à **t+2** et **t+15 min** après la reprise. C'est le seul
qui sépare « l'eau est réellement acide » de « la sonde lit faux ». ⚠️ Tout le reste
en découle.
⛔ **Préalable de sécurité** : régulation pH en **manuel** pendant toute
manipulation de sonde.

### Lié, et toujours ouvert côté logiciel
[`bug-135`](../specs/features/todo/bug-135-butee-brute-ph-se-declenche-sur-une-lecture-isolee.md)
— la butée de sécurité brute se déclenche sur ces lectures isolées. ⚠️ **Même
cause** : tant que le mode commun est là, le logiciel doit composer avec un signal
qui alterne entre deux niveaux.

## Références
- [ADR-0012 — mapping GPIO du PCB v2](../docs/adr/0012-mapping-gpio-pcb-v2.md)
- [ADR-0097 — Le filtre lisse, le dosage juge](../docs/adr/0097-le-filtre-lisse-le-dosage-juge.md) — ce que le logiciel fait du bruit, faute de pouvoir le supprimer
- [feature-097 — Seuil de stabilité sur données terrain](../specs/features/done/feature-097-seuil-de-stabilite-sur-donnees-terrain.md) — la campagne qui **caractérise** le bruit (⛔ sans l'expliquer)
