"""
Archivage automatique des artefacts de build (bug-famine-8bits, bloc F, v2.20.2).

POURQUOI
--------
Le coredump du crash de production du 15/08/2026 a été INEXPLOITABLE : décoder un
coredump exige l'ELF EXACT du firmware qui tournait, or le dernier ELF présent
dans `builds/` datait du 02/08. `deploy.sh` archive bien l'ELF (fonction
archive_elf), mais `pio run` lancé directement et `./ota_update.sh` ne passent pas
par lui — et ce sont les chemins réellement utilisés au quotidien.

Ce script est branché en `extra_scripts = post:` sur les envs matériels : chaque
`pio run` archive DÉSORMAIS l'ELF **et** le BIN (le BIN identifie l'image
effectivement flashée, l'ELF porte les symboles) sous :

    builds/<VERSION>/<ENV>/firmware_<AAAAMMJJ_HHMMSS>.{elf,bin}

La version est lue dans `src/version.h` (FIRMWARE_VERSION) : les archives sont
ainsi rangées par version, ce qui rend triviale la question « quel ELF correspond
au firmware 2.20.2 qui a crashé ? ». Le sous-dossier d'environnement
(`esp32dev_core3`) est MAINTENU (ADR-0042) : il est né du temps où deux envs
matériels produisaient des binaires DIFFÉRENTS sous la même version. L'env de
repli a disparu en v2.42.1 (ADR-0069), le cloisonnement reste — il garde les
archives lisibles et survivra à l'ajout d'une future cible (S3, banc d'essai).

Rétention : les 5 archives les plus récentes PAR VERSION ET PAR ENVIRONNEMENT
(l'ELF pèse ~10 Mo ; 5 suffisent à couvrir les itérations d'une même version sans
saturer le dépôt). Les paires elf/bin sont retenues indépendamment.

Le script ne fait JAMAIS échouer le build : toute erreur est signalée et ignorée.
"""

import os
import re
import shutil
from datetime import datetime

# Un fichier `firmware_*` NON horodaté fausserait le tri lexicographique de la
# purge : on ne considère que le format produit ici (contre-revue 2.20.2).
MOTIF_ARCHIVE = re.compile(r"^firmware_\d{8}_\d{6}\.[a-z]+$")

# Build NON STANDARD (PLATFORMIO_BUILD_FLAGS non vide) : suffixe `-diag`.
# POURQUOI (condition R3-2, security-reviewer P2, feature-128) : un binaire
# compilé avec le drapeau de diagnostic de R3 (voir l'ADR pour la commande
# exacte — elle ne peut pas être écrite ici, `tools/` est dans le périmètre de
# tools/check_no_peek_flag.sh) embarque une route qui LIT LA MÉMOIRE DU
# PROCESSUS. Rangé sous le motif standard, il devient indiscernable d'un build
# de production dans `builds/`, et `/release` — qui prend « le plus récent » —
# l'attacherait à une release publique. Ce n'est pas théorique : c'est arrivé
# le 2026-09-14 pendant la validation de ce lot, et seul
# `tools/check_release_artefacts.sh` l'a intercepté.
# Le suffixe sort ces archives du motif standard, donc de la sélection de
# `/release` ; elles gardent leur propre quota de rétention (un build de
# diagnostic DÉPLOYÉ a besoin de son ELF comme n'importe quel autre — c'est
# justement le cas de la fenêtre d'observation de 40 h).
MOTIF_ARCHIVE_DIAG = re.compile(r"^firmware_\d{8}_\d{6}-diag\.[a-z]+$")

Import("env")  # noqa: F821  (injecté par SCons/PlatformIO)

# Nombre d'archives conservées par version et par extension.
RETENTION_PER_VERSION = 5


def _lire_version(project_dir):
    """Extrait FIRMWARE_VERSION de src/version.h, ou 'inconnue' si illisible."""
    chemin = os.path.join(project_dir, "src", "version.h")
    try:
        with open(chemin, "r", encoding="utf-8") as f:
            m = re.search(r'#define\s+FIRMWARE_VERSION\s+"([^"]+)"', f.read())
            if m:
                return m.group(1)
    except OSError:
        pass
    return "inconnue"


