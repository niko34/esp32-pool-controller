"""Petit routeur de labyrinthe pour les RETOUCHES de la carte principale (2026-11-28).

Il cherche, sur les deux faces, le chemin le plus court entre un point et d'autres points du
même réseau, en évitant tout le cuivre des AUTRES réseaux. Il ne remplace pas un vrai outil de
routage : il sert à ajouter quelques liaisons dans une carte déjà tracée, sans y toucher.
⚠️ Le juge reste le contrôle des règles de KiCad : lancer `kicad-cli pcb drc` après coup.
⚠️ À l'enregistrement, KiCad donne à une piste le réseau du cuivre qu'elle TOUCHE : une piste posée par script contre
   un autre réseau change de nom sans rien dire (constaté le 2026-11-28). C'est « 0 liaison manquante, 0 écart entre
   schéma et carte » qui prouve que rien n'a été détourné — pas le fait que le script se soit terminé.

Ce qu'il respecte (kicad/REGLES-CEM-ET-FIABILITE.md) :
- aucune piste à moins de MARGE_BORD du bord de la carte (une piste au bord sort du plan de masse) ;
- rien dans la zone interdite de l'antenne, rien près des trous de fixation ;
- un via coûte l'équivalent de COUT_VIA mm de piste : le chemin en pose le moins possible ;
- aucun via sous un composant ;
- les plans de masse des deux faces ne sont PAS des obstacles : ils se referment autour des pistes.

Dépendances : pcbnew (KiCad 9) et numpy.

Usage, depuis un script de retouche :
    import routeur
    r = routeur.Routeur(carte)
    r.relier("/RESEAU", (x1, y1, "F"), [(x2, y2, "F"), (x3, y3, "B")], largeur=0.25)
    r.vers_le_plan("GND", (x, y, "F"), largeur=0.4)        # petite piste + via vers le plan interne
Les coordonnées sont celles de la carte (mm). "F" = dessus, "B" = dessous.
"""
import heapq, math
from array import array
import numpy as np
import pcbnew

PAS = 0.125            # mm : pas de la grille de recherche
ECART = 0.25           # mm : écart minimal gardé avec le cuivre d'un autre réseau (la carte exige 0,2)
CONFORT = 0.40         # mm : écart recherché d'abord ; ECART n'est utilisé que s'il n'y a pas de chemin
MARGE_BORD = 1.0       # mm : aucune piste plus près du bord de la carte
COUT_VIA = 8.0         # mm de piste équivalents
COUT_VIRAGE = 1.0      # mm de piste équivalents : évite les tracés en escalier
DIAM_VIA, TROU_VIA = 0.6, 0.3
POIDS = 1.2            # la recherche vise le but un peu plus fort que nécessaire : dix fois moins de cases visitées, tracé au plus 20 % plus long
MAX_CASES = 1500000    # garde-fou : au-delà, on renonce plutôt que d'occuper la machine
TOUR = 25.0            # mm : la recherche ne s'écarte pas davantage du départ et des buts
SOUS = ("BT1",)        # pièces sous lesquelles un passage est admis : le support de pile surplombe la carte, seules ses pastilles comptent
T = pcbnew.ToMM; mm = pcbnew.FromMM
DIRS = [(1, 0), (1, 1), (0, 1), (-1, 1), (-1, 0), (-1, -1), (0, -1), (1, -1)]


def _dilate(g, rayon):
    """Épaissit de `rayon` mm tout ce qui est vrai dans la grille g (disque)."""
    k = int(math.floor(rayon / PAS + 1e-9))
    if k <= 0: return g.copy()
    out = g.copy(); ny, nx = g.shape
    for dj in range(-k, k + 1):
        di = int(math.floor(math.sqrt(max(0.0, (rayon / PAS) ** 2 - dj * dj)) + 1e-9))
        c = np.cumsum(np.pad(g, ((0, 0), (di, di))).astype(np.int32), axis=1)
        ligne = (c[:, 2 * di:] - np.pad(c, ((0, 0), (1, 0)))[:, :nx]) > 0      # réunion des décalages horizontaux de -di à +di
        if dj >= 0: out[dj:, :] |= ligne[:ny - dj, :]
        else: out[:ny + dj, :] |= ligne[-dj:, :]
    return out


