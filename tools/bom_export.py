#!/usr/bin/env python3
"""Génère hardware/BOM.csv depuis le classeur Excel source (BOM.xlsx).

POURQUOI CE SCRIPT : le CSV est un fichier DÉRIVÉ, la source de vérité est le
classeur Excel. Or l'export CSV d'Excel ne permet pas de choisir à la fois
l'encodage UTF-8 ET le point-virgule comme séparateur : chaque export manuel
détruisait les caractères spéciaux du BOM (« 4.7KΩ » → « 4.7K½ », « céramique »
→ « c_ramique »). Ce script produit un CSV toujours identique et toujours
correct — on ne modifie plus jamais le CSV à la main.

USAGE :
    python3 tools/bom_export.py            # génère hardware/BOM.csv
    python3 tools/bom_export.py --check    # vérifie sans écrire (code 1 si écart)

FORMAT PRODUIT (identique aux exports historiques, pour un diff minimal) :
    - séparateur « ; » ;
    - guillemets uniquement quand le champ contient « ; », un guillemet ou un
      retour à la ligne (les virgules des listes « C1,C2,C3 » ne sont donc PAS
      protégées, comme dans les versions précédentes) ;
    - UTF-8 AVEC BOM et fins de ligne CRLF : Excel ouvre alors le fichier
      correctement d'un double-clic, sans boîte de dialogue d'import.

AUCUNE DÉPENDANCE : un .xlsx est une archive ZIP de fichiers XML, lue ici avec
la bibliothèque standard (openpyxl n'est pas requis).
"""

import argparse
import csv
import io
import os
import re
import sys
import zipfile
from xml.etree import ElementTree

# Espace de noms des feuilles de calcul (SpreadsheetML).
_NS = {"x": "http://schemas.openxmlformats.org/spreadsheetml/2006/main"}

# Emplacements par défaut, relatifs à la racine du dépôt.
_SOURCE_DEFAUT = os.path.join("kicad v2", "BOM.xlsx")
_SORTIE_DEFAUT = os.path.join("hardware", "BOM.csv")


def _texte_de_si(element):
    """Concatène tous les fragments <t> d'un <si> (texte enrichi = plusieurs runs)."""
    return "".join(t.text or "" for t in element.iter(f"{{{_NS['x']}}}t"))


def _chaines_partagees(archive):
    """Table des chaînes partagées du classeur (xl/sharedStrings.xml, optionnel)."""
    try:
        brut = archive.read("xl/sharedStrings.xml")
    except KeyError:
        return []
    racine = ElementTree.fromstring(brut)
    return [_texte_de_si(si) for si in racine.findall("x:si", _NS)]


def _chemin_premiere_feuille(archive):
    """Chemin XML de la première feuille, résolu via les relations du classeur."""
    relations = ElementTree.fromstring(archive.read("xl/_rels/workbook.xml.rels"))
    classeur = ElementTree.fromstring(archive.read("xl/workbook.xml"))
    feuille = classeur.find("x:sheets/x:sheet", _NS)
    id_relation = feuille.get(
        "{http://schemas.openxmlformats.org/officeDocument/2006/relationships}id"
    )
    for relation in relations:
        if relation.get("Id") == id_relation:
            cible = relation.get("Target")
            return cible if cible.startswith("xl/") else "xl/" + cible.lstrip("/")
    raise RuntimeError("Feuille introuvable dans les relations du classeur")


def _index_colonne(reference):
    """« C12 » → 2 (index 0-based de la colonne)."""
    lettres = re.match(r"([A-Z]+)", reference or "A").group(1)
    index = 0
    for lettre in lettres:
        index = index * 26 + (ord(lettre) - ord("A") + 1)
    return index - 1


