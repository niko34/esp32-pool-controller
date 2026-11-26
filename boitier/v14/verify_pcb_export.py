"""Controle PCB independant des cotes mesurees, sur les fichiers V14 relus."""
from pathlib import Path
import sys,json,math,hashlib
sys.path.insert(0,str(Path(__file__).resolve().parent))
import generate_freecad as g

def load_part(suffix):
    path=next(g.FC_DIR.glob('*_'+suffix+'.FCStd'))
    doc=g.App.openDocument(str(path))
    shape=next(o.Shape.copy() for o in doc.Objects if hasattr(o,'Shape') and not o.Shape.isNull())
    g.App.closeDocument(doc.Name)
    return shape

def difference(a,b):
    return a.Volume+b.Volume-2*a.common(b).Volume

def main():
    report=json.loads((g.REPORT_DIR/'validation_report.json').read_text())
    exported=json.loads((g.REPORT_DIR/'exports_reloaded.json').read_text())
    doc=g.App.openDocument(str(next(g.FC_DIR.glob('*_Assembly.FCStd'))))
    pcb=doc.getObject('PCBReference').Shape.copy()
    components=doc.getObject('PCBEnvelope').Shape.copy()
    bay=doc.getObject('PCBServiceBay').Shape.copy()
    expected=g.Part.makeBox(119.38,119.38,1.6,g.vec(76.62,13.5,12))
    holes=[]
    for x in (84.24,188.38):
        for y in (21.12,125.26):
            expected=expected.cut(g.Part.makeCylinder(1.6,1.8,g.vec(x,y,11.9)))
            probe=g.Part.makeCylinder(1.55,1.8,g.vec(x,y,11.9))
            holes.append({'x_mm':x,'y_mm':y,'pcb_obstruction_mm3':probe.common(pcb).Volume})
    board_delta=difference(pcb,expected)
    component_delta=difference(components,g.Part.makeBox(119.38,119.38,25,g.vec(76.62,13.5,13.6)))
    bay_delta=difference(bay,g.Part.makeBox(22,76,6,g.vec(76.62,40,42)))
    parts={'backplate_monobloc':load_part('WallBackplateMonoblocReference'),
        'psu_guard':load_part('PSUGuard')}
    cap=load_part('CoverMonoblocReference');cap.translate(g.vec(0,0,3.2))
    interface=g.pcb_interface_checks(parts,cap)
    coupon=load_part('PCBFitCoupon')
    coupon_expected=g.Part.makeBox(119.38,119.38,2)
    for x in (7.62,111.76):
        for y in (7.62,111.76):
            coupon_expected=coupon_expected.cut(g.Part.makeCylinder(1.6,2.2,g.vec(x,y,-.1)))
    coupon_delta=difference(coupon,coupon_expected)
    witness=g.Part.makeBox(2,2,.5,g.vec(106.62,42,9.5))
    under=g.Part.makeBox(119.38,119.38,3,g.vec(76.62,13.5,9))
    negative_volume=witness.common(under).Volume
    bnc_checks=[]
    for i,x in enumerate((123.102,159.424),1):
        stored=doc.getObject('BNC_'+str(i)).Shape
        probe=g.d_cutout_y(12.9,5.65,3.4,x,-.1,19.8)
        blocked=probe.common(parts['backplate_monobloc']).Volume
        bnc_checks.append({'index':i,'x_mm':x,'z_mm':19.8,
            'stored_x_mm':(stored.BoundBox.XMin+stored.BoundBox.XMax)/2,
            'stored_z_mm':(stored.BoundBox.ZMin+stored.BoundBox.ZMax)/2,
            'stored_y_min_mm':stored.BoundBox.YMin,'stored_y_max_mm':stored.BoundBox.YMax,
            'd_bore_obstruction_mm3':blocked})
    dimensions=[]
    for suffix in ('CoverLightCore','WallBackplateLightCore'):
        path=next(g.STL_DIR.glob('*_'+suffix+'.stl'));mesh=g.Mesh.Mesh(str(path))
        box=mesh.BoundBox
        dimensions.append({'part':suffix,'bbox_xyz_mm':[box.XLength,box.YLength,box.ZLength],
            'with_4mm_brim_xy_mm':[box.XLength+8,box.YLength+8],
            'fits_nominal_genius':box.XLength+8<=220.001 and box.YLength+8<=220.001 and box.ZLength<=250.001})
    # Acces bas droit controle avec des valeurs literales, sur la geometrie relue.
    head_path=g.Part.makeCylinder(3.75,128,g.vec(190,7.7,6))
    driver_path=g.Part.makeCylinder(3,124,g.vec(190,7.7,10))
    access={'axis_xy_mm':[190,7.7],'head_diameter_mm':7.5,'driver_diameter_mm':6,
        'head_pcb_gap_mm':head_path.distToShape(pcb)[0],
        'driver_pcb_gap_mm':driver_path.distToShape(pcb)[0],
        'head_plate_collision_mm3':head_path.common(parts['backplate_monobloc']).Volume,
        'driver_plate_collision_mm3':driver_path.common(parts['backplate_monobloc']).Volume,
        'head_pcb_collision_mm3':head_path.common(pcb).Volume,
        'driver_pcb_collision_mm3':driver_path.common(pcb).Volume}
    access['pass']=(access['head_pcb_gap_mm']>=2 and access['driver_pcb_gap_mm']>=2.75
        and max(access[k] for k in ('head_plate_collision_mm3','driver_plate_collision_mm3',
            'head_pcb_collision_mm3','driver_pcb_collision_mm3'))<=.01)
    g.App.closeDocument(doc.Name)
    out={'generator_sha256':hashlib.sha256((g.ROOT/'generate_freecad.py').read_bytes()).hexdigest(),
        'pcb_expected_dimensions_mm':[119.38,119.38,1.6],
        'independent_hole_spacing_mm':[104.14,104.14],'mounts':holes,
        'stored_pcb_difference_mm3':board_delta,'stored_components_difference_mm3':component_delta,
        'stored_service_bay_difference_mm3':bay_delta,'coupon_difference_mm3':coupon_delta,
        'pcb_interface':interface,'bnc_checks':bnc_checks,'printing_dimensions':dimensions,
        'lower_right_wall_access':access,
        'under_board_negative_control_mm3':negative_volume,
        'connector_orientation_confirmed':False,'actual_plugs_and_wires_qualified':False,
        'pass':report['pass'] and exported['pass'] and interface['pass'] and access['pass']
            and max(abs(v) for v in (board_delta,component_delta,bay_delta,coupon_delta))<=.01
            and all(r['pcb_obstruction_mm3']<=.01 for r in holes)
            and all(abs(r['x_mm']-r['stored_x_mm'])<=.001 and abs(r['z_mm']-r['stored_z_mm'])<=.001
                    and abs(r['stored_y_min_mm']+19)<=.001 and abs(r['stored_y_max_mm']-16.3)<=.001
                    and r['d_bore_obstruction_mm3']<=.01 for r in bnc_checks)
            and all(r['fits_nominal_genius'] for r in dimensions) and negative_volume>.1}
    (g.REPORT_DIR/'pcb_reloaded.json').write_text(json.dumps(out,indent=2)+'\n')
    if not out['pass']:raise RuntimeError('Echec verification PCB relu')
    print('PCB_RELOADED_PASS',json.dumps(out),flush=True)

main()
