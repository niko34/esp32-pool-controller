#!/usr/bin/env python3
# =============================================================================
# github_root_ca_vectors.py — le paquet de racines TLS est structurellement
# cohérent (bug-165, 2026-10-14)
# =============================================================================
# `src/github_root_ca.h` n'est PAS compilable nativement (dépend de mbedtls
# embarqué) : ce harnais EXTRAIT le texte réel du fichier livré (pas une copie
# recopiée à la main — même esprit que test/sha256_vectors.js, ADR-0099) et
# vérifie, via `openssl` (déjà présent sur les runners CI et en local) :
#
#   1. il contient exactement 5 certificats PEM ;
#   2. aucune des 3 racines d'ORIGINE n'a disparu (USERTrust RSA, USERTrust
#      ECC, ISRG Root X1 — cette dernière ancre objects.githubusercontent.com,
#      le CHEMIN DE TÉLÉCHARGEMENT du firmware OTA) ;
#   3. les empreintes SHA-256 ANNONCÉES EN COMMENTAIRE dans le header
#      correspondent aux certificats RÉELLEMENT présents — un commentaire qui
#      ment sur l'empreinte est exactement le genre d'écart qu'on ne détecte
#      qu'en le vérifiant mécaniquement.
#
# Ne vérifie PAS la validation TLS réelle contre GitHub (hors de portée d'un
# test natif/CI — nécessite le matériel, voir le plan de test manuel de
# bug-165). Ne vérifie pas non plus la non-expiration (couvert par la lecture
# humaine des dates ; un test automatique figerait une date de péremption).
#
# Usage : python3 tools/github_root_ca_vectors.py
# Sortie : 0 si conforme, 1 sinon (détail sur stdout).
# =============================================================================

import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
HEADER_PATH = REPO_ROOT / "src" / "github_root_ca.h"

# Les 3 racines d'origine (bug-checkupdate-tls-core3, 2026-08-08) : AUCUNE ne
# doit disparaître. ISRG Root X1 est celle qui ancre le CDN de téléchargement.
REQUIRED_ORIGINAL_SUBJECTS = [
    "USERTrust RSA Certification Authority",
    "USERTrust ECC Certification Authority",
    "ISRG Root X1",
]

# Étiquette de commentaire (telle qu'écrite dans le header) -> fragment de
# sujet du certificat correspondant. ⚠️ Les empreintes elles-mêmes ne sont PAS
# recopiées ici : `extract_announced_fingerprints()` les LIT dans le fichier
# livré, pour qu'un commentaire désaligné du certificat réel fasse échouer ce
# script plutôt que d'être masqué par une copie figée dans le harnais.
LABEL_TO_SUBJECT = {
    "RSA": "USERTrust RSA Certification Authority",
    "ECC": "USERTrust ECC Certification Authority",
    "E46 (ECC)": "Sectigo Public Server Authentication Root E46",
    "R46 (RSA)": "Sectigo Public Server Authentication Root R46",
    # Ajoutée sur revue (2026-10-14) : son empreinte n'était PAS contrôlée,
    # alors que c'est l'ancre d'objects.githubusercontent.com — le chemin de
    # TÉLÉCHARGEMENT du firmware. Couverture portée de 4/5 à 5/5.
    "X1": "ISRG Root X1",
}


def extract_announced_fingerprints(header_text):
    """Lit les lignes de commentaire du type
    ``//   RSA E7:93:...`` ou ``//   E46 (ECC) C9:0F:...`` et retourne
    {sujet: empreinte_annoncée}. Lues depuis le fichier LIVRÉ (pas recopiées)
    pour que ce script détecte un commentaire désaligné du certificat réel."""
    result = {}
    fp_pattern = r"([0-9A-Fa-f]{2}(?::[0-9A-Fa-f]{2}){31})"
    for label, subject in LABEL_TO_SUBJECT.items():
        m = re.search(
            r"^//\s+" + re.escape(label) + r"\s+" + fp_pattern + r"\s*$",
            header_text,
            re.MULTILINE,
        )
        if m:
            result[subject] = m.group(1).upper()
    return result


def fail(msg):
    print(f"ERREUR : {msg}", file=sys.stderr)
    sys.exit(1)


def extract_pem_blocks(header_text):
    """Extrait les blocs PEM réels du littéral C (ADR-0099 : on parse le code
    livré, on ne le recopie pas). Chaque ligne du littéral est de la forme
    "-----BEGIN...-----\\n" ... (avec une continuation ``\\`` en fin de ligne
    C) ; on isole le contenu entre guillemets et on déséchappe ``\\n``."""
    lines = []
    in_literal = False
    for raw_line in header_text.splitlines():
        if "const char* GITHUB_ROOT_CA" in raw_line:
            in_literal = True
        if not in_literal:
            continue
        m = re.search(r'"((?:[^"\\]|\\.)*)"', raw_line)
        if m:
            content = m.group(1).replace("\\n", "\n")
            lines.append(content)
        if raw_line.rstrip().endswith(";") and "GITHUB_ROOT_CA" not in raw_line:
            break
    full_text = "".join(lines)
    blocks = re.findall(
        r"-----BEGIN CERTIFICATE-----\n.*?-----END CERTIFICATE-----\n?",
        full_text,
        re.DOTALL,
    )
    return blocks


def openssl_subject(pem_block):
    res = subprocess.run(
        ["openssl", "x509", "-noout", "-subject", "-nameopt", "oneline"],
        input=pem_block, capture_output=True, text=True,
    )
    if res.returncode != 0:
        fail(f"openssl n'a pas pu lire le certificat (sujet) :\n{res.stderr}")
    return res.stdout.strip()