def _valeur_cellule(cellule, chaines):
    """Texte d'une cellule, quel que soit son type (chaîne partagée, inline, nombre)."""
    type_cellule = cellule.get("t")
    if type_cellule == "s":  # chaîne partagée
        valeur = cellule.find("x:v", _NS)
        return chaines[int(valeur.text)] if valeur is not None else ""
    if type_cellule == "inlineStr":
        est = cellule.find("x:is", _NS)
        return _texte_de_si(est) if est is not None else ""
    valeur = cellule.find("x:v", _NS)
    if valeur is None or valeur.text is None:
        return ""
    texte = valeur.text
    # Les entiers Excel arrivent en « 19.0 » : on retire le .0 parasite.
    if re.fullmatch(r"-?\d+\.0+", texte):
        return texte.split(".")[0]
    return texte


def lire_classeur(chemin_xlsx):
    """Retourne les lignes non vides de la première feuille, en liste de listes."""
    with zipfile.ZipFile(chemin_xlsx) as archive:
        chaines = _chaines_partagees(archive)
        feuille = ElementTree.fromstring(archive.read(_chemin_premiere_feuille(archive)))

    lignes = []
    for ligne_xml in feuille.iter(f"{{{_NS['x']}}}row"):
        cellules = {}
        for cellule in ligne_xml.findall("x:c", _NS):
            cellules[_index_colonne(cellule.get("r"))] = _valeur_cellule(cellule, chaines)
        if not cellules:
            continue
        largeur = max(cellules) + 1
        ligne = [cellules.get(i, "") for i in range(largeur)]
        if any(champ.strip() for champ in ligne):  # ignore les lignes entièrement vides
            lignes.append(ligne)
    return lignes


def rendre_csv(lignes):
    """Sérialise les lignes au format attendu (voir en-tête du module)."""
    tampon = io.StringIO(newline="")
    graveur = csv.writer(
        tampon, delimiter=";", quoting=csv.QUOTE_MINIMAL, lineterminator="\r\n"
    )
    # Toutes les lignes sont COMPLÉTÉES à la largeur de l'en-tête : un tableau
    # dont l'en-tête compte 5 colonnes doit en compter 5 partout (un champ Link
    # vide reste un champ). Compléter plutôt que tronquer rend aussi l'export
    # stable quelle que soit la zone sélectionnée dans Excel.
    largeur = len(lignes[0]) if lignes else 0
    for ligne in lignes:
        if len(ligne) < largeur:
            ligne = ligne + [""] * (largeur - len(ligne))
        graveur.writerow(ligne)
    return "﻿" + tampon.getvalue()  # BOM UTF-8 en tête


def main():
    analyseur = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    analyseur.add_argument("--source", default=_SOURCE_DEFAUT, help="classeur Excel source")
    analyseur.add_argument("--sortie", default=_SORTIE_DEFAUT, help="fichier CSV à générer")
    analyseur.add_argument(
        "--check",
        action="store_true",
        help="n'écrit rien ; code de sortie 1 si le CSV n'est pas à jour",
    )
    options = analyseur.parse_args()

    if not os.path.exists(options.source):
        print(f"ERREUR : classeur source introuvable : {options.source}", file=sys.stderr)
        return 2

    lignes = lire_classeur(options.source)
    contenu = rendre_csv(lignes).encode("utf-8")

    # Comparaison en OCTETS : le fichier déjà présent peut être dans un encodage
    # hérité (c'est le défaut que ce script corrige) — le décoder échouerait.
    actuel = None
    if os.path.exists(options.sortie):
        with open(options.sortie, "rb") as fichier:
            actuel = fichier.read()

    if options.check:
        if actuel == contenu:
            print(f"{options.sortie} est à jour ({len(lignes)} lignes)")
            return 0
        print(f"{options.sortie} N'EST PAS à jour — relancer sans --check", file=sys.stderr)
        return 1

    with open(options.sortie, "wb") as fichier:
        fichier.write(contenu)
    etat = "inchangé" if actuel == contenu else "mis à jour"
    print(f"{options.sortie} {etat} — {len(lignes)} lignes depuis {options.source}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
