# Passation — état du projet au 06/09/2026

Écrit à l'occasion d'une **bascule de poste de travail** (Mac → conteneur LXC).
Il dit **où en est chaque chantier**, **ce qui ne suit pas le dépôt**, et les
quelques leçons de méthode qui n'ont leur place dans aucune spec.

À lire une fois, au démarrage d'une session sur une machine neuve.

---

## 1. Ce qui ne suit PAS le dépôt

| Élément | Où | Conséquence |
|---|---|---|
| **Mot de passe de l'appareil** | nulle part dans git (délibéré) | à fournir à la session ; sans lui, aucun accès au contrôleur |
| **Adresse de l'appareil** | nulle part dans git | `poolcontroller.local` en mDNS, ou l'IP du réseau local |
| **`builds/`** — 2,6 Go, 132 ELF | local, ignoré | seul l'ELF de la **v2.42.0** est publié (asset de sa release) |
| **`.claude/settings.local.json`** | local (chemins absolus) | permissions à reconfirmer, travail plus verbeux |
| **Sauvegarde d'historique du 05/09** | restée **sur le Mac** (`builds/` est local et ignoré) | 245 points ; 145 seulement avaient pu être réimportés — plafond **corrigé en v2.48.0** (`bug-101` clos, [ADR-0075](adr/0075-decoupage-client-contre-streaming-firmware.md)). Les **100 points du 23 au 31 août** ne sont récupérables que tant que l'ancien dépôt local du Mac n'est pas nettoyé |

⚠️ **Depuis un conteneur, l'appareil n'est joignable que si le réseau atteint le
LAN.** Le déploiement OTA et tout relevé de diagnostic en dépendent.

⚠️ Le mot de passe **contient un `$`** : en shell, guillemets **simples**
obligatoires (`-u 'admin:…'`), sinon l'interpolation le mange et l'appareil
répond `401` sans autre explication.

---

## 2. État des chantiers

### ⚠️ Livré depuis le conteneur le 06/09 après-midi — POSTÉRIEUR à ce document

Cette passation a été écrite à **14:46**. Le conteneur travaillait déjà, et les deux
copies de `main` ont divergé deux heures sans se voir (fusion `85904c5`, sans conflit).
Ce qui suit n'était donc pas connu au moment de la rédaction :

- **`feature-102` / [ADR-0069](adr/0069-retrait-repli-core-2.md) — retrait du repli
  core 2** (v2.42.1). Il n'était plus compilable depuis la v2.40.0, plus flashable
  depuis que le bootloader est en core 3, et aucun binaire de secours n'avait jamais
  été archivé. Un seul environnement matériel subsiste. Voir §4, la leçon corrigée.
- **`feature-103` — deux gardes CI** : `build-firmware.yml` (la compilation devient un
  *gate* — c'est l'absence de cette garde qui avait laissé passer la casse du core 2
  pendant deux versions) et `hygiene-specs.yml` + `tools/check_specs.sh` (un nom de spec
  ne peut plus vivre dans deux dossiers). ⚠️ **Ces workflows n'ont jamais tourné sur un
  runner GitHub** : le premier push sera un rodage, et le build firmware y téléchargera
  ~2 Go de toolchain avant de peupler son cache.
- **`feature-105` — couverture réelle** : le filtre `gcovr` ne nommait que 2 modules sur
  34, donc ne surveillait que ce qu'on avait pensé à lui nommer. Élargi à tout `src/`,
  en CI **et** en local, avec la même garde de seuil. 5 tests neufs validés par mutation,
  **1291 cas**, lignes à **100 %**. `feature-067` renumérotée en **`feature-104`** (le
  numéro désignait deux specs).

Trois entrées neuves dans `todo/`, par ordre d'intérêt :

- **`feature-107`** — le refus de plan de dosage est **totalement silencieux** : ni
  journal, ni code de blocage, ni topic. La régulation peut s'arrêter sans laisser de
  trace. Son voisin, le refus de *gate* (pompe non caractérisée — cas bien plus probable),
  journalise mais reste invisible à l'écran et en MQTT. La tuyauterie existe pourtant
  déjà : il s'agit de l'**appeler**, pas de la construire.
