/* Centre de contrôle — vue en LECTURE SEULE.
 *
 * Aucune requête autre que GET n'est émise, et le serveur n'a de toute façon
 * aucune route qui écrive : `do_POST` n'existe pas dans son code.
 *
 * Le flux SSE transporte un SIGNAL, pas l'état. Le navigateur redemande
 * `/api/etat` quand il reçoit le signal — un état poussé et un état demandé
 * pourraient diverger, et il faudrait alors décider lequel fait foi.
 */
"use strict";

const $ = (id) => document.getElementById(id);

/* Tout ce qui vient du dépôt — titres de specs, statuts, noms de fichiers,
 * sujets de commit — est du texte écrit par des humains et des agents. Il passe
 * par `textContent`, jamais par `innerHTML`. */
function el(balise, classe, texte) {
  const noeud = document.createElement(balise);
  if (classe) noeud.className = classe;
  if (texte !== undefined && texte !== null) noeud.textContent = String(texte);
  return noeud;
}

function vider(noeud) {
  while (noeud.firstChild) noeud.removeChild(noeud.firstChild);
}

function heure(iso) {
  if (!iso) return "";
  const t = String(iso);
  return t.length >= 19 ? t.slice(11, 19) : t;
}

function duree(ms) {
  if (ms === null || ms === undefined) return null;   /* pas mesurable ≠ zéro */
  if (ms < 1000) return ms + " ms";
  if (ms < 60000) return (ms / 1000).toFixed(1) + " s";
  const minutes = Math.floor(ms / 60000);
  return minutes + " min " + Math.round((ms % 60000) / 1000) + " s";
}

/* Le champ « Priorité » est du Markdown écrit à la main. Il arrivait brut dans
 * l'interface — « **Haute** — un crash n », « ~~Moyenne — fausse une » — avec
 * les astérisques visibles et une coupure au milieu d'un mot.
 *
 * On retire l'emphase, on garde la PREMIÈRE clause (avant le tiret cadratin ou
 * la parenthèse), et on coupe sur une frontière de mot. Le texte intégral reste
 * accessible en infobulle : on raccourcit l'affichage, on ne perd rien. */
