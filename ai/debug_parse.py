#!/usr/bin/env python3
import re
from pathlib import Path

content = Path('ai/outputs/features/20260128_193727/result.md').read_text()

# Position après 'Fichier: data/index.html'
pos = 609
remaining = content[pos:]

print(f"Looking for code block in remaining content (length: {len(remaining)})")
print(f"First 300 chars: {remaining[:300]}")
print()

# Pattern simple avec caractère backtick
pattern = re.compile(r'```(\w+)\s*\n(.*?)```', re.DOTALL)
match = pattern.search(remaining)
if match:
    print(f'Found: lang={match.group(1)}, code length={len(match.group(2))}')
else:
    print('No match found')
    # Chercher manuellement
    idx = remaining.find('```')
    print(f'First ``` at index: {idx}')
    if idx >= 0:
        print(f'Context: {remaining[idx:idx+50]!r}')
