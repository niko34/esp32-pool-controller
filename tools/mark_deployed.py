#!/usr/bin/env python3
"""Marque l'archive de build qui correspond au binaire RÉELLEMENT flashé.

    python3 tools/mark_deployed.py <chemin du .bin envoyé à l'appareil>

POURQUOI CE SCRIPT EXISTE
-------------------------
`tools/archive_build.py` conserve les 5 archives les plus récentes par version et
par environnement. Sa garde d'origine protège contre l'éviction par un AUTRE
environnement — c'était le bloquant n°2 de la revue 2.20.2. Elle ne protège pas
contre cinq builds de la MÊME version.

Et c'est exactement ce qui s'est produit le 2026-09-13 : le travail sur bug-126 a
produit cinq builds de la version 2.51.0, qui ont évincé l'archive du 2026-09-12
— celle du firmware **réellement déployé**. Recherche sur tout le disque : aucun
ELF du 12/09. Un coredump de cet appareil serait donc INDÉCODABLE, `esp-coredump`
vérifiant le SHA256 de l'image applicative et refusant tout ELF qui ne correspond
pas. Un rebuild depuis le même commit ne suffit pas : l'IDF grave la date de
compilation dans l'image.

C'est la panne forensique du 15/08/2026 reconduite — celle-là même que
`archive_build.py` avait été écrit pour empêcher, et que sa propre docstring cite.

POURQUOI UNE COMPARAISON D'OCTETS, ET NON « LA PLUS RÉCENTE »
------------------------------------------------------------
Deux raisons, et la seconde est celle qui a mordu :

1. `archive_build.py` documente lui-même (`_purger`) pourquoi il trie par NOM et
   non par date : `shutil.copy2` recopie la mtime de la source, donc un rebuild
   d'une révision ancienne peut passer pour l'archive la plus récente.
2. Surtout, « la plus récente » est FAUSSE dès qu'un build a lieu après le
   déploiement — ce qui est le cas normal : on déploie, puis on continue à
   travailler. C'est précisément la séquence du 12-13/09.

La seule identification qui ne ment pas est l'égalité octet à octet entre le
binaire envoyé et un `.bin` archivé. L'ELF de la même paire (même horodatage)
est alors, lui aussi, celui qui correspond.

CE QUE LE MARQUEUR NE FAIT PAS
------------------------------
Il ne dit pas que l'appareil tourne ENCORE cette version — seulement qu'elle a
été flashée un jour. C'est ce qu'il faut : on veut conserver l'ELF de toute image
ayant tourné en production, pas seulement de la dernière.
"""

import glob
import os
import sys

# Doit rester identique au motif de tools/archive_build.py : les deux scripts
# nomment le même objet. Une divergence rendrait le marqueur invisible à la purge.
_SUFFIXE_MARQUEUR = ".deployed"


def _identique(a, b):
    """Égalité octet à octet, sans charger les fichiers entiers en mémoire."""
    try:
        if os.path.getsize(a) != os.path.getsize(b):
            return False
        with open(a, "rb") as fa, open(b, "rb") as fb:
            while True:
                ba, bb = fa.read(65536), fb.read(65536)
                if ba != bb:
                    return False
                if not ba:
                    return True
    except OSError:
        return False


def main():
    if len(sys.argv) != 2:
        print("usage: mark_deployed.py <firmware.bin flashé>", file=sys.stderr)
        return 2
    flashe = sys.argv[1]
    if not os.path.isfile(flashe):
        print("mark_deployed : %s introuvable — aucun marquage" % flashe)
        return 0  # jamais bloquant : un déploiement réussi ne doit pas échouer ici

    racine = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "builds")
    candidats = sorted(glob.glob(os.path.join(racine, "*", "*", "firmware_*.bin")))
    for archive in candidats:
        if not _identique(flashe, archive):
            continue
        marqueur = archive[: -len(".bin")] + _SUFFIXE_MARQUEUR
        try:
            with open(marqueur, "w", encoding="utf-8") as f:
                f.write("Image flashée sur l'appareil. Ne pas purger : un coredump\n"
                        "de cette version exige CET ELF (SHA256 de l'image applicative).\n")
        except OSError as exc:
            print("mark_deployed : marqueur non écrit (%s)" % exc)
            return 0
        print("Build marqué DÉPLOYÉ -> %s" % os.path.relpath(marqueur, os.path.dirname(racine)))
        return 0

    # Pas d'échec : un binaire construit ailleurs, ou une archive déjà purgée,
    # ne doivent pas faire rougir un déploiement qui a réussi. Mais on le DIT :
    # c'est le signal que l'ELF correspondant n'est plus récupérable.
    print("⚠️  mark_deployed : aucune archive ne correspond à %s" % os.path.basename(flashe))
    print("   L'ELF de cette image n'est pas archivé — un coredump serait indécodable.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
