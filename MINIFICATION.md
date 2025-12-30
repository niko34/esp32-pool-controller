# Minification des fichiers web

## Vue d'ensemble

Pour optimiser l'utilisation de la mémoire flash de l'ESP32, tous les fichiers HTML, CSS et JavaScript sont automatiquement minifiés lors de la construction du filesystem LittleFS.

## Processus

### Fichiers sources (versionnés dans Git)
- `data/` - Contient les fichiers sources lisibles et éditables
  - `index.html` - Interface principale
  - `login.html` - Page de connexion
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
python3 minify.py
# Vérifier data-build/
```

### 3. Compiler et déployer

Le script `build_fs.sh` minifie automatiquement:

```bash
./build_fs.sh  # Minifie + construit LittleFS
```

Ou utiliser le script de déploiement complet:

```bash
./deploy.sh fs   # Filesystem uniquement
./deploy.sh all  # Firmware + filesystem
```

## Script de minification

### minify.py

Script Python pur (sans dépendances externes) qui:

- **JavaScript**: Supprime commentaires, espaces superflus, sauts de ligne
- **CSS**: Supprime commentaires, espaces, optimise la syntaxe
- **HTML**: Supprime commentaires, espaces entre balises, minifie les `<script>` et `<style>` intégrés
- **Images/binaires**: Copiés tels quels (pas de minification)

### Limitations

La minification basique ne fait pas:
- Renommage de variables (obfuscation)
- Tree shaking
- Compression gzip/brotli
- Optimisation d'images

Pour une optimisation maximale, utilisez des outils externes (uglify-js, terser, csso) et copiez les résultats dans `data/`.

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
python3 ~/.platformio/packages/tool-esptoolpy/esptool.py \
  --chip esp32 --port /dev/cu.usbserial-210 --baud 115200 \
  write_flash 0x290000 .pio/build/esp32dev/littlefs.bin
```

## Notes importantes

- ⚠️ **Ne JAMAIS éditer les fichiers dans `data-build/`** - ils sont écrasés à chaque build
- ✅ **Toujours versionner uniquement `data/`** dans Git
- ✅ **Le dossier `data-build/` est ignoré par `.gitignore`**
- ✅ **La minification est automatique** - pas besoin de l'exécuter manuellement
