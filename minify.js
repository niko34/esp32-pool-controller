#!/usr/bin/env node
/**
 * Script de minification pour fichiers web ESP32
 * Utilise html-minifier-terser, terser et clean-css pour une minification fiable
 */

const fs = require('fs');
const path = require('path');
const zlib = require('zlib');
const { minify: minifyHTML } = require('html-minifier-terser');
const { minify: minifyJS } = require('terser');
const CleanCSS = require('clean-css');

const SRC_DIR = 'data';
const DST_DIR = 'data-build';

// Configuration pour html-minifier-terser
const htmlMinifierOptions = {
  collapseWhitespace: true,
  removeComments: true,
  removeRedundantAttributes: true,
  removeScriptTypeAttributes: true,
  removeStyleLinkTypeAttributes: true,
  useShortDoctype: true,
  minifyCSS: true,
  minifyJS: true,  // Utilise terser pour le JavaScript
  minifyURLs: false
};

// Configuration pour terser (JS)
const terserOptions = {
  compress: {
    dead_code: true,
    drop_console: false,
    drop_debugger: true,
    keep_classnames: false,
    keep_fargs: true,
    keep_fnames: false,
    keep_infinity: false
  },
  mangle: false,  // Ne pas renommer les variables pour garder le code lisible
  format: {
    comments: false
  }
};

// Instance clean-css pour CSS
const cleanCSS = new CleanCSS({
  level: 2,
  compatibility: '*'
});

function getFileSize(filePath) {
  try {
    return fs.statSync(filePath).size;
  } catch {
    return 0;
  }
}

/**
 * Feature-048 : pré-compression gzip des fichiers texte.
 * Écrit `<destPath>.gz` (zlib niveau 9) et NE conserve PAS la variante non
 * compressée : ESPAsyncWebServer résout nativement `<fichier>.gz` et sert
 * avec `Content-Encoding: gzip` (serveStatic + request->send(fs, path)).
 * Retourne la taille du fichier .gz écrit.
 */
function writeGzip(destPath, content) {
  const buf = Buffer.isBuffer(content) ? content : Buffer.from(content, 'utf8');
  const gz = zlib.gzipSync(buf, { level: 9 });
  fs.writeFileSync(destPath + '.gz', gz);
  return gz.length;
}

function copyDirectory(src, dest) {
  if (!fs.existsSync(dest)) {
    fs.mkdirSync(dest, { recursive: true });
  }

  const entries = fs.readdirSync(src, { withFileTypes: true });

  for (const entry of entries) {
    const srcPath = path.join(src, entry.name);
    const destPath = path.join(dest, entry.name);

    if (entry.name.startsWith('.')) {
      continue; // Skip hidden files
    }

    if (entry.isDirectory()) {
      copyDirectory(srcPath, destPath);
    } else {
      processFile(srcPath, destPath);
    }
  }
}

