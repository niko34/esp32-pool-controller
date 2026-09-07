#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Générateur des fixtures d'import d'historique (bug-101 / bug-113).

POURQUOI UN GÉNÉRATEUR ET PAS SIX FICHIERS ÉCRITS À LA MAIN
-----------------------------------------------------------
Les plafonds (kMaxRawDataPoints / kMaxHourlyDataPoints / kMaxDailyDataPoints)
sont susceptibles de bouger avec la partition. Un fichier de 339 lignes écrit à
la main devient alors un faux pire cas, silencieusement : il continue de passer
tout en ne testant plus la borne. Le générateur porte les plafonds en tête de
fichier, et `test_native_history_import` REVÉRIFIE que ce qu'il a produit
correspond aux plafonds lus dans src/constants.h — la fixture ne peut pas
divorcer du firmware sans qu'un test rougisse.

⚠️ AUCUNE DONNÉE RÉELLE ICI. La sauvegarde du 2026-09-05 vit hors du dépôt
(/root/pool-backups/2026-09-05-avant-flash-8mo/) et contient l'IP du broker MQTT
et le SSID. Elle a servi à CALIBRER les formes ci-dessous (distribution
77 RAW / 168 HOURLY, cellules manquantes, séparateur d'espace de l'export
hérité) ; rien n'en est recopié. Tout est synthétique et DÉTERMINISTE (PRNG
graine fixe) : deux exécutions donnent des fichiers identiques, donc un diff
git vide tant que le format ne change pas.

FORMAT — celui de `bindHistoryBackup` (data/app.js), à la colonne près :
    datetime,ph,orp,temperature,filtration,dosing,dosing_mask,granularity
`datetime` en `YYYY-MM-DDTHH:MM:SS` (le `T` de formatDateTimeFromEpoch).
`dosing` = booléen d'héritage, `dosing_mask` = champ de bits kHistDose*.

Usage :  python3 tools/gen_history_fixture.py [dossier_de_sortie]
Défaut : test/fixtures/
"""

import os
import random
import sys
from datetime import datetime, timedelta

# Plafonds — DOIVENT rester alignés sur src/constants.h:183-185.
# test_native_history_import les relit dans le firmware et compare.
K_MAX_RAW = 96
K_MAX_HOURLY = 168
K_MAX_DAILY = 75
K_TOTAL_WORST = K_MAX_RAW + K_MAX_HOURLY + K_MAX_DAILY  # 339

# Granularités (enum Granularity de src/history.h)
RAW, HOURLY, DAILY = 0, 1, 2

# Masques de dosage (kHistDose* de src/history.h)
DOSE_NONE, DOSE_PH, DOSE_ORP, DOSE_UNKNOWN = 0x00, 0x01, 0x02, 0x04

HEADER = ["datetime", "ph", "orp", "temperature", "filtration", "dosing",
          "dosing_mask", "granularity"]

# Instant de référence : figé, jamais `now()`. Une fixture qui dépend de la date
# du jour produit un diff git à chaque exécution et un pire cas mouvant.
REF = datetime(2026, 9, 5, 14, 30, 0)


def fmt(dt):
    """Format EXACT de formatDateTimeFromEpoch (séparateur `T`)."""
    return dt.strftime("%Y-%m-%dT%H:%M:%S")


def plausible(rng, dt):
    """Valeurs du bassin de l'utilisateur : pH 6,9-7,4 · ORP 480-620 · T 22-28.

    Une ondulation diurne s'ajoute au bruit : un jeu plat masquerait un défaut
    d'agrégation (moyenner des valeurs identiques rend le bon résultat même
    quand la moyenne est fausse).
    """
    hour = dt.hour + dt.minute / 60.0
    diurne = (hour - 14.0) / 14.0  # ~0 à 14 h, négatif la nuit
    ph = round(7.15 + 0.20 * diurne + rng.uniform(-0.06, 0.06), 2)
    ph = min(7.40, max(6.90, ph))
    orp = int(round(550 + 55 * diurne + rng.uniform(-18, 18)))
    orp = min(620, max(480, orp))
    temp = round(25.0 + 2.4 * diurne + rng.uniform(-0.4, 0.4), 1)
    temp = min(28.0, max(22.0, temp))
    return ph, orp, temp


def row(dt, ph, orp, temp, filtration, mask, gran):
    """Une ligne au format de l'export. `None` -> cellule VIDE (`point.ph ?? ""`)."""
    def cell(v):
        return "" if v is None else str(v)
    return ",".join([
        fmt(dt), cell(ph), cell(orp), cell(temp),
        "1" if filtration else "0",
        "1" if mask else "0",          # `dosing` : booléen d'héritage
        str(mask),                     # `dosing_mask` : la voie réelle
        str(gran),
    ])


