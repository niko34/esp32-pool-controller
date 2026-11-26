"""Compare les solides natifs communs MP1/AliExpress V14, via FreeCADCmd."""
from pathlib import Path
import sys, json, hashlib

here = Path(__file__).resolve().parent
root = here.parent if here.name.endswith('_PrintPack') else here
variants = [root/'PoolController_AliExpress_V14_PrintPack', root/'PoolController_MP1_V14_PrintPack']
sys.path.insert(0, str(variants[0]))
import generate_freecad as g

def read_shape(folder, suffix):
    path = next((folder/'freecad').glob('*_'+suffix+'.FCStd'))
    doc = g.App.openDocument(str(path))
    shape = next(o.Shape.copy() for o in doc.Objects if hasattr(o,'Shape') and not o.Shape.isNull())
    g.App.closeDocument(doc.Name)
    return shape

rows = []
suffixes = ('WallBackplateLightCore', 'WallBackplateTechnicalSkin',
    'WallBackplateMonoblocReference', 'PSUGuard', 'CoverTurquoiseInlay',
    'PCBFitCoupon', 'ConnectorFitCoupon', 'M4InsertFitCoupon', 'GasketFitCoupon',
    'WallAccessFitCoupon', 'LeftWallAccessFitCoupon')
for suffix in suffixes:
    a,b = [read_shape(folder, suffix) for folder in variants]
    common = a.common(b).Volume
    delta = a.Volume+b.Volume-2*common
    rows.append({'part':suffix, 'ali_volume_mm3':a.Volume, 'mp1_volume_mm3':b.Volume,
        'symmetric_difference_mm3':delta,
        'pass':a.isValid() and b.isValid() and abs(delta)<=.01})
report = {'generator_sha256_by_variant':{
    p.name:hashlib.sha256((p/'generate_freecad.py').read_bytes()).hexdigest() for p in variants},
    'parts':rows, 'pass':all(r['pass'] for r in rows),
    'scope':'solides natifs communs relus ; capots et coupons pompe exclus car empreintes différentes'}
for folder in variants:
    (folder/'reports'/'common_plate_reloaded.json').write_text(json.dumps(report,indent=2)+'\n')
print('COMMON_PARTS',json.dumps(report),flush=True)
if not report['pass']:
    raise RuntimeError('Les pieces communes V14 different')