async function processFile(srcPath, destPath) {
  const ext = path.extname(srcPath).toLowerCase();
  const relPath = path.relative(SRC_DIR, srcPath);

  // Pour les fichiers HTML, utiliser html-minifier-terser
  if (ext === '.html') {
    try {
      const content = fs.readFileSync(srcPath, 'utf8');
      const originalSize = content.length;

      const minified = await minifyHTML(content, htmlMinifierOptions);
      const minifiedSize = minified.length;
      const gzSize = writeGzip(destPath, minified);

      const reduction = ((originalSize - gzSize) / originalSize) * 100;
      console.log(`  ${relPath}: ${originalSize.toLocaleString()} → ${minifiedSize.toLocaleString()} → gz ${gzSize.toLocaleString()} bytes (-${reduction.toFixed(1)}%)`);

      return { original: originalSize, minified: gzSize };
    } catch (error) {
      console.error(`Erreur lors de la minification de ${srcPath}:`, error.message);
      const gzSize = writeGzip(destPath, fs.readFileSync(srcPath));
      return { original: getFileSize(srcPath), minified: gzSize };
    }
  }

  // Pour les fichiers JavaScript, utiliser terser
  if (ext === '.js') {
    try {
      const content = fs.readFileSync(srcPath, 'utf8');
      const originalSize = content.length;

      const result = await minifyJS(content, terserOptions);
      const minified = result.code;
      const minifiedSize = minified.length;
      const gzSize = writeGzip(destPath, minified);

      const reduction = ((originalSize - gzSize) / originalSize) * 100;
      console.log(`  ${relPath}: ${originalSize.toLocaleString()} → ${minifiedSize.toLocaleString()} → gz ${gzSize.toLocaleString()} bytes (-${reduction.toFixed(1)}%)`);

      return { original: originalSize, minified: gzSize };
    } catch (error) {
      console.error(`Erreur lors de la minification de ${srcPath}:`, error.message);
      const gzSize = writeGzip(destPath, fs.readFileSync(srcPath));
      return { original: getFileSize(srcPath), minified: gzSize };
    }
  }

  // Pour les fichiers CSS, utiliser clean-css
  if (ext === '.css') {
    try {
      const content = fs.readFileSync(srcPath, 'utf8');
      const originalSize = content.length;

      const output = cleanCSS.minify(content);

      if (output.errors && output.errors.length > 0) {
        throw new Error(output.errors.join(', '));
      }

      const minified = output.styles;
      const minifiedSize = minified.length;
      const gzSize = writeGzip(destPath, minified);

      const reduction = ((originalSize - gzSize) / originalSize) * 100;
      console.log(`  ${relPath}: ${originalSize.toLocaleString()} → ${minifiedSize.toLocaleString()} → gz ${gzSize.toLocaleString()} bytes (-${reduction.toFixed(1)}%)`);

      return { original: originalSize, minified: gzSize };
    } catch (error) {
      console.error(`Erreur lors de la minification de ${srcPath}:`, error.message);
      const gzSize = writeGzip(destPath, fs.readFileSync(srcPath));
      return { original: getFileSize(srcPath), minified: gzSize };
    }
  }

  // Pour les autres fichiers (images, icônes), copier tel quel (pas de gzip)
  fs.copyFileSync(srcPath, destPath);
  const size = getFileSize(srcPath);
  return { original: size, minified: size };
}

async function main() {
  console.log('🔧 Minification des fichiers web...\n');

  // Supprimer et recréer le dossier de destination
  if (fs.existsSync(DST_DIR)) {
    fs.rmSync(DST_DIR, { recursive: true, force: true });
  }
  fs.mkdirSync(DST_DIR, { recursive: true });

  let totalOriginal = 0;
  let totalMinified = 0;

  // Parcourir tous les fichiers
  const entries = fs.readdirSync(SRC_DIR, { withFileTypes: true });

  for (const entry of entries) {
    const srcPath = path.join(SRC_DIR, entry.name);
    const destPath = path.join(DST_DIR, entry.name);

    if (entry.name.startsWith('.')) {
      continue;
    }

    if (entry.isDirectory()) {
      copyDirectory(srcPath, destPath);
    } else {
      const result = await processFile(srcPath, destPath);
      totalOriginal += result.original;
      totalMinified += result.minified;
    }
  }

  console.log();
  console.log(`📊 Total: ${totalOriginal.toLocaleString()} → ${totalMinified.toLocaleString()} bytes`);
  const totalReduction = ((totalOriginal - totalMinified) / totalOriginal) * 100;
  console.log(`💾 Économie: ${(totalOriginal - totalMinified).toLocaleString()} bytes (-${totalReduction.toFixed(1)}%)`);
  console.log();
  console.log(`✅ Fichiers minifiés dans ${DST_DIR}/`);
}

main().catch(error => {
  console.error('❌ Erreur:', error);
  process.exit(1);
});