def write(path, header, lines, eol="\n", bom=False):
    data = eol.join([header] + lines) + eol
    with open(path, "w", encoding="utf-8", newline="") as fp:
        if bom:
            fp.write("﻿")
        fp.write(data)
    print("  %-28s %4d lignes  %6d o" % (os.path.basename(path), len(lines), len(data)))


# ---------------------------------------------------------------------------
# 1. history-339.csv — PIRE CAS STRUCTUREL
# ---------------------------------------------------------------------------
# 96 RAW + 168 HOURLY + 75 DAILY = exactement les trois plafonds. C'est le
# maximum STOCKABLE : au-delà, rien de plus ne rentre, quoi qu'on importe. Donc
# le seul jeu qui prouve « import complet, dropped = 0 ».
# Ordre chronologique DAILY -> HOURLY -> RAW, comme sur l'appareil : chaque
# granularité couvre une fenêtre distincte, sans chevauchement de créneau (deux
# points de MÊME timestamp et MÊME granularité seraient un doublon, et la
# fixture ne doit pas en contenir — c'est le test qui le vérifie).
def gen_339():
    rng = random.Random(339)
    lines = []
    # RAW : 96 points à 5 min, la fenêtre la plus récente, finissant à REF.
    raw_start = REF - timedelta(minutes=5 * (K_MAX_RAW - 1))
    # HOURLY : 168 points horaires alignés sur :00:00, finissant AVANT raw_start.
    hourly_end = (raw_start - timedelta(hours=1)).replace(minute=0, second=0)
    hourly_start = hourly_end - timedelta(hours=K_MAX_HOURLY - 1)
    # DAILY : 75 points à 00:00:00, finissant AVANT hourly_start.
    daily_end = (hourly_start - timedelta(days=1)).replace(hour=0, minute=0, second=0)
    daily_start = daily_end - timedelta(days=K_MAX_DAILY - 1)

    for i in range(K_MAX_DAILY):
        dt = daily_start + timedelta(days=i)
        ph, orp, temp = plausible(rng, dt.replace(hour=14))
        # Un agrégat DAILY porte l'UNION des masques de sa journée : 0x03 est le
        # cas normal d'une journée où les deux pompes ont tourné.
        mask = rng.choice([DOSE_NONE, DOSE_PH, DOSE_ORP, DOSE_PH | DOSE_ORP])
        lines.append(row(dt, ph, orp, temp, True, mask, DAILY))

    for i in range(K_MAX_HOURLY):
        dt = hourly_start + timedelta(hours=i)
        ph, orp, temp = plausible(rng, dt)
        filtr = 8 <= dt.hour < 20
        mask = rng.choice([DOSE_NONE, DOSE_NONE, DOSE_PH, DOSE_ORP]) if filtr else DOSE_NONE
        lines.append(row(dt, ph, orp, temp, filtr, mask, HOURLY))

    for i in range(K_MAX_RAW):
        dt = raw_start + timedelta(minutes=5 * i)
        ph, orp, temp = plausible(rng, dt)
        mask = rng.choice([DOSE_NONE] * 5 + [DOSE_PH, DOSE_ORP])
        lines.append(row(dt, ph, orp, temp, True, mask, RAW))
    return lines


# ---------------------------------------------------------------------------
# 2. history-245.csv — LE CAS RÉEL DU 05/09, RECONSTITUÉ
# ---------------------------------------------------------------------------
# Même DISTRIBUTION que la sauvegarde hors dépôt : 77 RAW + 168 HOURLY, zéro
# DAILY (l'appareil venait d'être remis en service). Onze points RAW aux trois
# mesures ABSENTES y figuraient — la porte de mesure fermée, capteurs non lus.
# Ils sont reproduits parce que c'est là que l'import se casse le plus souvent :
# une cellule vide relue comme `0` fabrique un pH 0 dans l'historique.
def gen_245():
    rng = random.Random(245)
    lines = []
    raw_count = 77
    raw_start = REF - timedelta(minutes=5 * (raw_count - 1))
    hourly_end = (raw_start - timedelta(hours=1)).replace(minute=0, second=0)
    hourly_start = hourly_end - timedelta(hours=K_MAX_HOURLY - 1)

    for i in range(K_MAX_HOURLY):
        dt = hourly_start + timedelta(hours=i)
        ph, orp, temp = plausible(rng, dt)
        filtr = 8 <= dt.hour < 20
        lines.append(row(dt, ph, orp, temp, filtr,
                         DOSE_PH if (filtr and i % 7 == 0) else DOSE_NONE, HOURLY))

    # Les 11 trous, répartis (pas groupés) : un import qui ne gère les vides que
    # par « saut de la fin du fichier » passerait sur un bloc contigu.
    holes = {3, 11, 17, 24, 31, 38, 46, 53, 61, 68, 74}
    for i in range(raw_count):
        dt = raw_start + timedelta(minutes=5 * i)
        if i in holes:
            lines.append(row(dt, None, None, None, True, DOSE_NONE, RAW))
        else:
            ph, orp, temp = plausible(rng, dt)
            lines.append(row(dt, ph, orp, temp, True,
                             rng.choice([DOSE_NONE] * 6 + [DOSE_PH, DOSE_ORP]), RAW))
    return lines


