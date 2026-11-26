"""Controle independant des fichiers relus, a executer apres generate_freecad.py."""
from pathlib import Path
import sys, json, hashlib
sys.path.insert(0, str(Path(__file__).resolve().parent))
import generate_freecad as g

root = Path(__file__).resolve().parent
report = json.loads((root/"reports"/"validation_report.json").read_text())
results = []
for item in report["parts"]:
    name = item["name"]
    doc = g.App.openDocument(str(root/"freecad"/(name+".FCStd")))
    shape = next(o.Shape.copy() for o in doc.Objects if hasattr(o,"Shape") and not o.Shape.isNull())
    g.App.closeDocument(doc.Name)
    step = g.Part.read(str(root/"step"/(name+".step")))
    row = {"name":name,"fcstd_valid":shape.isValid(),"fcstd_solids":len(shape.Solids),
           "step_valid":step.isValid(),"step_solids":len(step.Solids),
           "step_volume_error_percent":100*abs(shape.Volume-step.Volume)/shape.Volume}
    if item["mesh_exported"]:
        mesh = g.Mesh.Mesh(str(root/"stl"/(name+".stl")))
        row.update({"stl_closed":mesh.isSolid(),"stl_components":mesh.countComponents(),
                    "stl_self_intersections":mesh.hasSelfIntersections(),
                    "stl_bbox_mm":[mesh.BoundBox.XLength,mesh.BoundBox.YLength,mesh.BoundBox.ZLength],
                    "stl_volume_error_percent":100*abs(shape.Volume-abs(mesh.Volume))/shape.Volume})
    row["pass"]=(row["fcstd_valid"] and row["fcstd_solids"]==1 and
                 row["step_valid"] and row["step_solids"]==1 and
                 row["step_volume_error_percent"]<.1 and row.get("stl_closed",True) and
                 row.get("stl_components",1)==1 and not row.get("stl_self_intersections",False) and
                 row.get("stl_volume_error_percent",0)<.5)
    results.append(row)
    print("EXPORT_CHECK",json.dumps(row))
assembly = g.App.openDocument(str(root/"freecad"/"PoolControllerV14MP1_Assembly.FCStd"))
stored_screws = assembly.getObject("WallScrewReferences").Shape.copy()
expected_screws = g.Part.makeCompound([g.build_wall_screw_reference(x,y) for x,y in g.WALL_MOUNTS])
common_volume = stored_screws.common(expected_screws).Volume
assembly_check = {
    "stored_wall_screws_valid":stored_screws.isValid(),
    "stored_wall_screws_solids":len(stored_screws.Solids),
    "wall_screws_geometry_difference_mm3":stored_screws.Volume+expected_screws.Volume-2*common_volume,
    "wall_screws_pcb_collision_mm3":stored_screws.common(assembly.getObject("PCBReference").Shape).Volume,
    "wall_screws_components_collision_mm3":stored_screws.common(assembly.getObject("PCBEnvelope").Shape).Volume}
assembly_check["pass"] = (assembly_check["stored_wall_screws_valid"]
    and assembly_check["stored_wall_screws_solids"]==4
    and abs(assembly_check["wall_screws_geometry_difference_mm3"])<=.01
    and assembly_check["wall_screws_pcb_collision_mm3"]<=.01
    and assembly_check["wall_screws_components_collision_mm3"]<=.01)
plate=assembly.getObject('WallBackplate').Shape.fuse(assembly.getObject('TechnicalSkin').Shape)
guard=assembly.getObject('PSUGuard').Shape
expected_barrier=g.build_mains_barrier()
barrier_missing=expected_barrier.Volume-expected_barrier.common(plate).Volume
assembly_check['barrier_front_end_z_mm']=g.REAR+g.MAINS_BARRIER_H
assembly_check['barrier_missing_volume_mm3']=barrier_missing
assembly_check['pass']=assembly_check['pass'] and abs(barrier_missing)<=.01
expected_guard=g.placed_psu_guard(g.build_psu_guard())
guard_delta=guard.Volume+expected_guard.Volume-2*guard.common(expected_guard).Volume
expected_baffle=g.placed_psu_guard(g.build_guard_barrier_baffle())
baffle_missing=expected_baffle.Volume-expected_baffle.common(guard).Volume
assembly_check['guard_geometry_difference_mm3']=guard_delta
assembly_check['baffle_missing_volume_mm3']=baffle_missing
assembly_check['barrier_guard_clearance_mm']=expected_barrier.distToShape(guard)[0]
assembly_check['pass']=assembly_check['pass'] and abs(guard_delta)<=.01 and abs(baffle_missing)<=.01
wiring_checks=[]
for name,expected in g.build_wiring_routes().items():
    stored=assembly.getObject('RPSWiring_'+name).Shape
    difference=stored.Volume+expected.Volume-2*stored.common(expected).Volume
    row={'name':name,'geometry_difference_mm3':difference,
         'guard_collision_mm3':stored.common(guard).Volume,
         'plate_collision_mm3':stored.common(plate).Volume}
    row['pass']=abs(difference)<=.01 and row['guard_collision_mm3']<=.01 and row['plate_collision_mm3']<=.01
    wiring_checks.append(row)