def _purger(dossier, extension, garder, motif=MOTIF_ARCHIVE):
    """Ne conserve que les `garder` fichiers *.extension les plus récents.

    Le dossier est cloisonné PAR ENVIRONNEMENT (cf. archiver_build) : la purge
    ne peut donc pas faire évincer l'ELF déployé par une série de builds d'un
    AUTRE env. C'était le bloquant n°2 de la revue 2.20.2 (à l'époque, une série
    de builds core 2 évinçait l'ELF core 3 réellement en production), et c'est
    très exactement la perte forensique qu'on corrige.

    Tri par NOM et non par date de modification : `shutil.copy2` recopie la mtime
    de la source, si bien qu'un rebuild d'une révision plus ancienne pouvait
    passer pour l'archive la plus récente. L'horodatage étant en tête du nom au
    format AAAAMMJJ_HHMMSS, l'ordre lexicographique EST l'ordre chronologique.

    Une archive PORTEUSE D'UN MARQUEUR `.deployed` n'est JAMAIS purgée, et ne
    compte pas dans le quota. Le cloisonnement par env ne suffisait pas : le
    2026-09-13, cinq builds de la version 2.51.0 dans le MÊME env ont évincé
    l'archive du 12/09 — celle du firmware réellement déployé. Aucun ELF du 12/09
    ne subsistait sur le disque, donc un coredump de cet appareil était devenu
    INDÉCODABLE. C'est la panne du 15/08/2026 reconduite, celle que ce fichier
    existe pour empêcher. Le marqueur est posé par `tools/mark_deployed.py`, par
    comparaison OCTET À OCTET du binaire flashé (voir sa docstring : « la plus
    récente » est fausse dès qu'un build suit le déploiement, ce qui est le cas
    normal).
    """
    fichiers = [
        os.path.join(dossier, n)
        for n in os.listdir(dossier)
        if n.endswith(extension) and motif.match(n)
    ]
    fichiers.sort(reverse=True)
    # Les protégées sont mises de côté AVANT de compter : sinon elles
    # consommeraient le quota et finiraient par le saturer à elles seules.
    candidates = [f for f in fichiers
                  if not os.path.exists(f[: -len(extension)] + ".deployed")]
    for obsolete in candidates[garder:]:
        try:
            os.remove(obsolete)
        except OSError:
            pass


def archiver_build(source, target, env):  # noqa: ARG001 (signature imposée par SCons)
    """Copie firmware.elf et firmware.bin dans builds/<VERSION>/ avec horodatage."""
    try:
        project_dir = env.subst("$PROJECT_DIR")
        build_dir = env.subst("$BUILD_DIR")
        version = _lire_version(project_dir)
        # Cloisonnement PAR ENVIRONNEMENT (revue 2.20.2, bloquant n°2) : deux envs
        # matériels produisaient des binaires DIFFÉRENTS sous la même version et,
        # rangés ensemble, 5 builds core 2 évinçaient l'ELF core 3 réellement
        # déployé — soit la panne forensique du 15/08 reconduite. Le repli core 2
        # a disparu (ADR-0069) ; le cloisonnement, lui, est MAINTENU (ADR-0042).
        pioenv = env.subst("$PIOENV") or "env-inconnu"
        dossier = os.path.join(project_dir, "builds", version, pioenv)
        os.makedirs(dossier, exist_ok=True)

        horodatage = datetime.now().strftime("%Y%m%d_%H%M%S")
        # R3-2 : un build non standard ne doit pas pouvoir passer pour un build
        # de production dans `builds/` (cf. MOTIF_ARCHIVE_DIAG ci-dessus).
        drapeaux = (os.environ.get("PLATFORMIO_BUILD_FLAGS") or "").strip()
        suffixe = "-diag" if drapeaux else ""
        motif = MOTIF_ARCHIVE_DIAG if drapeaux else MOTIF_ARCHIVE
        archives = []
        for extension in (".elf", ".bin"):
            source_path = os.path.join(build_dir, "firmware" + extension)
            if not os.path.isfile(source_path):
                continue
            destination = os.path.join(
                dossier, "firmware_%s%s%s" % (horodatage, suffixe, extension))
            shutil.copy2(source_path, destination)
            archives.append(os.path.basename(destination))
            _purger(dossier, extension, RETENTION_PER_VERSION, motif)

        if archives:
            etiquette = " (DIAGNOSTIC, hors sélection /release)" if drapeaux else ""
            print("Build archivé -> builds/%s/%s/{%s}%s"
                  % (version, pioenv, ", ".join(archives), etiquette))
        else:
            print("Archivage build : aucun artefact trouvé dans %s" % build_dir)
    except Exception as exc:  # noqa: BLE001 - ne JAMAIS casser le build pour un archivage
        print("Archivage build ignoré (%s)" % exc)


# Déclenché après la production de firmware.bin (dernière étape du build image).
env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", archiver_build)  # noqa: F821