# ---------------------------------------------------------------------------
# 3. history-worst-bytes.csv — PIRE CAS D'OCTETS
# ---------------------------------------------------------------------------
# Décimales à 17 chiffres significatifs (le maximum que rende JSON.stringify) et
# colonnes filtrées présentes dans le fichier.
#
# ⚠️ DEUX CHIFFRES, ET TOUS DEUX JUSTES POUR UNE FORME DIFFÉRENTE — mesurés au
# node du poste, pas estimés :
#   · 149 o/point  = la forme RÉELLEMENT postée aujourd'hui par
#     `parseHistoryCsv` (7 clés : timestamp, ph, orp, temperature, filtration,
#     dosing, granularity), toutes décimales déployées. C'est le pire cas
#     ATTEIGNABLE en l'état.
#   · 212 o/point  = la MÊME charge augmentée de `phFiltered`/`orpFiltered`.
#     Ces deux colonnes existent dans DataPoint (feature-095) mais ne sont ni
#     exportées ni relues : la charge ne les porte PAS encore. Le fichier les
#     contient tout de même, pour que le jour où elles rejoignent l'import le
#     pire cas d'octets soit DÉJÀ dans les fixtures — et pour que l'invariant de
#     marge du test soit vérifié contre 212, pas contre 149.
# Les colonnes en trop sont ignorées par `parseHistoryCsv` (indexOf -> -1) :
# c'est aussi ce que ce fichier vérifie au passage.
def gen_worst_bytes():
    lines = []
    header = HEADER[:-1] + ["ph_filtered", "orp_filtered", HEADER[-1]]
    # Valeurs choisies pour que Number -> String rende 17 caracteres : un
    # arrondi court (7.9) donnerait 3 caracteres et raterait la cible.
    phs = ["7.123456789012345", "6.987654321098765", "7.000000000000001"]
    orps = ["612.3456789012345", "481.9876543210987", "555.0000000000001"]
    temps = ["26.98765432109876", "22.12345678901234", "27.00000000000001"]
    for i in range(60):
        dt = REF - timedelta(minutes=5 * (59 - i))
        cells = [
            fmt(dt), phs[i % 3], orps[i % 3], temps[i % 3],
            "1", "1", str(DOSE_PH | DOSE_ORP | DOSE_UNKNOWN),  # masque le plus long
            phs[(i + 1) % 3], orps[(i + 1) % 3],
            str(RAW),
        ]
        lines.append(",".join(cells))
    return ",".join(header), lines


# ---------------------------------------------------------------------------
# 4. history-overflow.csv — LE DÉPASSEMENT DOIT ÊTRE RAPPORTÉ
# ---------------------------------------------------------------------------
# 200 points HOURLY pour un plafond de 168 : 32 points NE PEUVENT PAS tenir.
# L'attendu n'est pas « ça marche », c'est `dropped = 32` REMONTÉ AU CLIENT.
# Une perte muette est le défaut d'origine de bug-101 : l'utilisateur croyait
# avoir restauré sa sauvegarde.
def gen_overflow():
    rng = random.Random(200)
    lines = []
    count = 200
    end = REF.replace(minute=0, second=0)
    start = end - timedelta(hours=count - 1)
    for i in range(count):
        dt = start + timedelta(hours=i)
        ph, orp, temp = plausible(rng, dt)
        lines.append(row(dt, ph, orp, temp, 8 <= dt.hour < 20, DOSE_NONE, HOURLY))
    return lines


