// js_extract.js — scanner d'extraction partagé (ADR-0099, amendement
// 2026-10-11, feature-154).
//
// Pourquoi ce fichier existe : `test/sha256_vectors.js` (feature-118) et
// `test/trace_logic_vectors.js` (feature-153) contenaient chacun une copie
// STRICTEMENT IDENTIQUE (au commentaire près — vérifié par diff avant ce
// lot) de `findMatchingClose()` et `extractBalancedFromAnchor()`. ADR-0099
// avait écarté la généralisation « pour ce lot », en la désignant comme
// « la direction naturelle si un troisième cas apparaît ». `feature-154`
// (pastilles de santé pH) EST ce troisième cas : son harnais
// `test/ph_health_vectors.js` en a besoin pour extraire `classifyPhCriterion`
// de `data/app.js`. Trois copies valaient la peine d'être évitées ; ce
// fichier factorise la fonction une seule fois, et les deux harnais
// existants la REQUIÈRENT désormais au lieu de la dupliquer une troisième
// fois. ADR-0099 est amendé en conséquence (section « Mise à jour
// 2026-10-11 ») — amendement, pas supersession : la décision centrale
// (extraire le code VIVANT, jamais le recopier) ne change pas.
//
// Ce module n'EXÉCUTE rien par lui-même : il expose deux fonctions pures de
// repérage de texte, utilisées par chaque harnais pour isoler le code réel
// de `data/app.js` avant de l'évaluer sous Node.
//
// Exécution directe : aucune — ce fichier n'a pas de harnais propre, il est
// `require()`-é par `sha256_vectors.js`, `trace_logic_vectors.js` et
// `ph_health_vectors.js`.

"use strict";

// Retourne l'index du caractère de FERMETURE correspondant à `openChar` situé
// à `openIdx` dans `src` (qui doit être `openChar`), en ignorant tout
// `openChar`/`closeChar` situé à l'intérieur d'une chaîne ('...'/"..."/`...`)
// ou d'un commentaire (// ou /* */) — un texte extrait doit rester du CODE,
// jamais une citation en commentaire (même piège que bug-098 /
// check_fopen_guard.sh).
function findMatchingClose(src, openIdx, openChar, closeChar) {
  if (src[openIdx] !== openChar) {
    throw new Error(`findMatchingClose: src[${openIdx}] n'est pas '${openChar}'`);
  }
  let depth = 0;
  let i = openIdx;
  while (i < src.length) {
    const c = src[i];
    if (c === '"' || c === "'" || c === "`") {
      const quote = c;
      i++;
      while (i < src.length && src[i] !== quote) {
        i += src[i] === "\\" ? 2 : 1;
      }
      i++;
      continue;
    }
    if (c === "/" && src[i + 1] === "/") {
      i += 2;
      while (i < src.length && src[i] !== "\n") i++;
      continue;
    }
    if (c === "/" && src[i + 1] === "*") {
      i += 2;
      while (i < src.length && !(src[i] === "*" && src[i + 1] === "/")) i++;
      i += 2;
      continue;
    }
    if (c === openChar) {
      depth++;
    } else if (c === closeChar) {
      depth--;
      if (depth === 0) return i;
    }
    i++;
  }
  throw new Error(`findMatchingClose: aucune fermeture '${closeChar}' trouvée pour l'ouverture à l'index ${openIdx}`);
}

// Localise `anchorText` dans `src`, exige UNE SEULE occurrence (sinon
// l'extraction serait ambiguë), et retourne la portion de `src` depuis le
// début de l'ancre jusqu'au caractère `closeChar` correspondant au
// `openChar` qui termine l'ancre (inclus).
function extractBalancedFromAnchor(src, anchorText, openChar, closeChar) {
  const firstIdx = src.indexOf(anchorText);
  if (firstIdx === -1) {
    throw new Error(`Ancre introuvable dans data/app.js : ${JSON.stringify(anchorText)}`);
  }
  if (src.indexOf(anchorText, firstIdx + 1) !== -1) {
    throw new Error(`Ancre AMBIGUË (plusieurs occurrences) dans data/app.js : ${JSON.stringify(anchorText)}`);
  }
  const openIdx = firstIdx + anchorText.length - 1;
  const closeIdx = findMatchingClose(src, openIdx, openChar, closeChar);
  return { text: src.slice(firstIdx, closeIdx + 1), closeIdx };
}

module.exports = { findMatchingClose, extractBalancedFromAnchor };