- **`bug-106`** — sur recul du compteur de filtration, la journée s'archive à
  `minutes = 0`, indistinguable d'un « déjà à la cible ». Indicateur informatif, aucune
  incidence dosage. Trouvé en écrivant les tests de `feature-105`, **délibérément non
  corrigé** : poser une assertion dessus l'aurait entériné.
- **`feature-104`** — ex-`feature-067`, l'étude de portage ESP32-S3. Inchangée au fond.

### En cours (`specs/features/doing/`) — 2 seulement

**`bug-080` — redémarrages PANIC et fragmentation.** Lots A, B-1, B-2, B-3
livrés (v2.33.0 → v2.41.0). Restent deux critères :

- **origine de la fragmentation** — la sonde du lot B-2 a parlé le 06/09
  (`caps=0x080C`, DMA, 675 µs) mais a confirmé la *classe* d'échec, pas désigné le
  coupable. Il faut d'autres déclenchements ;
- **7 jours sans redémarrage inopiné** — le compteur repart du **05/09 ~14:16**
  (dernier PANIC, daté par SHA256 du coredump). Échéance **12/09**. ⚠️ Il ne peut
  pas aboutir avant `bug-100` : au moins deux autres chemins `fopen` peuvent
  encore aborter.

**`feature-075` — assistant de mesure de débit.** Un seul critère ouvert, **et il
n'attend qu'un geste** : mesurer physiquement le bidon de chlore. Baseline posée
le 31/08 (8 750 mL, compteur remis à zéro) ; l'appareil a compté **5 275 mL**
depuis, il devrait rester **3 475 mL**. Écart ≈ 0 ⇒ la chaîne de comptage est
juste, spec close ; écart ≈ +10 % ⇒ biais de comptage, correctif de flotte à
instruire avec `pool-chemistry`. ⚠️ Sous-compter **relâche les quotas** et retarde
l'alerte de bidon vide.

### À ouvrir en priorité (`todo/`)

- **`bug-100`** — `abort()` sur le `fopen` de l'historique. **Critique** : même
  mécanisme que le PANIC fondateur, sur un chemin que le lot B-3 ne couvre pas.
  Livrable principal : le **recensement de tous les `fopen` internes**, pas le
  correctif d'un site isolé.
- **`feature-097`** — seuil de stabilité sur données terrain. **1 matin sur 5**
  collecté (06/09). ⚠️ Son protocole a été **élargi** ce jour-là : relever aussi
  la dérive du pH sur les **45 min qui suivent** l'ouverture de la porte, et
  l'heure de démarrage de la filtration.
- **`feature-092`** — carte « Débit d'injection ». **Deux points à rejouer avant
  ouverture**, consignés en tête de spec : budget WS périmé (chiffré sur 2816, le
  gabarit est à 3072) et placement arbitré sans le bloc TTT.

### Dette de vérification

`specs/features/done/README.md` recense **7 specs archivées avec des vérifications
on-target jamais faites**. Ce n'est pas de la dette de code — tout est livré et
testé — mais des cases vides sans décision. À reprendre, ou à écarter
explicitement.

---

## 3. Ce qui reste à faire côté humain

