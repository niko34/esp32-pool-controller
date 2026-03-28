# Minification des fichiers web

## Vue d'ensemble

Pour optimiser l'utilisation de la mémoire flash de l'ESP32, tous les fichiers HTML, CSS et JavaScript sont automatiquement minifiés lors de la construction du filesystem LittleFS.

## Processus

### Fichiers sources (versionnés dans Git)
- `data/` - Contient les fichiers sources lisibles et éditables
  - `index.html` - Interface principale
  - `login.html` - Page de connexion
  - `wizard.html` - Assistant de configuration initiale
  - `wifi.html` - Page de configuration WiFi
  - `app.js` - Logique JavaScript
  - `app.css` - Styles CSS
  - Images et autres fichiers statiques

### Fichiers minifiés (générés automatiquement)
- `data-build/` - Contient les fichiers minifiés (ignoré par `.gitignore`)
  - Généré automatiquement par `build_fs.sh`
  - Ne doit JAMAIS être édité manuellement
  - Ne doit JAMAIS être versionné dans Git

## Économie d'espace

La minification économise environ **60KB** (~13%) sur les fichiers web:

| Fichier | Original | Minifié | Économie |
|---------|----------|---------|----------|
| index.html | 57 KB | 38 KB | -33% |
| app.js | 108 KB | 75 KB | -31% |
| login.html | 9 KB | 6 KB | -35% |
| app.css | 22 KB | 18 KB | -18% |

**Total: 472 KB → 404 KB**

## Workflow de développement

### 1. Éditer les fichiers sources

Toujours éditer les fichiers dans `data/`:

```bash
# Éditer l'interface
vim data/index.html
vim data/app.js
vim data/app.css
```

### 2. Tester localement (optionnel)

Pour tester les fichiers minifiés sans uploader:

```bash
node minify.js
# Vérifier data-build/
```

### 3. Compiler et déployer

Le script `build_fs.sh` minifie automatiquement:

```bash
./build_fs.sh   # Minifie + construit LittleFS
./build_all.sh  # Compile firmware + filesystem, copie les .bin à la racine
```

Ou utiliser le script de déploiement complet (upload USB) :

```bash
./deploy.sh fs       # Build + upload filesystem uniquement
./deploy.sh firmware # Build + upload firmware uniquement
./deploy.sh all      # Build + upload firmware + filesystem
```

## Script de minification

### minify.js

Script Node.js utilisant des outils professionnels standards de l'industrie :

- **JavaScript** : Minification via **Terser** (suppression commentaires, renommage variables, optimisation)
- **CSS** : Minification via **CleanCSS** (suppression commentaires, optimisation syntaxe)
- **HTML** : Minification via **html-minifier-terser** (suppression commentaires, espaces, minification des scripts et styles intégrés)
- **Images/binaires** : Copiés tels quels (pas de minification)

### Dépendances

Les outils sont installés via npm (voir `package.json`) :
```bash
npm install  # Installe terser, clean-css, html-minifier-terser
```

### Fonctionnalités

La minification inclut :
- ✅ Suppression des commentaires
- ✅ Suppression des espaces superflus
- ✅ Renommage des variables (JavaScript)
- ✅ Optimisation de la syntaxe CSS
- ✅ Minification des scripts inline dans le HTML

## Dépannage

### Le site web ne s'affiche pas correctement

1. Vérifier que `data-build/` a été généré:
   ```bash
   ls -la data-build/
   ```

2. Rebuilder le filesystem:
   ```bash
   ./build_fs.sh
   ```

3. Si le problème persiste, tester les fichiers sources dans `data/` directement (désactiver temporairement la minification)

### Erreur de minification

Si un fichier cause une erreur lors de la minification:

1. Vérifier la syntaxe du fichier source
2. Vérifier l'encodage (doit être UTF-8)
3. Le script copiera le fichier tel quel en cas d'erreur

### Modifications non prises en compte

Toujours rebuilder le filesystem après modification:

```bash
./build_fs.sh
~/.platformio/penv/bin/esptool.py \
  --chip esp32 --port /dev/cu.usbserial-0001 --baud 115200 \
  write_flash 0x2B0000 .pio/build/esp32dev/littlefs.bin
```

## Notes importantes

- ⚠️ **Ne JAMAIS éditer les fichiers dans `data-build/`** - ils sont écrasés à chaque build
- ✅ **Toujours versionner uniquement `data/`** dans Git
- ✅ **Le dossier `data-build/` est ignoré par `.gitignore`**
- ✅ **La minification est automatique** - pas besoin de l'exécuter manuellement