function nettoyerPriorite(brut) {
  if (!brut) return null;
  let texte = String(brut)
    .replace(/\*\*|~~|`|__/g, "")
    .replace(/\s+/g, " ")
    .trim();
  const coupure = texte.search(/\s[—–-]\s|\s\(/);
  if (coupure > 0) texte = texte.slice(0, coupure);
  texte = texte.replace(/[,;:.]+$/, "").trim();
  if (texte.length > 20) {
    const espace = texte.lastIndexOf(" ", 20);
    texte = texte.slice(0, espace > 8 ? espace : 20) + "…";
  }
  return texte || null;
}

/* ---------------------------------------------------------------- agents -- */
function rendreAgents(agents) {
  const liste = $("agents");
  vider(liste);
  agents.forEach((agent) => {
    const ligne = el("li", agent.actif_sur ? "actif" : "");
    const gauche = el("div");
    gauche.appendChild(el("span", "nom", agent.nom));
    if (agent.actif_sur) gauche.appendChild(el("span", "plume", " ● en cours"));
    ligne.appendChild(gauche);
    const droite = el("div");
    droite.appendChild(el("span", "modele", agent.modele || "—"));
    droite.appendChild(el("span", "plume", agent.peut_ecrire ? " ✎" : " ·"));
    ligne.appendChild(droite);
    ligne.title = (agent.description || agent.nom) +
      "\nOutils : " + (agent.outils.join(", ") || "—") +
      (agent.peut_deleguer ? "\nPeut déléguer (Task)" : "\nNe peut pas déléguer (pas de Task)");
    liste.appendChild(ligne);
  });
  $("note-agents").textContent =
    agents.length + " agents découverts · ✎ = peut écrire";
}

/* --------------------------------------------------------------- système -- */
function rendreSysteme(etat) {
  const dl = $("systeme");
  vider(dl);
  const lignes = [
    ["Dépôt", etat.git.depot ? (etat.git.branche || "?") : "absent"],
    ["Fichiers touchés", etat.git.touches.length],
    ["Divergences", etat.divergences],
    ["Workflows", etat.workflows.length],
    ["Configuration", etat.config_lue ? "lue" : "défauts"],
    ["Commandes", etat.commandes_declarees.length],
    ["Budget max", (etat.garde_fous.budget_max_usd ?? "—") + " $"],
    ["Écrivains", etat.garde_fous.ecrivains_simultanes ?? "—"],
  ];
  lignes.forEach(([cle, valeur]) => {
    dl.appendChild(el("dt", "", cle));
    dl.appendChild(el("dd", "", valeur));
  });
}

/* ---------------------------------------------------------------- kanban -- */
let dernierEtat = null;

function specCorrespond(spec, terme, divergencesSeules) {
  if (divergencesSeules && !spec.divergence) return false;
  if (!terme) return true;
  const champs = [spec.id, spec.titre, spec.statut_texte, spec.priorite, spec.type];
  return champs.some((c) => c && String(c).toLowerCase().indexOf(terme) !== -1);
}

function rendreKanban(etat) {
  const kanban = $("kanban");
  vider(kanban);
  const terme = ($("filtre").value || "").trim().toLowerCase();
  const divergencesSeules = $("filtre-divergences").checked;
  let affichees = 0;

  etat.colonnes.forEach((colonne) => {
    const toutes = etat.specs.filter((s) => s.colonne === colonne.classe);
    const specs = toutes.filter((s) => specCorrespond(s, terme, divergencesSeules));
    affichees += specs.length;

    const bloc = el("div", "colonne");
    const entete = el("div", "colonne-entete");
    entete.appendChild(el("span", "", colonne.libelle));
    /* Quand un filtre est actif, on montre les DEUX nombres : cacher le total
     * ferait croire que la colonne a rétréci. */
    entete.appendChild(el("span", "",
      specs.length === toutes.length ? String(toutes.length)
                                     : specs.length + " / " + toutes.length));
    bloc.appendChild(entete);

    const liste = el("div", "colonne-liste");
    specs.forEach((spec) => {
      const carte = el("button", "carte" + (spec.divergence ? " divergente" : ""));
      carte.type = "button";
      carte.appendChild(el("div", "cid", spec.id));
      if (spec.titre && spec.titre !== spec.id) {
        carte.appendChild(el("div", "ctitre", spec.titre));
      }
      const bas = el("div", "cbas");
      if (spec.divergence) {
        const badge = el("span", "etiquette divergence", "≠ " + colonne.libelle);
        /* Le statut brut est affiché TEL QUEL en infobulle, jamais réécrit :
         * c'est une phrase écrite par un humain, pas un jeton. */
        badge.title = "Le fichier annonce : " + (spec.statut_texte || "");
        bas.appendChild(badge);
      }
      const priorite = nettoyerPriorite(spec.priorite);
      if (priorite) {
        const p = priorite.toLowerCase();
        const classe = p.indexOf("critique") === 0 ? "priorite-critique"
          : p.indexOf("haute") === 0 ? "priorite-haute" : "";
        const etiquette = el("span", "etiquette " + classe, priorite);
        etiquette.title = "Priorité, telle qu'écrite : " + spec.priorite;
        bas.appendChild(etiquette);
      }
      if (bas.childNodes.length) carte.appendChild(bas);
      carte.addEventListener("click", () => ouvrirSpec(spec.id));
      liste.appendChild(carte);
    });
    if (!specs.length) {
      liste.appendChild(el("div", "vide", toutes.length ? "aucune ne correspond" : "aucune"));
    }
    bloc.appendChild(liste);
    kanban.appendChild(bloc);
  });

  const filtre_actif = terme || divergencesSeules;
  $("compteur-specs").textContent =
    (filtre_actif ? affichees + " sur " + etat.specs.length + " specs"
                  : etat.specs.length + " specs") +
    " · " + etat.divergences + " divergence" + (etat.divergences > 1 ? "s" : "") +
    " répertoire / champ Statut";
}

/* ------------------------------------------------------------- workflows -- */
function rendreWorkflows(etat) {
  const zone = $("workflows");
  vider(zone);
  const workflows = etat.workflows.slice().reverse();
  if (!workflows.length) {
    zone.appendChild(el("div", "vide", "aucun événement encore observé"));
    $("compteur-workflows").textContent = "";
    return;
  }
  workflows.forEach((wf) => {
    const bloc = el("div", "workflow");
    const entete = el("div", "wf-entete");
    const actif = wf.fin_observee_absente;
    entete.appendChild(el("span", "marqueur " + (actif ? "actif" : "clos"),
      actif ? "● sans fin observée" : "✓ close"));
    entete.appendChild(el("span", "session", "session " + String(wf.session_id).slice(0, 8)));
    entete.appendChild(el("span", "session", heure(wf.debut) + " → " + heure(wf.fin_observee)));
    entete.appendChild(el("span", "session", wf.fichiers.length + " fichier(s)"));
    bloc.appendChild(entete);

    if (wf.passages.length) {
      const chaine = el("div", "chaine");
      wf.passages.forEach((passage, index) => {
        if (index) chaine.appendChild(el("span", "fleche", "→"));
        let classe = "maillon";
        if (!passage.termine) classe += " en-cours";
        if (passage.echecs) classe += " echec";
        const maillon = el("span", classe);
        maillon.appendChild(el("span", "mnom", passage.agent));
        const d = duree(passage.duree_ms);
        maillon.appendChild(el("span", "mduree",
          d !== null ? d : (passage.termine ? "durée invalide" : "en cours")));
        if (passage.echecs) maillon.appendChild(el("span", "mduree", "✕" + passage.echecs));
        chaine.appendChild(maillon);
      });
      bloc.appendChild(chaine);
    }

    const notes = [];
    const reprises = Object.keys(wf.reprises || {});
    if (reprises.length) {
      notes.push("reprises : " + reprises.map((a) => a + " ×" + wf.reprises[a]).join(", "));
    }
    if (wf.commandes && Object.keys(wf.commandes).length) {
      notes.push("commandes : " + Object.keys(wf.commandes)
        .map((c) => c + " ×" + wf.commandes[c]).join(", "));
    }
    if (notes.length) bloc.appendChild(el("div", "wf-notes", notes.join(" · ")));

    /* Les incertitudes sont AFFICHÉES, pas masquées : un récit muet sur ce
     * qu'il ignore se lit comme un récit complet. */
    if (wf.imbrication_suspectee || wf.fermetures_orphelines) {
      const avert = el("div", "wf-notes");
      const parts = [];
      if (wf.imbrication_suspectee) parts.push("imbrication suspectée — récit incomplet");
      if (wf.fermetures_orphelines) {
        parts.push(wf.fermetures_orphelines +
          " fermeture(s) sans ouverture (le runtime en émet une par fin de tour)");
      }
      avert.appendChild(el("span", "alerte", "⚠ " + parts.join(" · ")));
      bloc.appendChild(avert);
    }
    zone.appendChild(bloc);
  });
  $("compteur-workflows").textContent = etat.evenements.length + " derniers événements";
}

/* -------------------------------------------------------------- fichiers -- */
function rendreFichiers(etat) {
  const zone = $("fichiers");
  vider(zone);
  if (!etat.git.depot) {
    zone.appendChild(el("div", "vide", "pas un dépôt git"));
    return;
  }
  if (!etat.git.touches.length) {
    zone.appendChild(el("div", "vide", "arbre de travail propre"));
  }
  etat.git.touches.forEach((entree) => {
    const ligne = el("button", "fichier");
    ligne.type = "button";
    ligne.appendChild(el("span", "fcode", entree.libelle));
    ligne.appendChild(el("span", "fchemin",
      entree.ancien ? entree.ancien + " → " + entree.chemin : entree.chemin));
    ligne.addEventListener("click", () => ouvrirDiff(entree.chemin));
    zone.appendChild(ligne);
  });

  (etat.git.commits || []).slice(0, 4).forEach((commit) => {
    const ligne = el("div", "fichier");
    ligne.appendChild(el("span", "fcode", commit.hash));
    ligne.appendChild(el("span", "fchemin", commit.sujet));
    zone.appendChild(ligne);
  });
}

/* -------------------------------------------------------------- timeline -- */
function rendreTimeline(etat) {
  const liste = $("timeline");
  vider(liste);
  const evenements = etat.evenements.slice().reverse();
  if (!evenements.length) {
    liste.appendChild(el("li", "vide", "journal vide"));
    return;
  }
  evenements.forEach((e) => {
    let classe = "";
    let quoi = e.hook_event_name || "?";
    if (e.hook_event_name === "SubagentStart") { classe = "agent"; quoi = "▶ " + (e.agent_type || "?"); }
    else if (e.hook_event_name === "SubagentStop") { classe = "agent"; quoi = "■ fin de sous-agent"; }
    else if (e.failed) { classe = "echec"; quoi = "✕ " + (e.tool || "outil") + " en échec"; }
    else if (e.command_head) { quoi = "$ " + e.command_head; }
    else if (e.file) { quoi = (e.tool || "fichier") + " " + e.file; }
    const ligne = el("li", classe);
    ligne.appendChild(el("span", "heure", heure(e.ts)));
    ligne.appendChild(el("span", "quoi", quoi));
    liste.appendChild(ligne);
  });
}

/* ---------------------------------------------------------------- tiroir -- */
function ouvrirTiroir(titre, contenu) {
  $("tiroir-titre").textContent = titre;
  $("tiroir-contenu").textContent = contenu;
  $("tiroir").hidden = false;
  $("tiroir-fermer").focus();
}

function fermerTiroir() { $("tiroir").hidden = true; }

async function ouvrirSpec(identifiant) {
  ouvrirTiroir(identifiant, "chargement…");
  try {
    const reponse = await fetch("/api/spec?id=" + encodeURIComponent(identifiant));
    const donnees = await reponse.json();
    if (donnees.erreur) return ouvrirTiroir(identifiant, "Erreur : " + donnees.erreur);
    const entete = [
      "colonne   : " + donnees.colonne,
      "statut    : " + (donnees.statut_texte || "(absent)"),
      "classé    : " + donnees.statut_classe +
        (donnees.divergence ? "   ⚠ DIVERGENCE avec le répertoire" : ""),
      "priorité  : " + (donnees.priorite || "—"),
      "".padEnd(60, "─"),
      "",
    ].join("\n");
    ouvrirTiroir(identifiant, entete + donnees.contenu);
  } catch (erreur) {
    ouvrirTiroir(identifiant, "Erreur réseau : " + erreur.message);
  }
}

async function ouvrirDiff(chemin) {
  ouvrirTiroir(chemin, "chargement…");
  try {
    const reponse = await fetch("/api/diff?chemin=" + encodeURIComponent(chemin));
    const donnees = await reponse.json();
    if (donnees.erreur) return ouvrirTiroir(chemin, "Erreur : " + donnees.erreur);
    const avis = donnees.tronque
      ? "⚠ DIFF TRONQUÉ — la suite n'est pas affichée\n" + "".padEnd(60, "─") + "\n\n"
      : "";
    ouvrirTiroir(chemin, avis + (donnees.diff || "(aucune différence)"));
  } catch (erreur) {
    ouvrirTiroir(chemin, "Erreur réseau : " + erreur.message);
  }
}

/* ------------------------------------------------------------------ flux -- */
let enCours = false;

async function rafraichir() {
  if (enCours) return;
  enCours = true;
  try {
    const reponse = await fetch("/api/etat");
    const etat = await reponse.json();
    $("projet").textContent = etat.projet || "";
    $("horodatage").textContent = heure(etat.horodatage) + " UTC";
    const jaugeGit = $("jauge-git");
    jaugeGit.textContent = etat.git.depot ? "git " + (etat.git.branche || "?") : "git absent";
    jaugeGit.className = "jauge " + (etat.git.depot ? "ok" : "ko");
    dernierEtat = etat;
    rendreAgents(etat.agents);
    rendreSysteme(etat);
    rendreKanban(etat);
    rendreWorkflows(etat);
    rendreFichiers(etat);
    rendreTimeline(etat);
  } catch (erreur) {
    const jauge = $("jauge-flux");
    jauge.textContent = "serveur injoignable";
    jauge.className = "jauge ko";
  } finally {
    enCours = false;
  }
}

function brancherFlux() {
  const jauge = $("jauge-flux");
  const source = new EventSource("/api/flux");
  source.onopen = () => { jauge.textContent = "flux connecté"; jauge.className = "jauge ok"; };
  source.addEventListener("maj", rafraichir);
  source.onerror = () => {
    jauge.textContent = "flux rompu — reconnexion";
    jauge.className = "jauge ko";
    /* EventSource se reconnecte tout seul : on ne double pas sa logique. */
  };
}

/* Filtrer ne redemande RIEN au serveur : on redessine l'état déjà chargé. Une
 * requête par frappe ferait tourner `git status` et relire 167 fichiers à chaque
 * lettre. */
function refiltrer() { if (dernierEtat) rendreKanban(dernierEtat); }
$("filtre").addEventListener("input", refiltrer);
$("filtre-divergences").addEventListener("change", refiltrer);

document.addEventListener("keydown", (evenement) => {
  if (evenement.key === "Escape") fermerTiroir();
});
$("tiroir-fermer").addEventListener("click", fermerTiroir);

rafraichir();
brancherFlux();