def openssl_fingerprint_sha256(pem_block):
    res = subprocess.run(
        ["openssl", "x509", "-noout", "-fingerprint", "-sha256"],
        input=pem_block, capture_output=True, text=True,
    )
    if res.returncode != 0:
        fail(f"openssl n'a pas pu lire le certificat (empreinte) :\n{res.stderr}")
    # Format : "sha256 Fingerprint=AA:BB:..." ou "SHA256 Fingerprint=..."
    m = re.search(r"Fingerprint=([0-9A-Fa-f:]+)", res.stdout)
    if not m:
        fail(f"empreinte SHA-256 introuvable dans la sortie openssl : {res.stdout!r}")
    return m.group(1).upper()


def openssl_is_self_signed(pem_block):
    """Vrai si le certificat est AUTO-SIGNÉ (sujet == émetteur ET signature valide
    par sa propre clé).

    ⚠️ C'EST L'INVARIANT QUI A CAUSÉ LE BUG DEUX FOIS (2026-08-08, puis 2026-10-14) :
    mbedtls 3.x REFUSE les ancres de confiance cross-signées. Une racine croisée
    ressemble à une racine, porte le même nom, affiche la même clé publique — et
    ne fonctionne pas. ⛔ Vérifier le NOM ne suffit pas : on vérifie la SIGNATURE.
    """
    import tempfile, os
    with tempfile.NamedTemporaryFile("w", suffix=".pem", delete=False) as fh:
        fh.write(pem_block + "\n")
        path = fh.name
    try:
        r = subprocess.run(
            ["openssl", "verify", "-no-CAfile", "-no-CApath", "-CAfile", path, path],
            capture_output=True, text=True)
        return r.returncode == 0
    finally:
        os.unlink(path)


def main():
    if not HEADER_PATH.exists():
        fail(f"fichier introuvable : {HEADER_PATH}")
    header_text = HEADER_PATH.read_text()

    blocks = extract_pem_blocks(header_text)
    announced_fingerprints = extract_announced_fingerprints(header_text)

    errors = []

    if len(announced_fingerprints) != len(LABEL_TO_SUBJECT):
        missing = set(LABEL_TO_SUBJECT.values()) - set(announced_fingerprints)
        errors.append(
            f"empreinte(s) annoncée(s) introuvable(s) dans les commentaires du "
            f"header pour : {', '.join(missing)}"
        )

    # --- 1. exactement 5 certificats -----------------------------------
    if len(blocks) != 5:
        errors.append(
            f"attendu 5 certificats dans GITHUB_ROOT_CA, trouvé {len(blocks)}"
        )

    # Sujet -> empreinte calculée, pour les vérifications suivantes.
    subjects_present = []
    fingerprint_by_subject = {}
    for block in blocks:
        subject = openssl_subject(block)
        subjects_present.append(subject)
        fp = openssl_fingerprint_sha256(block)
        # On indexe par fragment de sujet reconnu (CN=...) pour retrouver le
        # certificat correspondant dans ANNOUNCED_FINGERPRINTS/REQUIRED_*.
        for known in list(REQUIRED_ORIGINAL_SUBJECTS) + list(LABEL_TO_SUBJECT.values()):
            if known in subject:
                fingerprint_by_subject[known] = fp

    # --- 1 bis. CHAQUE certificat est AUTO-SIGNÉ -------------------------
    # ⚠️ Condition posée par security-reviewer : le contrôle annonçait
    # « toutes auto-signées » sans le vérifier. Un garde-fou qui ne garde pas
    # ce qu'il annonce est pire qu'absent — et c'est l'invariant qui a cassé
    # la mise à jour en ligne DEUX FOIS.
    for block in blocks:
        subject = openssl_subject(block)
        if not openssl_is_self_signed(block):
            errors.append(
                f"ancre NON AUTO-SIGNÉE : « {subject} ». mbedtls 3.x REFUSE les "
                f"ancres cross-signées — c'est la cause de bug-165 (2026-10-14) "
                f"et de l'incident du 2026-08-08. Prendre la variante auto-signée."
            )

    # --- 2. aucune des 3 racines d'origine n'a disparu -------------------
    for required in REQUIRED_ORIGINAL_SUBJECTS:
        if not any(required in s for s in subjects_present):
            errors.append(
                f"racine d'origine DISPARUE : « {required} » absente du paquet "
                f"(ISRG Root X1 ancre objects.githubusercontent.com, le chemin "
                f"de téléchargement OTA)"
            )

    # --- 3. les empreintes annoncées correspondent aux certificats réels -
    for subject_fragment, announced_fp in announced_fingerprints.items():
        actual_fp = fingerprint_by_subject.get(subject_fragment)
        if actual_fp is None:
            errors.append(
                f"certificat « {subject_fragment} » (empreinte annoncée en "
                f"commentaire) introuvable parmi les certificats présents"
            )
            continue
        if actual_fp.replace(":", "") != announced_fp.replace(":", ""):
            errors.append(
                f"empreinte DÉSALIGNÉE pour « {subject_fragment} » : "
                f"commentaire={announced_fp} réel={actual_fp}"
            )

    if errors:
        for e in errors:
            print(f"ERREUR : {e}", file=sys.stderr)
        print(
            f"\n{len(blocks)} certificat(s) extrait(s), sujets : "
            + "; ".join(subjects_present),
            file=sys.stderr,
        )
        sys.exit(1)

    print(
        f"OK : {len(blocks)} certificats, 3 racines d'origine présentes, "
        f"{len(announced_fingerprints)} empreintes annoncées vérifiées."
    )
    sys.exit(0)


if __name__ == "__main__":
    main()
