#!/usr/bin/env python3
"""Compare deux recensements du tas et désigne les blocs à examiner (feature-128, T1).

    python3 tools/compare_census.py <avant.json> <apres.json>

Les deux fichiers sont des réponses de `GET /debug/heap_census?kind=used`, capturées
à plusieurs heures d'intervalle pendant une fenêtre d'observation.

## Ce que ce script cherche

La fenêtre du 12-13/09/2026 a établi que le plus gros bloc contigu s'effondre en
**quatre marches discrètes** entre 20,7 h et 28,2 h d'uptime, sans jamais remonter
(0 remontée sur 28 relevés). Quatre allocations longue durée se sont posées au
milieu d'une zone libre et n'en sont jamais reparties. Ce script les isole.

## ⚠️ UNE ADRESSE N'EST PAS UNE IDENTITÉ

C'est le piège central de cette comparaison, et il est structurel : entre deux
recensements espacés de plusieurs heures, un bloc peut avoir été **libéré puis
réalloué** à la même adresse par une allocation totalement différente. L'allocateur
TLSF réutilise volontiers une adresse libérée.

Ce script ne dit donc JAMAIS « la même allocation ». Il désigne des **candidats**,
à confirmer par une lecture de contenu (`POST /debug/heap_peek`) qui, elle, revalide
l'adresse contre un parcours frais et teste le canari avant ET après la copie.

La taille demandée (`alloc_size`, gratuite grâce au poisoning léger) est un
discriminant bien plus fort que l'adresse : deux blocs de même adresse ET de même
`alloc_size` sont un candidat sérieux ; de même adresse mais de taille différente,
c'est presque sûrement une réallocation.

## ⚠️ CE SCRIPT N'AFFICHE JAMAIS DE CONTENU

Condition S11 de `security-reviewer` : aucune sortie hexadécimale ou ASCII de
`/debug/heap_peek` ne doit être versionnée. Ce script ne lit que des adresses et
des tailles — il ne consomme pas les réponses de `heap_peek`, et sa sortie peut
être collée telle quelle dans une note d'incident.
"""

import json
import os
import sys


def _charger(chemin):
    """Rend (donnees, None) ou (None, motif de rejet). Ne lève jamais.

    ⚠️ Un 404 est une capture VALIDE ET VIDE. `capture-logs.sh` écrit ce que `curl`
    rend, sans contrôle : une route absente (firmware antérieur, ou build sans le
    volet R3) produit un corps 404 stocké sous un nom de fichier `.json`. Le rejet
    doit être EXPLICITE — un `json.load` qui explose sur « Not Found » enverrait
    chercher un bug là où il n'y a qu'un fichier hors sujet.
    """
    try:
        with open(chemin, encoding="utf-8") as f:
            brut = f.read()
    except OSError as exc:
        return None, "illisible (%s)" % exc
    if not brut.strip():
        return None, "vide"
    try:
        d = json.loads(brut)
    except ValueError:
        apercu = brut.strip().splitlines()[0][:60] if brut.strip() else ""
        return None, "n'est pas du JSON — première ligne : %r" % apercu
    if not isinstance(d, dict):
        return None, "JSON valide mais ce n'est pas un objet"
    # ⚠️ Le tableau s'appelle `entries`, PAS `used` — écrit d'après le design,
    # corrigé d'après l'implémentation livrée. La forme réelle, relevée sur
    # l'appareil le 14/09 : {"kind":"used","entries":[{"addr","size","alloc_size"}],
    # "returned","total","truncated","used_in_window_count","used_truncated"}.
    if d.get("kind") == "free":
        return None, ("c'est un recensement des blocs LIBRES (`kind=free`) — "
                      "la comparaison porte sur les blocs OCCUPÉS (`kind=used`)")
    if "entries" not in d:
        motif = d.get("error")
        if motif:
            return None, "l'appareil a refusé : %r (pas un recensement)" % motif
        return None, "pas de tableau `entries` — est-ce bien un GET ?kind=used ?"
    return d, None


