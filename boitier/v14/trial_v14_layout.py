"""Diagnostic rapide avant generation ; aucun export ni succes de livraison."""
from pathlib import Path
import sys,json
sys.path.insert(0,str(Path(__file__).resolve().parent))
import generate_freecad as g
plate=g.build_backplate_monobloc(g.build_wall_backplate(),g.build_backplate_technical_skin())
skin=g.build_cover_graphite_skin();accent=g.build_cover_turquoise_inlay()
cap=g.build_cover_monobloc(g.build_cover_light_core(skin,accent),skin,accent)
cap.translate(g.vec(0,0,g.COVER_ASSEMBLY_Z))
pcb,components=g.build_pcb_reference_shapes()
guard=g.placed_psu_guard(g.build_psu_guard())
psu=g.build_psu_reference_shape()
wires=g.build_wiring_routes()
bay=g.build_pcb_service_bay()
obstacles={'plate':plate,'cap':cap,'guard':guard,'psu':psu,
 'nuts':g.Part.makeCompound([g.build_pg7_body(x,z,True) for x,z in g.PG7_LAYOUT]),
 'corridors':g.build_pg7_corridors(),**{'wire_'+k:v for k,v in wires.items()}}
for name,shape in {'pcb':pcb,'components':components}.items():
 print('OBJECT',name,'COLLISIONS',json.dumps({k:shape.common(s).Volume for k,s in obstacles.items()}),
       'GAPS',json.dumps({k:shape.distToShape(s)[0] for k,s in obstacles.items()}),flush=True)
print('SERVICE_BAY',json.dumps({k:bay.common(s).Volume for k,s in
 {'plate':plate,'cap':cap,'guard':guard,'psu':psu,'pcb':pcb,'components':components}.items()}),flush=True)
for x,y in g.WALL_MOUNTS:
 path=g.Part.makeCylinder(g.wall_head_d(x,y)/2,g.WALL_ACCESS_END_Z-g.WALL_HEAD_SEAT_Z,g.vec(x,y,g.WALL_HEAD_SEAT_Z))
 print('WALL_ACCESS',x,y,json.dumps({k:path.common(s).Volume for k,s in
   {'plate':plate,'pcb':pcb,'components':components,**wires}.items()}),flush=True)
removal=[]
for forward in range(0,61,3):
 candidate=guard.translated(g.vec(0,0,forward))
 for name,shape in {'plate':plate,'PCB':pcb,**wires}.items():
  volume=candidate.common(shape).Volume
  if volume>.1:removal.append({'forward':forward,'obstacle':name,'volume':volume})
print('GUARD_REMOVAL',json.dumps(removal),flush=True)
