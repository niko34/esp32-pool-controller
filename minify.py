#!/usr/bin/env python3
"""
Script de minification pour fichiers web ESP32
Minifie HTML, CSS et JavaScript sans dépendances externes
"""

import re
import os
import shutil
from pathlib import Path

def minify_js(content):
    """Minification JavaScript basique"""
    # Supprimer les commentaires sur une ligne
    content = re.sub(r'//.*?$', '', content, flags=re.MULTILINE)
    # Supprimer les commentaires multi-lignes
    content = re.sub(r'/\*.*?\*/', '', content, flags=re.DOTALL)
    # Supprimer les espaces multiples
    content = re.sub(r'\s+', ' ', content)
    # Supprimer les espaces autour des opérateurs
    content = re.sub(r'\s*([{};,:])\s*', r'\1', content)
    content = re.sub(r'\s*([=<>!+\-*/])\s*', r'\1', content)
    # Supprimer les sauts de ligne inutiles
    content = content.replace('\n', '')
    return content.strip()

def minify_css(content):
    """Minification CSS"""
    # Supprimer les commentaires
    content = re.sub(r'/\*.*?\*/', '', content, flags=re.DOTALL)
    # Supprimer les espaces multiples
    content = re.sub(r'\s+', ' ', content)
    # Supprimer les espaces autour des caractères spéciaux
    content = re.sub(r'\s*([{}:;,])\s*', r'\1', content)
    # Supprimer les sauts de ligne
    content = content.replace('\n', '')
    return content.strip()

def minify_html(content):
    """Minification HTML (conserve les scripts et styles)"""
    # Protéger les blocs script et style
    scripts = []
    styles = []

    def save_script(match):
        scripts.append(match.group(0))
        return f'___SCRIPT_{len(scripts)-1}___'

    def save_style(match):
        styles.append(match.group(0))
        return f'___STYLE_{len(styles)-1}___'

    # Sauvegarder les scripts
    content = re.sub(r'<script[^>]*>.*?</script>', save_script, content, flags=re.DOTALL | re.IGNORECASE)
    # Sauvegarder les styles
    content = re.sub(r'<style[^>]*>.*?</style>', save_style, content, flags=re.DOTALL | re.IGNORECASE)

    # Supprimer les commentaires HTML
    content = re.sub(r'<!--.*?-->', '', content, flags=re.DOTALL)
    # Supprimer les espaces multiples
    content = re.sub(r'\s+', ' ', content)
    # Supprimer les espaces entre les balises
    content = re.sub(r'>\s+<', '><', content)

    # Restaurer les scripts (déjà minifiés)
    for i, script in enumerate(scripts):
        # Minifier le contenu du script
        script_match = re.search(r'<script[^>]*>(.*?)</script>', script, re.DOTALL | re.IGNORECASE)
        if script_match and 'src=' not in script:
            script_content = script_match.group(1)
            minified_script = minify_js(script_content)
            script = re.sub(r'<script[^>]*>(.*?)</script>',
                          f'<script>{minified_script}</script>',
                          script, flags=re.DOTALL | re.IGNORECASE)
        content = content.replace(f'___SCRIPT_{i}___', script)

    # Restaurer les styles (déjà minifiés)
    for i, style in enumerate(styles):
        # Minifier le contenu du style
        style_match = re.search(r'<style[^>]*>(.*?)</style>', style, re.DOTALL | re.IGNORECASE)
        if style_match:
            style_content = style_match.group(1)
            minified_style = minify_css(style_content)
            style = re.sub(r'<style[^>]*>(.*?)</style>',
                         f'<style>{minified_style}</style>',
                         style, flags=re.DOTALL | re.IGNORECASE)
        content = content.replace(f'___STYLE_{i}___', style)

    return content.strip()

def minify_file(src_path, dst_path):
    """Minifie un fichier selon son extension"""
    ext = src_path.suffix.lower()

    # Fichiers binaires ou non-minifiables
    if ext not in ['.js', '.css', '.html']:
        shutil.copy2(src_path, dst_path)
        original_size = src_path.stat().st_size
        return original_size, original_size

    # Fichiers texte à minifier
    try:
        with open(src_path, 'r', encoding='utf-8') as f:
            content = f.read()
    except UnicodeDecodeError:
        # Si échec de lecture UTF-8, copier tel quel
        shutil.copy2(src_path, dst_path)
        original_size = src_path.stat().st_size
        return original_size, original_size

    original_size = len(content)

    if ext == '.js':
        minified = minify_js(content)
    elif ext == '.css':
        minified = minify_css(content)
    elif ext == '.html':
        minified = minify_html(content)
    else:
        minified = content

    with open(dst_path, 'w', encoding='utf-8') as f:
        f.write(minified)

    minified_size = len(minified)
    return original_size, minified_size

def main():
    """Point d'entrée principal"""
    src_dir = Path('data')
    dst_dir = Path('data-build')

    # Créer le dossier de destination
    if dst_dir.exists():
        shutil.rmtree(dst_dir)
    dst_dir.mkdir()

    total_original = 0
    total_minified = 0

    print("🔧 Minification des fichiers web...")
    print()

    # Parcourir tous les fichiers du dossier data
    for src_path in src_dir.rglob('*'):
        if src_path.is_file() and not src_path.name.startswith('.'):
            # Calculer le chemin de destination
            rel_path = src_path.relative_to(src_dir)
            dst_path = dst_dir / rel_path
            dst_path.parent.mkdir(parents=True, exist_ok=True)

            # Minifier ou copier
            original_size, minified_size = minify_file(src_path, dst_path)

            total_original += original_size
            total_minified += minified_size

            if original_size > 0:
                reduction = ((original_size - minified_size) / original_size) * 100
                if reduction > 1:  # Afficher seulement si réduction significative
                    print(f"  {rel_path}: {original_size:,} → {minified_size:,} bytes (-{reduction:.1f}%)")

    print()
    print(f"📊 Total: {total_original:,} → {total_minified:,} bytes")
    total_reduction = ((total_original - total_minified) / total_original) * 100
    print(f"💾 Économie: {total_original - total_minified:,} bytes (-{total_reduction:.1f}%)")
    print()
    print(f"✅ Fichiers minifiés dans {dst_dir}/")

if __name__ == '__main__':
    main()
