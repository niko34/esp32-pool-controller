#!/usr/bin/env python3
"""Trace l'évolution de la fragmentation du tas depuis les captures.

    python3 tools/trace_fragmentation.py [répertoire]

Lit les `sysinfo_*.json` produits par `capture-logs.sh` et rend la SÉRIE des
quatre grandeurs qui décrivent l'état du tas interne 8 bits (masque 0x0804,
celui sur lequel toutes les gardes du projet décident) :

    libre · plus gros bloc · nombre de trous · blocs alloués

## Pourquoi ces quatre-là, et pas deux

Le projet n'a longtemps lu que `libre` et `plus gros bloc`. Deux régimes très
différents produisent pourtant le MÊME couple :

  · le grand bloc contigu se fait grignoter, laissant peu de gros trous ;
  · le tas se pulvérise en une multitude de miettes.

`free_blocks` les sépare : `libre / trous` donne la taille MOYENNE d'un trou.
Ces deux compteurs étaient calculés par `heap_caps_get_info()` depuis toujours,
et jetés — v2.51.0 les publie (voir ADR-0077 et la note d'incident du 10/09).

## Ce que le script ne fait pas

Il ne conclut pas. Il aligne des mesures et calcule des écarts ; l'interprétation
reste humaine. En particulier, il ne prétend jamais qu'une pente observée sur
quelques heures se prolongera.
"""

import glob
import json
import os
import sys

# Sentinelle du projet : « non renseigné », jamais une valeur du domaine
# (ADR-0072). Les masques à chemin O(régions) n'ont pas de compte de blocs.
_INCONNU = 0xFFFFFFFF


def _lire(chemin):
    """Rend un relevé exploitable, ou None. Ne lève jamais."""
    try:
        with open(chemin, encoding="utf-8") as fichier:
            d = json.load(fichier)
    except (OSError, ValueError):
        return None
    if not isinstance(d, dict):
        return None
    hf = d.get("hf")
    i8 = hf.get("i8") if isinstance(hf, dict) else None
    if not isinstance(i8, dict):
        # Capture antérieure à `?hf=1`, ou route interrogée sans le paramètre.
        return {"fichier": os.path.basename(chemin), "sans_hf": True,
                "uptime": d.get("uptime_seconds"), "version": d.get("firmware_version")}
    def valeur(cle):
        v = i8.get(cle)
        return None if v is None or v == _INCONNU else v
    return {
        "fichier": os.path.basename(chemin),
        "sans_hf": False,
        "version": d.get("firmware_version"),
        # ⚠️ `uptime_seconds` porte le TOTAL ; `days`/`hours`/`minutes` n'en sont
        # que la décomposition. Les additionner double-compte — erreur commise
        # le 12/09, qui annonçait 12 h 40 pour 6 h 20 réelles.
        "uptime": d.get("uptime_seconds"),
        "libre": valeur("f"),
        "bloc": valeur("b"),
        "trous": valeur("fb"),
        "alloues": valeur("ab"),
        "min_libre": valeur("mf"),
        "min_bloc": valeur("mb"),
        "walk_max": (hf or {}).get("wmax"),
    }


def _heures(secondes):
    return "—" if secondes is None else "%5.1f" % (secondes / 3600.0)


def main():
    racine = sys.argv[1] if len(sys.argv) > 1 else "/root/pool-captures"
    fichiers = sorted(glob.glob(os.path.join(racine, "sysinfo_*.json")))
    if not fichiers:
        print("Aucune capture dans %s" % racine)
        return 1

    releves = [r for r in (_lire(f) for f in fichiers) if r]
    avec_hf = [r for r in releves if not r["sans_hf"] and r["trous"]]
    # Trois causes d'absence, à NE PAS confondre — un diagnostic qui désigne la
    # mauvaise envoie chercher au mauvais endroit.
    sans_objet_hf = [r for r in releves if r["sans_hf"]]
    trop_ancien = [r for r in releves if not r["sans_hf"] and not r["trous"]]

    print("%d capture(s) · %d exploitable(s) · %d sans bloc `hf` · %d antérieure(s) "
          "aux compteurs de blocs"
          % (len(releves), len(avec_hf), len(sans_objet_hf), len(trop_ancien)))
    if not avec_hf:
        print()
        if trop_ancien:
            versions = sorted({r["version"] for r in trop_ancien if r["version"]})
            print("Aucune capture exploitable : %d portent bien l'objet `hf`, mais SANS"
                  % len(trop_ancien))
            print("`fb`/`ab` — ces compteurs n'existent que depuis v2.51.0.")
            print("Versions rencontrées : %s" % (", ".join(versions) or "inconnues"))
            print()
            print("Ce n'est pas un défaut de capture : il faut simplement attendre un")
            print("relevé postérieur au déploiement.")
        if sans_objet_hf:
            print("%d capture(s) n'ont aucun objet `hf` — la route a-t-elle été"
                  % len(sans_objet_hf))
            print("interrogée sans `?hf=1` ?")
        return 1

    print()
    print("  %-22s %-8s %5s │ %8s %8s %6s %6s │ %8s %8s"
          % ("capture", "version", "h", "libre", "bloc", "trous", "alloués",
             "min libre", "min bloc"))
    print("  " + "─" * 104)

    precedent = None
    for r in avec_hf:
        moyen = (r["libre"] / r["trous"]) if (r["libre"] and r["trous"]) else None
        ligne = ("  %-22s %-8s %5s │ %8s %8s %6s %6s │ %8s %8s"
                 % (r["fichier"].replace("sysinfo_", "").replace(".json", ""),
                    r["version"] or "—", _heures(r["uptime"]),
                    r["libre"], r["bloc"], r["trous"], r["alloues"],
                    r["min_libre"], r["min_bloc"]))
        print(ligne)
        # Une reprise d'uptime signale un REDÉMARRAGE : la série repart de zéro
        # et comparer de part et d'autre n'a aucun sens.
        if precedent and r["uptime"] is not None and precedent["uptime"] is not None:
            if r["uptime"] < precedent["uptime"]:
                print("  %s REDÉMARRAGE ici — la série repart, ne compare pas "
                      "à travers" % (" " * 20))
        precedent = r
        if moyen:
            reste = (r["libre"] - r["bloc"]) if r["bloc"] else None
            autres = r["trous"] - 1
            detail = "trou moyen %4.0f o" % moyen
            if reste is not None and autres > 0:
                detail += " · hors gros bloc : %6d o sur %3d trous = %4.0f o" % (
                    reste, autres, reste / autres)
            print("  %s└─ %s" % (" " * 20, detail))

    premier, dernier = avec_hf[0], avec_hf[-1]
    if premier is not dernier:
        print()
        print("ÉCART ENTRE LA PREMIÈRE ET LA DERNIÈRE CAPTURE")
        for libelle, cle in (("libre", "libre"), ("plus gros bloc", "bloc"),
                             ("trous libres", "trous"), ("blocs alloués", "alloues")):
            a, b = premier[cle], dernier[cle]
            if a and b:
                print("  %-16s %8d → %8d   %+7d  (%+.0f %%)"
                      % (libelle, a, b, b - a, 100.0 * (b - a) / a))
        print()
        print("⚠️ Un écart n'est pas une tendance. Une reprise d'uptime dans la")
        print("   série ci-dessus invalide toute comparaison qui l'enjambe.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