# ---------------------------------------------------------------------------
# 5. history-legacy.csv — EXPORT <= 2.46, `dosing` BOOLÉEN SANS `dosing_mask`
# ---------------------------------------------------------------------------
# Sept colonnes, séparateur d'espace dans `datetime` : la forme EXACTE de la
# sauvegarde hors dépôt du 05/09.
# ATTENDU : `dosing=1` doit devenir 0x04 (DOSE_UNKNOWN, « voie indéterminée »),
# JAMAIS 0x01. Le lire 0x01 accuserait la pompe d'ACIDE d'injections de chlore —
# donnée fausse ET favorable, que consolidateData() figerait 75 jours dans les
# agrégats DAILY. C'est précisément le défaut clos par bug-098.
def gen_legacy():
    rng = random.Random(246)
    header = "datetime,ph,orp,temperature,filtration,dosing,granularity"
    lines = []
    raw_count = 77
    raw_start = REF - timedelta(minutes=5 * (raw_count - 1))
    hourly_end = (raw_start - timedelta(hours=1)).replace(minute=0, second=0)
    hourly_start = hourly_end - timedelta(hours=K_MAX_HOURLY - 1)

    def legacy_row(dt, ph, orp, temp, filtr, dosed, gran):
        def cell(v):
            return "" if v is None else str(v)
        return ",".join([dt.strftime("%Y-%m-%d %H:%M:%S"), cell(ph), cell(orp),
                         cell(temp), "1" if filtr else "0",
                         "1" if dosed else "0", str(gran)])

    for i in range(K_MAX_HOURLY):
        dt = hourly_start + timedelta(hours=i)
        ph, orp, temp = plausible(rng, dt)
        filtr = 8 <= dt.hour < 20
        lines.append(legacy_row(dt, ph, orp, temp, filtr, filtr and i % 5 == 0, HOURLY))
    for i in range(raw_count):
        dt = raw_start + timedelta(minutes=5 * i)
        ph, orp, temp = plausible(rng, dt)
        lines.append(legacy_row(dt, ph, orp, temp, True, i % 9 == 0, RAW))
    return header, lines


# ---------------------------------------------------------------------------
# 6. history-hostile.csv — FICHIER MALTRAITÉ
# ---------------------------------------------------------------------------
# BOM UTF-8, délimiteur `;`, fins de ligne CRLF, virgule décimale, cellules
# vides, ligne TRONQUÉE (moins de colonnes que l'en-tête) et ligne sans
# timestamp exploitable. Attendu : les lignes récupérables passent, les
# irrécupérables sont SAUTÉES — jamais un pH 0 inventé, jamais un plantage.
# ⚠️ Avec `;` comme délimiteur, la virgule décimale n'est PAS ambiguë : c'est
# exactement la sortie d'un Excel en locale française, le tableur avec lequel un
# utilisateur va rouvrir sa sauvegarde.
def gen_hostile():
    header = ";".join(HEADER)
    lines = [
        "2026-09-05T10:00:00;7,15;548;26,4;1;0;0;1",       # virgule décimale
        "2026-09-05T11:00:00;7,20;561;26,6;1;1;1;1",
        "2026-09-05T12:00:00;;;;1;0;0;1",                   # trois mesures vides
        "2026-09-05T13:00:00;7,05;533;27,1;0;1;2;1",
        "2026-09-05T14:00:00;7,10;540",                      # ligne TRONQUEE
        ";7,30;600;27,5;1;0;0;1",                            # datetime VIDE -> saut
        "pas-une-date;7,30;600;27,5;1;0;0;1",                # datetime illisible -> saut
        "2026-09-05T14:05:00;7,12;545;26,9;1;1;4;0",         # RAW, masque 0x04
        "2026-09-05T14:10:00;7,13;546;26,9;1;1;4;0",
    ]
    return header, lines


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "test", "fixtures")
    os.makedirs(out, exist_ok=True)
    head = ",".join(HEADER)
    print("Fixtures d'import d'historique -> %s" % out)

    write(os.path.join(out, "history-339.csv"), head, gen_339())
    write(os.path.join(out, "history-245.csv"), head, gen_245())
    wb_head, wb_lines = gen_worst_bytes()
    write(os.path.join(out, "history-worst-bytes.csv"), wb_head, wb_lines)
    write(os.path.join(out, "history-overflow.csv"), head, gen_overflow())
    lg_head, lg_lines = gen_legacy()
    write(os.path.join(out, "history-legacy.csv"), lg_head, lg_lines)
    ho_head, ho_lines = gen_hostile()
    write(os.path.join(out, "history-hostile.csv"), ho_head, ho_lines,
          eol="\r\n", bom=True)
    print("OK — %d fichiers. Pire cas structurel = %d points." % (6, K_TOTAL_WORST))


if __name__ == "__main__":
    main()