1. **Mesurer le bidon de chlore** → clôt `feature-075` (cf. §2).
2. **Corriger `BOM.xlsx`** (Excel requis) — trois libellés mal encodés :
   - `… module pH, module ORP**É**` → `… module pH, module ORP`
   - `Pompe P**Ž**ristaltique … plus simple **ˆ** trouver … Santopr**ne**`
     → `P**é**ristaltique … **à** trouver … Santopr**ène**`
   - `Collecteur … diam**tre** 50` → `diam**ètre** 50`
   Puis `python3 tools/bom_export.py`, et `--check` doit être vert.
   ⚠️ **Ne jamais corriger le `.csv`** : il est régénéré à chaque exécution.
   *(Il n'y a PAS d'erreur sur les contacteurs Finder : le BOM en prévoit
   légitimement **deux**, un pour la filtration, un pour l'éclairage.)*
3. **Sauvegarder `builds/`** si l'historique des ELF compte — sinon seuls les ELF
   publiés en release seront décodables.

---

## 4. Leçons de méthode — celles qui ont coûté cher

**`uptime` seul est un mauvais détecteur de redémarrage.** `uptime_minutes` est un
reste modulo 60 : lire « 56 min » quand il s'agit d'1 h 56 fait manquer un crash.
Deux redémarrages sont passés inaperçus pendant deux heures le 05/09, sur un
appareil sous surveillance active. **Vérifier `/coredump/info`** fait désormais
partie de tout relevé.

**Un `diff` aligne ce qu'il peut.** Deux lignes présentées en vis-à-vis ne se
correspondent pas forcément. Une « erreur » a été signalée dans le BOM sur cette
base, et propagée jusque dans les notes d'une release publiée : il y avait
simplement **deux lignes Finder** distinctes. Chercher le terme dans le fichier
avant de conclure.

**Pour détecter une dérive noyée dans du bruit, tester la MONOTONIE, pas
l'amplitude.** Le 06/09, une montée de +0,100 pH a été qualifiée « dans le bruit »
en comparant des moyennes — l'écart-type valait 0,046. La même série, lue en
moyenne glissante, montrait **sept valeurs croissantes d'affilée** puis un
plateau : structure impossible à confondre avec du hasard.

**Une hypothèse prédite d'avance ne paie pas la correction pour recherche
multiple.** Six hausses consécutives valent 1,6 % à une position *fixée*, 17 % si
on les cherche n'importe où. Quand la position est prédite — « le pH remonte-t-il
après la purge ? » —, c'est le premier chiffre qui s'applique.

**Vérifier l'heure plutôt que l'estimer.** Trois annonces d'horaire fausses en une
session, toutes évitables par un `date`.

**~~Ne jamais lancer `pio run -e esp32dev`~~ — piège DISPARU le 06/09.** Il n'y a
plus qu'un environnement matériel, `esp32dev_core3` ([ADR-0069](adr/0069-retrait-repli-core-2.md),
`feature-102`, v2.42.1) : le repli core 2 a été retiré, et avec lui le conflit de
paquet `framework-arduinoespressif32` qui faisait que les deux environnements se
détruisaient mutuellement. `pio run` suffit désormais, sans `-e`.

> Cette leçon a été écrite le 06/09 à 14:46 depuis le Mac, pendant que le conteneur
> supprimait précisément ce repli (commit de 14:09). Les deux copies de `main` ont
> divergé deux heures durant sans se voir — les fichiers touchés étant disjoints,
> la fusion est passée sans conflit, et seul ce paragraphe s'est retrouvé faux.
> **C'est une leçon de méthode en soi**, et elle vaut d'être gardée : une passation
> écrite pendant que quelqu'un d'autre travaille se périme à l'encre encore fraîche.

Le motif du retrait, lui, mérite d'être connu : ce repli n'était plus **compilable**
depuis la v2.40.0, plus **flashable** depuis que le bootloader est en core 3, et
**aucun binaire core 2 n'avait jamais été archivé**. Une assurance triplement
inopérante, dont personne ne s'était aperçu qu'elle avait expiré.

---

## 5. Où trouver le reste

- **`.claude/memory/`** — la mémoire accumulée : pièges récurrents, hypothèses
  réfutées, paramètres du bassin, incidents. **À lire en premier.**
- **`CLAUDE.md`** — le workflow, les règles de sécurité chimique, les fichiers
  générés et les releases.
- **`README.md`** § « Reprendre le projet » — outils, arborescence, commandes.
- **`specs/plan-2026-09-regulation-et-robustesse.md`** — le journal du chantier en
  cours, entrée par entrée.
- **`docs/incidents/`** — les coredumps de production et leurs traces décodées.
- **`docs/adr/`** — les décisions et, surtout, les alternatives écartées.