class Routeur:
    def __init__(self, carte):
        self.b = carte
        e = carte.GetBoardEdgesBoundingBox(); tour = MARGE_BORD + 1.0      # la grille déborde de la carte : c'est ce « dehors » qui tient les pistes loin du bord
        self.x0, self.y0 = round(T(e.GetLeft()) - tour, 3), round(T(e.GetTop()) - tour, 3)
        self.nx = int(round((T(e.GetRight()) + tour - self.x0) / PAS)) + 1; self.ny = int(round((T(e.GetBottom()) + tour - self.y0) / PAS)) + 1
        self.F, self.B = pcbnew.F_Cu, pcbnew.B_Cu

    # ---------- grille
    def ij(self, x, y): return int(round((x - self.x0) / PAS)), int(round((y - self.y0) / PAS))
    def xy(self, i, j): return round(self.x0 + i * PAS, 4), round(self.y0 + j * PAS, 4)
    def _vide(self): return np.zeros((self.ny, self.nx), bool)
    def _rect(self, g, xa, ya, xb, yb, v=True):
        i0, j0 = self.ij(xa, ya); i1, j1 = self.ij(xb, yb)
        i0, j0, i1, j1 = max(i0, 0), max(j0, 0), min(i1, self.nx - 1), min(j1, self.ny - 1)
        if i0 <= i1 and j0 <= j1: g[j0:j1 + 1, i0:i1 + 1] = v
    def _disque(self, g, x, y, r):
        i0, j0 = self.ij(x - r, y - r); i1, j1 = self.ij(x + r, y + r)
        i0, j0, i1, j1 = max(i0, 0), max(j0, 0), min(i1, self.nx - 1), min(j1, self.ny - 1)
        if i0 > i1 or j0 > j1: return
        xs = self.x0 + np.arange(i0, i1 + 1) * PAS; ys = self.y0 + np.arange(j0, j1 + 1) * PAS
        g[j0:j1 + 1, i0:i1 + 1] |= ((xs[None, :] - x) ** 2 + (ys[:, None] - y) ** 2) <= r * r + 1e-9
    def _trait(self, g, xa, ya, xb, yb, r):
        n = max(1, int(math.hypot(xb - xa, yb - ya) / (PAS / 2)))
        for k in range(n + 1): self._disque(g, xa + (xb - xa) * k / n, ya + (yb - ya) * k / n, r)

    # ---------- relevé de la carte pour un réseau donné
    def _releve(self, reseau):
        """Cuivre brut des AUTRES réseaux sur chaque face, zones où rien ne passe, zones sans via."""
        cu = {self.F: self._vide(), self.B: self._vide()}; interdit = self._vide(); sans_via = self._vide(); a_soi = {self.F: [], self.B: []}
        for t in self.b.GetTracks():
            autre = t.GetNetname() != reseau
            if t.Type() == pcbnew.PCB_VIA_T:
                x, y = T(t.GetPosition().x), T(t.GetPosition().y); r = T(t.GetWidth(self.F)) / 2
                if autre:
                    for c in (self.F, self.B): self._disque(cu[c], x, y, r)
                self._disque(sans_via, x, y, T(t.GetDrill()) / 2 + 0.25 + TROU_VIA / 2)      # trou à trou, même pour un via du même réseau
            elif autre and t.GetLayer() in cu:
                self._trait(cu[t.GetLayer()], T(t.GetStart().x), T(t.GetStart().y), T(t.GetEnd().x), T(t.GetEnd().y), T(t.GetWidth()) / 2)
        for fp in self.b.GetFootprints():
            fp.BuildCourtyardCaches(); m = DIAM_VIA / 2 + 0.15      # le passage entier reste hors de l'encombrement de la pièce : la soudure ne peut pas y couler
            for c in (self.F, self.B):
                cb = fp.GetCourtyard(c).BBox()
                if cb.GetWidth() > 0 and fp.GetReference() not in SOUS: self._rect(sans_via, T(cb.GetLeft()) - m, T(cb.GetTop()) - m, T(cb.GetRight()) + m, T(cb.GetBottom()) + m)
            for q in fp.Pads():
                bb = q.GetBoundingBox(); box = (T(bb.GetLeft()), T(bb.GetTop()), T(bb.GetRight()), T(bb.GetBottom()))
                x, y = T(q.GetPosition().x), T(q.GetPosition().y); att = q.GetAttribute()
                trav = att in (pcbnew.PAD_ATTRIB_PTH, pcbnew.PAD_ATTRIB_NPTH)
                couches = (self.F, self.B) if trav else tuple(c for c in (self.F, self.B) if q.IsOnLayer(c))
                if att == pcbnew.PAD_ATTRIB_NPTH:      # trou nu (fixation) : on s'en tient à 2 mm, sur les deux faces
                    self._disque(interdit, x, y, T(q.GetDrillSize().x) / 2 + 2.0); continue
                if q.GetNetname() == reseau:
                    for c in couches: a_soi[c].append(box)
                else:
                    for c in couches: self._rect(cu[c], *box)
                if trav: self._disque(sans_via, x, y, T(q.GetDrillSize().x) / 2 + 0.25 + TROU_VIA / 2)
                else: self._rect(sans_via, box[0] - m - 0.15, box[1] - m - 0.15, box[2] + m + 0.15, box[3] + m + 0.15)      # jamais de passage dans une pastille à souder, ni contre elle
            for z in fp.Zones():                      # zone interdite d'une empreinte : l'antenne du module
                if z.GetIsRuleArea() and (z.GetDoNotAllowTracks() or z.GetDoNotAllowVias()): zb = z.GetBoundingBox(); self._rect(interdit, T(zb.GetLeft()), T(zb.GetTop()), T(zb.GetRight()), T(zb.GetBottom()))
        for z in self.b.Zones():      # une zone qui n'interdit que le remplissage de cuivre ne gêne pas les pistes
            if z.GetIsRuleArea() and (z.GetDoNotAllowTracks() or z.GetDoNotAllowVias()): zb = z.GetBoundingBox(); self._rect(interdit, T(zb.GetLeft()), T(zb.GetTop()), T(zb.GetRight()), T(zb.GetBottom()))
        # intérieur de la carte, d'après son contour
        contour = pcbnew.SHAPE_POLY_SET(); self.b.GetBoardPolygonOutlines(contour)
        o = contour.Outline(0); pts = [(T(o.CPoint(k).x), T(o.CPoint(k).y)) for k in range(o.PointCount())]
        X, Y = np.meshgrid(self.x0 + np.arange(self.nx) * PAS, self.y0 + np.arange(self.ny) * PAS)
        dedans = self._vide()
        for (xa, ya), (xb, yb) in zip(pts, pts[1:] + pts[:1]):
            if ya != yb: dedans ^= ((ya > Y) != (yb > Y)) & (X < (xb - xa) * (Y - ya) / (yb - ya) + xa)
        return cu, interdit, sans_via, ~dedans, a_soi

    def _grilles(self, reseau, largeur, ecart):
        cu, interdit, sans_via, dehors, a_soi = self._releve(reseau)
        bloc = np.zeros((2, self.ny, self.nx), bool)
        fixe = _dilate(interdit, largeur / 2) | _dilate(dehors, MARGE_BORD + largeur / 2 + PAS)
        for k, c in enumerate((self.F, self.B)):
            bloc[k] = _dilate(cu[c], ecart + largeur / 2 + PAS) | fixe      # + PAS : la grille arrondit à une demi-case près, des deux côtés
            for (xa, ya, xb, yb) in a_soi[c]:          # dans une pastille du réseau, la piste n'ajoute aucun cuivre : toujours permis
                if xb - xa > largeur and yb - ya > largeur: self._rect(bloc[k], xa + largeur / 2, ya + largeur / 2, xb - largeur / 2, yb - largeur / 2, False)
        r = DIAM_VIA / 2
        pas_de_via = _dilate(cu[self.F] | cu[self.B], ecart + r + PAS) | _dilate(interdit, r) | _dilate(dehors, MARGE_BORD + r + PAS) | sans_via
        return bloc, pas_de_via

    def points(self, reseau, sauf=()):
        """Tous les points où l'on peut se raccorder au réseau : pastilles, vias, bouts de pistes. `sauf` : repères de pièces à ignorer."""
        P = []; k = {self.F: "F", self.B: "B"}
        for t in self.b.GetTracks():
            if t.GetNetname() != reseau: continue
            if t.Type() == pcbnew.PCB_VIA_T: P += [(T(t.GetPosition().x), T(t.GetPosition().y), c) for c in "FB"]
            elif t.GetLayer() in k: P += [(T(p.x), T(p.y), k[t.GetLayer()]) for p in (t.GetStart(), t.GetEnd())]
        for fp in self.b.GetFootprints():
            if fp.GetReference() in sauf: continue
            for q in fp.Pads():
                if q.GetNetname() == reseau: P += [(T(q.GetPosition().x), T(q.GetPosition().y), k[c]) for c in (self.F, self.B) if q.IsOnLayer(c)]
        return sorted(set(P))

    # ---------- recherche
    def _chemin(self, bloc, pas_de_via, depart, cibles, couches, via_final=False):
        """Plus court chemin sur la grille (A*). Rend la liste des (face, j, i), ou None.
        Mémoire bornée : trois tableaux de la taille de la grille, quoi qu'il arrive (un premier jet à
        dictionnaires a pris 1,3 Go et bloqué la machine, le 2026-11-28)."""
        ny, nx = self.ny, self.nx; N = ny * nx; k_de = {"F": 0, "B": 1}
        bl = bloc.copy(); bl[:, 0, :] = bl[:, -1, :] = True; bl[:, :, 0] = bl[:, :, -1] = True      # pourtour fermé : pas de test de limites
        if "F" not in couches: bl[0] = True
        if "B" not in couches: bl[1] = True
        i0, j0 = self.ij(depart[0], depart[1]); s = k_de[depart[2]] * N + j0 * nx + i0
        buts = set()
        if via_final: h = array("f", bytes(4 * N))
        else:
            J, I = np.mgrid[0:ny, 0:nx]; hh = np.full((ny, nx), 1e9, np.float32)
            for (x, y, c) in cibles:
                a, b2 = self.ij(x, y); buts.add(k_de[c] * N + b2 * nx + a); bl[k_de[c], b2, a] = False
                dx, dy = np.abs(I - a), np.abs(J - b2); hh = np.minimum(hh, np.maximum(dx, dy) + 0.4142 * np.minimum(dx, dy))
            h = array("f"); h.frombytes((hh * POIDS).astype(np.float32).tobytes())
        bl[k_de[depart[2]], j0, i0] = False
        if not via_final:                              # on ne cherche pas plus loin que TOUR mm autour du départ et des buts
            ii = [i0] + [(q % N) % nx for q in buts]; jj = [j0] + [(q % N) // nx for q in buts]; t = int(TOUR / PAS)
            bl[:, :max(min(jj) - t, 0), :] = True; bl[:, max(jj) + t + 1:, :] = True; bl[:, :, :max(min(ii) - t, 0)] = True; bl[:, :, max(ii) + t + 1:] = True
        B = bl.tobytes(); PV = pas_de_via.tobytes()
        cout = array("d", [1e18]) * (2 * N); prec = array("i", [-1]) * (2 * N); sens = array("b", [8]) * (2 * N)      # "d" exprès : en simple précision, la valeur relue serait plus petite que celle écrite et la case serait sautée
        pas = [1, nx + 1, nx, nx - 1, -1, -nx - 1, -nx, -nx + 1]                 # mêmes sens que DIRS
        long_ = [1.0, 1.4142, 1.0, 1.4142, 1.0, 1.4142, 1.0, 1.4142]
        via_pas, vir = COUT_VIA / PAS, COUT_VIRAGE / PAS
        cout[s] = 0.0; tas = [(h[s % N], 0.0, s)]; vus = 0; pousser, tirer = heapq.heappush, heapq.heappop
        while tas:
            f, c0, n = tirer(tas)
            if c0 > cout[n]: continue
            cell = n % N
            if (via_final and not PV[cell] and n != s) or n in buts:
                ch = []
                while n != -1: l = n // N; ch.append((l, (n % N) // nx, (n % N) % nx)); n = prec[n]
                return ch[::-1]
            vus += 1
            if vus > MAX_CASES: return None
            d0 = sens[n]
            for d in range(8):
                m = n + pas[d]
                if B[m]: continue
                if d & 1 and (B[n + pas[d - 1]] or B[n + pas[(d + 1) & 7]]): continue       # diagonale : pas de coin coupé
                c1 = c0 + long_[d]
                if d0 != 8 and d != d0:
                    e = (d - d0) & 7
                    if e > 4: e = 8 - e
                    if e > 3: continue                                                        # pas de demi-tour
                    c1 += vir * (e if e < 3 else 6)                                           # un angle aigu coûte cher
                if c1 < cout[m]: cout[m] = c1; prec[m] = n; sens[m] = d; pousser(tas, (c1 + h[m % N], c1, m))
            if not via_final and not PV[cell]:
                m = n + N if n < N else n - N
                if not B[m]:
                    c1 = c0 + via_pas
                    if c1 < cout[m]: cout[m] = c1; prec[m] = n; sens[m] = 8; pousser(tas, (c1 + h[cell], c1, m))
        return None

    # ---------- écriture sur la carte
    def _net(self, reseau):
        n = self.b.FindNet(reseau)
        if n is None: raise SystemExit("routeur : le réseau %s n'existe pas sur la carte" % reseau)
        return n
    def _via(self, net, x, y):
        v = pcbnew.PCB_VIA(self.b); v.SetPosition(pcbnew.VECTOR2I(mm(x), mm(y))); v.SetDrill(mm(TROU_VIA)); v.SetWidth(self.F, mm(DIAM_VIA))
        v.SetLayerPair(self.F, self.B); v.SetNet(net); self.b.Add(v)
    def _piste(self, net, c, p, q, largeur):
        if math.hypot(q[0] - p[0], q[1] - p[1]) < 1e-6: return 0.0
        t = pcbnew.PCB_TRACK(self.b); t.SetStart(pcbnew.VECTOR2I(mm(p[0]), mm(p[1]))); t.SetEnd(pcbnew.VECTOR2I(mm(q[0]), mm(q[1])))
        t.SetWidth(mm(largeur)); t.SetLayer(c); t.SetNet(net); self.b.Add(t); return math.hypot(q[0] - p[0], q[1] - p[1])
    def _poser(self, reseau, ch, largeur, exacts):
        """Écrit le chemin : fusionne les pas alignés, pose les vias, raccorde aux points exacts (hors grille)."""
        net = self._net(reseau); face = (self.F, self.B); lon = 0.0; nv = 0; morceaux = [[ch[0]]]
        for a, b2 in zip(ch, ch[1:]):
            if a[0] != b2[0]: self._via(net, *self.xy(a[2], a[1])); nv += 1; morceaux.append([b2])
            else: morceaux[-1].append(b2)
        for m in morceaux:
            pts = [self.xy(n[2], n[1]) for n in m]; simple = [pts[0]]
            for k in range(1, len(pts) - 1):
                (xa, ya), (xb, yb), (xc, yc) = simple[-1], pts[k], pts[k + 1]
                if abs((xb - xa) * (yc - yb) - (yb - ya) * (xc - xb)) > 1e-9: simple.append(pts[k])
            if len(pts) > 1: simple.append(pts[-1])
            for p, q in zip(simple, simple[1:]): lon += self._piste(net, face[m[0][0]], p, q, largeur)
            for bout in (simple[0], simple[-1]):
                if bout in exacts: lon += self._piste(net, face[m[0][0]], bout, exacts[bout], largeur)
        return lon, nv

    def relier(self, reseau, depart, cibles, largeur=0.25, couches="FB", eviter=()):
        """Trace la liaison. `eviter` : rectangles (x0, y0, x1, y1) où cette liaison ne doit pas passer.
        Rend (longueur en mm, nombre de vias, écart tenu)."""
        exacts = {self.xy(*self.ij(x, y)): (x, y) for (x, y, c) in [depart] + list(cibles)}
        for ecart in (CONFORT, ECART):
            bloc, pas_de_via = self._grilles(reseau, largeur, ecart)
            for r in eviter: self._rect(bloc[0], *r); self._rect(bloc[1], *r)
            ch = self._chemin(bloc, pas_de_via, depart, cibles, couches)
            if ch is not None:
                lon, nv = self._poser(reseau, ch, largeur, exacts); return round(lon, 2), nv, ecart
        raise SystemExit("routeur : aucun chemin pour %s de %s vers %s" % (reseau, depart, cibles))

    def vers_le_plan(self, reseau, depart, largeur=0.4):
        """Relie un point au plan interne de son réseau : la plus courte piste jusqu'à un endroit où un via tient. Rend la place du via."""
        exacts = {self.xy(*self.ij(depart[0], depart[1])): (depart[0], depart[1])}
        for ecart in (CONFORT, ECART):
            bloc, pas_de_via = self._grilles(reseau, largeur, ecart)
            ch = self._chemin(bloc, pas_de_via, depart, [], depart[2], via_final=True)
            if ch is not None:
                self._poser(reseau, ch, largeur, exacts); x, y = self.xy(ch[-1][2], ch[-1][1]); self._via(self._net(reseau), x, y); return x, y
        raise SystemExit("routeur : aucun endroit pour un via %s près de %s" % (reseau, depart))