def _index(donnees):
    """{adresse: (taille TLSF, alloc_size)} — alloc_size None si le canari a été refusé."""
    table = {}
    for bloc in donnees.get("entries") or []:
        if not isinstance(bloc, dict):
            continue
        adresse = bloc.get("addr")
        if adresse is None:
            continue
        table[int(adresse)] = (bloc.get("size"), bloc.get("alloc_size"))
    return table


def _hex(v):
    return "—" if v is None else "0x%08X" % v


def _taille(v):
    return "—" if v is None else str(v)


def main():
    if len(sys.argv) != 3:
        print(__doc__.splitlines()[2].strip(), file=sys.stderr)
        return 2

    avant, motif_a = _charger(sys.argv[1])
    apres, motif_b = _charger(sys.argv[2])
    for nom, motif in ((sys.argv[1], motif_a), (sys.argv[2], motif_b)):
        if motif:
            print("REJET — %s : %s" % (os.path.basename(nom), motif))
    if motif_a or motif_b:
        return 1

    # Le seul auto-contrôle du dispositif : si les compteurs du recensement ne
    # concordent pas avec fb/ab, l'instrument lui-même est suspect.
    for etiquette, d in (("avant", avant), ("après", apres)):
        conc = d.get("concordance")
        if conc is not None and conc != "match":
            print("⚠️  %s : concordance = %r — l'instrument n'a pas vu le même tas "
                  "que la sonde. Toute conclusion tirée de ce relevé est fragile."
                  % (etiquette, conc))

    ia, ib = _index(avant), _index(apres)
    apparus = sorted(set(ib) - set(ia))
    disparus = sorted(set(ia) - set(ib))
    communs = sorted(set(ia) & set(ib))

    print("Recensement AVANT : %d blocs occupés dans la fenêtre · APRÈS : %d"
          % (len(ia), len(ib)))
    for etiquette, d in (("avant", avant), ("après", apres)):
        if d.get("used_truncated") or d.get("truncated"):
            print("⚠️  %s : liste TRONQUÉE (`used_truncated`) — des blocs manquent, "
                  "resserre la fenêtre `used_from`/`used_to`." % etiquette)
    print()

    print("APPARUS entre les deux relevés — %d" % len(apparus))
    print("  Ce sont les candidats : un bloc occupé qui n'existait pas avant, dans une")
    print("  zone auparavant libre, est ce qui découpe le grand bloc contigu.")
    print()
    if apparus:
        print("  %-12s %10s %12s" % ("adresse", "taille", "alloc_size"))
        for adresse in apparus:
            taille, demande = ib[adresse]
            print("  %-12s %10s %12s" % (_hex(adresse), _taille(taille), _taille(demande)))
    else:
        print("  (aucun — élargis la fenêtre, ou espace davantage les deux relevés)")
    print()

    if communs:
        # Même adresse ET même alloc_size : candidat sérieux. Même adresse, taille
        # différente : réallocation quasi certaine (cf. avertissement en tête).
        stables = [a for a in communs if ia[a] == ib[a]]
        remplaces = [a for a in communs if ia[a] != ib[a]]
        print("PRÉSENTS DANS LES DEUX — %d, dont %d à taille identique"
              % (len(communs), len(stables)))
        if remplaces:
            print("  ⚠️  %d adresses réapparaissent avec une taille DIFFÉRENTE : ce sont"
                  % len(remplaces))
            print("      des réallocations, pas des survivants. Ne pas les suivre.")
            for adresse in remplaces[:10]:
                print("      %s : %s/%s → %s/%s" % (
                    _hex(adresse), _taille(ia[adresse][0]), _taille(ia[adresse][1]),
                    _taille(ib[adresse][0]), _taille(ib[adresse][1])))
        print()

    print("DISPARUS — %d (libérés entre les deux relevés : ils ne fragmentent pas "
          "durablement)" % len(disparus))
    print()
    print("SUITE — pour chaque candidat APPARU, sur un build de DIAGNOSTIC :")
    print("    POST /debug/heap_peek?addr=<adresse>&len=64   puis   GET /debug/heap_peek")
    print("  Le champ `ascii` lit une String de journal EN CLAIR. ⚠️ Ne JAMAIS verser")
    print("  ce contenu dans le dépôt (condition S11) : la note d'incident cite")
    print("  l'adresse, la taille et l'identification — jamais les octets.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
