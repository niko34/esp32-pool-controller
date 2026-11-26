"""Rejoue les controles sur les FCStd effectivement livres, sans regenerer les pieces."""
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))
import generate_freecad as g

names={"backplate_core":"WallBackplateLightCore","technical_skin":"WallBackplateTechnicalSkin",
       "backplate_monobloc":"WallBackplateMonoblocReference","cover_core":"CoverLightCore",
       "graphite_skin":"CoverGraphiteSkin","turquoise_inlay":"CoverTurquoiseInlay",
       "cover_monobloc":"CoverMonoblocReference","psu_guard":"PSUGuard",
       "pump_coupon":"MP1FitCoupon","connector_coupon":"ConnectorFitCoupon",
       "m4_coupon":"M4InsertFitCoupon","gasket_coupon":"GasketFitCoupon",
       "wall_access_coupon":"WallAccessFitCoupon",
       "left_wall_access_coupon":"LeftWallAccessFitCoupon","pcb_coupon":"PCBFitCoupon"}
parts, rows = {}, []
for key,name in names.items():
    name="PoolControllerV14MP1_"+name
    doc=g.App.openDocument(str(g.FC_DIR/(name+".FCStd")))
    shape=next(o.Shape.copy() for o in doc.Objects if hasattr(o,"Shape") and not o.Shape.isNull())
    g.App.closeDocument(doc.Name)
    parts[key]=shape
    export_mesh=not name.endswith("MonoblocReference")
    closed=True
    if export_mesh:
        mesh=g.Mesh.Mesh(str(g.STL_DIR/(name+".stl")))
        closed=mesh.isSolid() and mesh.countComponents()==1
    rows.append({"name":name,"volume_mm3":round(shape.Volume,2),
                 "bbox_mm":[round(shape.BoundBox.XLength,2),round(shape.BoundBox.YLength,2),
                            round(shape.BoundBox.ZLength,2)],
                 "mesh_exported":export_mesh,"mesh_closed_connected":closed})
report=g.write_validation_report(rows,parts)
if not report["pass"] or not all(r["mesh_closed_connected"] for r in rows):
    raise RuntimeError("Echec validation des fichiers relus")
print("Validation V12 sur fichiers relus reussie.")
