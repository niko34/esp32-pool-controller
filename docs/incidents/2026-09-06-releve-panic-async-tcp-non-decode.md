# Relevé du 06/09/2026 — coredump `async_tcp` NON DÉCODÉ

**Ce dump n'a pas de trace décodée.** L'ELF correspondant n'a pas été retrouvé.
Il est archivé quand même : il aurait été **écrasé au crash suivant**, et il est
irremplaçable.

## Ce que l'appareil en dit

Relevé le 2026-09-06 à 19:15 locale sur `192.168.1.48`, via `/coredump/download`.

```json
{"available":true,"task":"async_tcp","pc":1074329872,
 "exc_cause":0,"exc_cause_str":"IllegalInstruction","exc_vaddr":0}
```

- Taille : **26 884 o** — distincte des trois incidents déjà archivés
- `pc = 0x4008F8D0`
- Empreinte de l'image applicative inscrite dans le dump : **`2b353dc58`**

## Pourquoi il n'est pas décodé

`esp-coredump` vérifie le SHA256 de l'image applicative et refuse tout ELF qui ne
correspond pas. Deux ELF ont été essayés, **tous deux rejetés** :

| ELF essayé | `app SHA256` | Verdict |
|---|---|---|
| Asset de la release `v2.42.0` | `65a2d593a` | refusé |
| `builds/2.42.0/…/firmware_20260905_193400.elf` (Mac) | `65a2d593a` | refusé — **fichier identique au précédent** (même MD5) |
| Build local `2.42.1` (contrôle) | `1a1009445` | refusé — confirme que la valeur est propre à l'ELF |

L'appareil exécutait au moment du relevé la **v2.42.0 compilée le 05/09 à 19:33:50**,
d'empreinte `65a2d593a`. Le dump portant `2b353dc58`, **le crash a eu lieu sous un
firmware antérieur** et le dump a survécu à la mise à jour — il vit dans sa propre
partition.

**Datation probable** : après le relevé du 05/09 15:35 (qui avait extrait le dump
`2026-09-05-panic-fopen-historique`, v2.40.0) et avant le déploiement du build de
19:33:50. Donc **v2.41.0 ou un build antérieur de la 2.42.0**, dans la soirée du 05/09.

## Ce qu'il faut pour le décoder

L'ELF dont `esp-coredump` rapporte `app SHA256(2b353dc58)`. Il n'existe que dans
`builds/` sur le Mac (rétention : 5 builds par version). Procédure d'identification
dans [README.md](README.md).

## Pourquoi il n'appartient PAS à la famille des trois autres

Les trois incidents archivés sont des `abort()` de `lock_init_generic` faute d'un
verrou newlib, sur un `fopen`. Celui-ci est une **instruction illégale** sur
`async_tcp`. Signature différente, mécanisme probablement différent.

## Note de méthode

Le redémarrage constaté le 06/09 vers 14:19 locale n'a **pas** produit de coredump —
celui-ci lui est antérieur. Sa cause avait pourtant été journalisée au démarrage par
`heapRingDumpIfCrash()`, mais la ligne avait déjà été évincée du journal persistant
au moment du relevé : sa fenêtre ne couvrait que **28 minutes**. Voir
[`feature-109`](../../specs/features/done/feature-109-decouverte-ha-une-ligne-au-journal.md).