assembly_check['rps_wiring_checks']=wiring_checks
assembly_check['guard_plate_collision_mm3']=guard.common(plate).Volume
assembly_check['pass']=assembly_check['pass'] and all(r['pass'] for r in wiring_checks) and assembly_check['guard_plate_collision_mm3']<=.01
mounts=g.rps_guard_mount_checks(plate,guard,assembly.getObject('PCBReference').Shape,
    assembly.getObject('PCBEnvelope').Shape,assembly.getObject('PSUReference').Shape,
    {name:assembly.getObject('RPSWiring_'+name).Shape for name in g.build_wiring_routes()})
assembly_check['rps_guard_mounts']=mounts
assembly_check['pass']=assembly_check['pass'] and mounts['pass']
pg7_checks=[]
for i,(x,z) in enumerate(g.PG7_LAYOUT,1):
    stored=assembly.getObject('PG7_'+str(i)).Shape
    expected=g.build_pg7_body(x,z)
    delta=stored.Volume+expected.Volume-2*stored.common(expected).Volume
    actual_z=(stored.BoundBox.ZMin+stored.BoundBox.ZMax)/2
    bore=g.Part.makeCylinder(g.PG7_D/2-.05,g.TECH_TOTAL_T+.2,
        g.vec(x,-.1,z),g.vec(0,1,0))
    bore_volume=bore.common(plate).Volume
    pg7_checks.append({'index':i,'x_mm':x,'z_mm':z,'stored_z_mm':actual_z,
        'difference_mm3':delta,'bore_obstruction_mm3':bore_volume,
        'pass':abs(delta)<=.01 and abs(actual_z-g.PG7_ROW_Z)<=1e-6 and bore_volume<=.01})
stored_nuts=assembly.getObject('PG7InternalNutReferences').Shape
expected_nuts=g.Part.makeCompound([g.build_pg7_body(x,z,True) for x,z in g.PG7_LAYOUT])
nut_delta=stored_nuts.Volume+expected_nuts.Volume-2*stored_nuts.common(expected_nuts).Volume
nut_obstructions={k:stored_nuts.common(shape).Volume for k,shape in
    {'PCB':assembly.getObject('PCBReference').Shape,'components':assembly.getObject('PCBEnvelope').Shape,
     'plate':plate,'guard':guard}.items()}
assembly_check['pg7_checks']=pg7_checks
assembly_check['stored_nuts_difference_mm3']=nut_delta
assembly_check['stored_nuts_collisions_mm3']=nut_obstructions
assembly_check['pass']=assembly_check['pass'] and all(r['pass'] for r in pg7_checks) and abs(nut_delta)<=.01 and max(nut_obstructions.values())<=.01
g.App.closeDocument(assembly.Name)
out={"generator_sha256":hashlib.sha256((root/"generate_freecad.py").read_bytes()).hexdigest(),
     "validation_report_sha256":hashlib.sha256((root/"reports"/"validation_report.json").read_bytes()).hexdigest(),
     "checks":results,"assembly_check":assembly_check,
     "pass":report["pass"] and all(r["pass"] for r in results) and assembly_check["pass"]}
(root/"reports"/"exports_reloaded.json").write_text(json.dumps(out,indent=2)+"\n")
if not out["pass"]:
    raise RuntimeError("Echec des controles apres relecture")
print("Tous les fichiers FCStd/STEP/STL recharges sont conformes.")
