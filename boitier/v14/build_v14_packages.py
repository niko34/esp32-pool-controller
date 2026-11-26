"""Archive les deux packages V14 seulement apres verification des fichiers livres.

A lancer avec Python standard, depuis le dossier parent ou une des variantes.
Ne remplace aucune archive existante. Manifestes et controle CRC inclus.
"""
from pathlib import Path
import hashlib, json, zipfile

here=Path(__file__).resolve().parent
root=here.parent if here.name.endswith('_PrintPack') else here
variants=('PoolController_AliExpress_V14_PrintPack','PoolController_MP1_V14_PrintPack')
profile_name='PoolController_V12_Capot_SUNLU_PLAPlus_Genius_0.4_Supports.curaprofile'
profile_sha='a8c3c5ba95a78a5a12b2e99e523a04df6310a1300f098b48415fe0f91735337c'
docs=('README.md','SESSION_HANDOFF.md','DESIGN_ARCHITECTURE.md','ASSEMBLY.md',
    'FABRICATION.md','VALIDATION_CHECKLIST.md','REVISION_NOTES.md',
    'PROMPT_MODELISATION_IA.md','RPS_WIRING.md','BOM.md','REFERENCES.md',
    'CRAFTCLOUD_ORDER.md','MULTIMATERIAL_IMPORT.md')
scripts=('generate_freecad.py','validate_existing.py','verify_exports.py',
    'verify_pcb_export.py','trial_v14_layout.py','preview_interface.FCMacro',
    'verify_v14_common_plate.py','build_v14_packages.py')

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def prepare(name):
    folder=root/name
    sha=digest(folder/'generate_freecad.py')
    reports=[]
    for file in ('validation_report.json','exports_reloaded.json','pcb_reloaded.json'):
        p=folder/'reports'/file
        report=json.loads(p.read_text())
        if not report['pass'] or (file!='validation_report.json' and report['generator_sha256']!=sha):
            raise RuntimeError('Rapport absent, obsolete ou en echec : '+str(p))
        reports.append(p)
    exports_report=json.loads((folder/'reports'/'exports_reloaded.json').read_text())
    if exports_report['validation_report_sha256']!=digest(folder/'reports'/'validation_report.json'):
        raise RuntimeError('Rapport de mouvement modifie depuis verification des exports')
    common=folder/'reports'/'common_plate_reloaded.json'
    report=json.loads(common.read_text())
    if not report['pass'] or report['generator_sha256_by_variant'][name]!=sha:
        raise RuntimeError('Comparaison commune obsolete ou en echec')
    profile=folder/'cura'/profile_name
    if digest(profile)!=profile_sha:
        raise RuntimeError('Profil Cura modifie sans nouvelle validation')
    pcb_report=json.loads((folder/'reports'/'pcb_reloaded.json').read_text())
    right_access=pcb_report['lower_right_wall_access']
    if not right_access['pass'] or right_access['head_pcb_gap_mm']<2:
        raise RuntimeError('Degagement V14 bas droit insuffisant')
    exports=[]
    for directory,suffix,count in (('freecad','FCStd',16),('step','step',15),('stl','stl',13)):
        files=sorted((folder/directory).glob('*.'+suffix))
        if len(files)!=count:
            raise RuntimeError('Inventaire inattendu : '+str(folder/directory))
        exports+=files
    pump='PUMP_ALIEXPRESS.md' if 'AliExpress' in name else 'PUMP_MP1.md'
    files=[folder/file for file in (*docs,*scripts,pump)] + exports + reports + [common,profile]
    files += [folder/'cura'/file for file in ('README.md','V12_CAPOT_PLA_PLUS.md',
        'V12_PROFILE_VALIDATION.json','build_v12_profile.py')]
    files += sorted(p for p in (folder/'cura'/'v12_sources').rglob('*') if p.is_file())
    files += sorted(p for p in (folder/'references').rglob('*') if p.is_file())
    if any(not p.is_file() for p in files):
        raise RuntimeError('Fichier de livraison manquant')
    # Aucun ancien essai V12, cache Python, sauvegarde FreeCAD ou ancien profil sans supports.
    manifest={'version':'V14','generator_sha256':sha,'profile_sha256':profile_sha,
        'inventory':{'FCStd':16,'STEP':15,'STL':13},
        'body_dimensions_mm':[210,175,94],'pcb_dimensions_mm':[119.38,119.38],
        'pcb_hole_spacing_mm':[104.14,104.14],
        'pcb_position_xy_mm':pcb_report['pcb_interface']['position_xy_mm'],
        'lower_right_wall_access':right_access,
        'mechanical_controls_pass':True,'physical_connector_qualification':False,
        'production_ready':False,
        'files':{str(p.relative_to(folder)):{'bytes':p.stat().st_size,'sha256':digest(p)} for p in files}}
    manifest_file=folder/'PACKAGE_MANIFEST.json'
    manifest_file.write_text(json.dumps(manifest,indent=2,ensure_ascii=False)+'\n')
    files.append(manifest_file)
    label='AliExpress' if 'AliExpress' in name else 'MP1'
    archive=root/f'PoolController_{label}_V14_2026-10-07_Model_Cura.zip'
    if archive.exists():
        raise RuntimeError('Archive existante preservee : '+str(archive))
    return folder,archive,files

packages=[prepare(name) for name in variants]
combined=root/'PoolController_DeuxPompes_V14_2026-10-07_Model_Cura.zip'
if combined.exists():
    raise RuntimeError('Archive commune existante preservee : '+str(combined))
for folder,archive,files in packages:
    with zipfile.ZipFile(archive,'x',zipfile.ZIP_DEFLATED) as z:
        for p in files:
            z.write(p,str(Path(folder.name)/p.relative_to(folder)))
    with zipfile.ZipFile(archive) as z:
        if z.testzip() is not None:
            raise RuntimeError('Erreur CRC archive')
        manifest=json.loads(z.read(folder.name+'/PACKAGE_MANIFEST.json'))
        for relative,row in manifest['files'].items():
            data=z.read(folder.name+'/'+relative)
            if len(data)!=row['bytes'] or hashlib.sha256(data).hexdigest()!=row['sha256']:
                raise RuntimeError('Erreur verification entree ZIP : '+relative)
    print(json.dumps({'archive':str(archive),'bytes':archive.stat().st_size,
        'sha256':digest(archive),'verified_entries':len(files),'pass':True}),flush=True)

with zipfile.ZipFile(combined,'x',zipfile.ZIP_DEFLATED) as z:
    for folder,archive,files in packages:
        for p in files:
            z.write(p,str(Path(folder.name)/p.relative_to(folder)))
with zipfile.ZipFile(combined) as z:
    if z.testzip() is not None:
        raise RuntimeError('Erreur CRC archive commune')
    for folder,archive,files in packages:
        manifest=json.loads(z.read(folder.name+'/PACKAGE_MANIFEST.json'))
        for relative,row in manifest['files'].items():
            data=z.read(folder.name+'/'+relative)
            if len(data)!=row['bytes'] or hashlib.sha256(data).hexdigest()!=row['sha256']:
                raise RuntimeError('Erreur entree ZIP commune : '+relative)
print(json.dumps({'archive':str(combined),'bytes':combined.stat().st_size,
    'sha256':digest(combined),'contains_variants':list(variants),'pass':True}),flush=True)
