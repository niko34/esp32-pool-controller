## Métadonnées
- **ID** : bug-182
- **Slug** : pages-autonomes-sous-le-seuil-de-contraste
- **Type** : bug — accessibilité
- **Statut** : todo
- **Priorité** : normale
- **Version cible** : 2.89.0
- **Spec(s) liée(s)** : `feature-151` D3-b1 (découvert en concevant la page de secours), `feature-169`

## Domaines touchés
- [ ] Capteurs · [ ] Régulation · [ ] Filtration / Éclairage · [ ] Web — routes
- [ ] MQTT · [ ] UART · [ ] Authentification (apparence seule) · [ ] Historique
- [x] Frontend (`data/`)

## Agents requis
- [x] spec-reader · [ ] architect · [ ] pool-chemistry · [x] ux-ui-designer
- [ ] embedded-developer · [x] web-ui-developer · [x] test-engineer · [x] code-reviewer · [x] doc-writer

## Description
Les **trois pages autonomes** du produit — `login.html`, `wizard.html`, `wifi.html` — partagent un
style **sous les seuils de contraste WCAG AA**, et des champs à **14 px** qui font **zoomer iOS** au
focus.

## Contexte
**Mesuré le 2026-11-15** par `ux-ui-designer`, en concevant la page de secours de `feature-151`
D3-b1 — qui a justement **écarté ce style** pour cette raison.

| Élément | Couleurs | Ratio | Verdict |
|---|---|---|---|
| Texte de bouton | blanc sur `#5c7cdb` → `#687ad8` | **≈ 3,9:1** | ⛔ **sous AA 4,5:1** ⚠️ 14 px gras n'est **pas** du « grand texte » |
| Bordure de champ | `#e5e7eb` sur `#fafbfc` | **≈ 1,2:1** | ⛔ **sous 3:1** (WCAG 1.4.11, composants d'interface) |
| Texte d'erreur | `#dc2626` sur blanc | ≈ 4,8:1 | ✅ conforme, ⚠️ **sans marge** (≈ 4,5 sur fond teinté) |
| Champs | `font-size: 14px` | — | ⚠️ **iOS zoome au focus** sous 16 px |

⭐ **Correction honnête de l'agent** : sa première version annonçait le texte d'erreur **sous AA** ;
mesure faite, il est à **4,8:1**. ⇒ Le défaut est **plus étroit** qu'annoncé, et mieux cerné.

**Vérifié, le style est partagé par les trois fichiers** :
```
data/login.html:177   background: linear-gradient(135deg, #5c7cdb 0%, #687ad8 100%);
data/login.html:136   border: 1.5px solid #e5e7eb;   :139  background: #fafbfc;
data/wizard.html:61   même dégradé        (8 occurrences du couple de bleus)
data/wifi.html:61     même dégradé
```

⚠️ **Ce sont les pages qu'on voit EN PREMIER**, et celles qu'on utilise **quand quelque chose ne va
pas** : la connexion, l'assistant de configuration initiale, et la configuration Wi-Fi en mode
point d'accès. ⇒ Un utilisateur en plein soleil au bord du bassin, sur un téléphone, les lit dans
les pires conditions possibles.

## Comportement attendu
1. **Tous les textes et composants atteignent AA** : 4,5:1 pour le texte, **3:1** pour les bordures
   et les composants d'interface (WCAG 1.4.11).
2. **Champs à 16 px minimum** ⇒ ⛔ plus de zoom iOS au focus.
3. ⛔ **Aucun signal porté par la couleur seule** (daltonisme) — vérifier au passage.
4. **Le focus reste visible** sur tous les contrôles.
5. ⚠️ **Les trois pages sont traitées ENSEMBLE** : elles partagent le style, et n'en corriger qu'une
   créerait une **troisième apparence** dans un produit qui en a déjà deux.

### Cas limites
- Mode **point d'accès** (`wifi.html`, `wizard.html`) : l'utilisateur n'a **pas** de réseau, souvent
  en extérieur. ⇒ C'est le pire contexte de lecture du produit.
- ⚠️ `feature-151` D3-b1 ajoute une **quatrième** page autonome (la page de secours), qui utilise
  déjà la palette **conforme** de `app.css`. ⇒ Après ce lot, elle sera la **seule** conforme si rien
  n'est fait — à aligner dans le même geste, ou à déclarer.

## Comportement actuel
Les trois pages portent un style **local**, hérité et dupliqué, sans lien avec les jetons de
`data/app.css` (qui, eux, sont conformes : `--accent #2563eb` donne **5,2:1**).

## Contraintes techniques
- ⚠️ **Ces pages ne peuvent pas charger `app.css`** : elles sont autonomes par conception (servies
  avant authentification, ou en mode point d'accès). ⇒ La palette se **recopie**, elle ne se
  partage pas. ⛔ Ne pas tenter de les faire dépendre d'une feuille commune.
- Budget FS `spiffs` 512 KB (actuellement **41 %**).
- ⚠️ **RESPONSIVE** : 390 px d'abord.
- Vanilla, aucune dépendance.

## Fichiers potentiellement concernés
- `data/login.html`, `data/wizard.html`, `data/wifi.html`
- `docs/DESIGN_SYSTEM.md` — ⭐ y **déclarer la palette des pages autonomes** comme copie figée, et
  **pourquoi** elle ne peut pas être partagée

## Livrables — check-list
- [ ] A1 — Palette conforme recopiée depuis les jetons de `app.css` dans les **trois** pages
- [ ] A2 — Champs à **16 px minimum**
- [ ] A3 — Bordures de champ à **≥ 3:1**
- [ ] A4 — Focus visible, aucun signal porté par la couleur seule
- [ ] C1 — `docs/DESIGN_SYSTEM.md` : la palette des pages autonomes, **déclarée comme copie figée**
- [ ] C2 — `CHANGELOG.md`
- [ ] D1 — ⭐ **Ratios RE-MESURÉS** après correction, et écrits dans la spec
- [ ] D2 — Rendu vérifié **par capture** à 390 px sur les trois pages

## ADR attendu ?
- **Probable** : non — correction d'apparence, sans alternative structurante

## Critères d'acceptation
- [ ] Les **trois** pages passent AA : texte ≥ 4,5:1, composants ≥ 3:1, **ratios mesurés**
- [ ] ⛔ Plus aucun champ sous 16 px
- [ ] Les trois pages se **ressemblent entre elles** et ressemblent à l'application
- [ ] `./build_fs.sh` passe sous la partition

## Plan de test manuel
1. Capture des trois pages à 390 px, avant et après.
2. Sur un iPhone : vérifier qu'aucun champ ne déclenche de zoom au focus.
3. Mode point d'accès : lisibilité de `wifi.html` **en extérieur**.

## Notes pour agent
- ⚠️ Défaut **PRÉEXISTANT**, mesuré en concevant autre chose. ⛔ Ce n'est pas une régression.
- ⚠️ **Ne pas chercher à partager une feuille de style** : l'autonomie de ces pages est une
  propriété voulue, pas un oubli.
- ⚠️ `ux-ui-designer` passe **avant** `web-ui-developer`, avec mandat sur **les trois pages
  ensemble** — et sur **tout choix visuel**, même non demandé.
