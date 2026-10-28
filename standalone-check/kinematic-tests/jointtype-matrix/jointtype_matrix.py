"""
jointtype_matrix.py - Testgeruest fuer die restlichen Gelenktypen (Plan
plans/2026-10-28-restliche-gelenktypen.md, Phase 0).

Eine Tabelle FALL x KONTEXT statt einer Handarbeitsdatei je Gelenktyp und Verschachtelungstiefe:

FAELLE
  - Paar-Gelenke (zwei Quader BoxA geerdet, BoxB): Parallel, Perpendicular, Angle, Distance in den
    Unterarten, die Quader hergeben (Punkt-Punkt, Punkt-Ebene, Ebene-Ebene, Linie-Linie,
    Linie-Ebene). Geprueft wird die Zwangsbedingung selbst an den JCS-Weltlagen (wie
    joint_test_utils.check_*), nicht eine bestimmte Loesung.
  - Mechanismen (BoxA geerdet als Gestell, BoxB und BoxD mit Dreh-/Schiebegelenk am Gestell):
    Gears, Belt, RackPinion, Screw. Nach dem Loesen wird BoxD um seine Drehachse gezogen
    (preDrag/doDragStep) und das Uebersetzungsverhaeltnis zu BoxB geprueft (Betrag; das
    Vorzeichen wird ausgegeben und in Phase 1 gegen Vanilla verglichen).

KONTEXTE
  - flat:      alles in einem Dokument.
  - nested:    Inhalt in Sub (BoxA dort geerdet); Top: BoxC geerdet -[Fixed]- Spiegel BoxA,
               SubLink flexibel; Startlage = erfuellte Verankerung mal LINK_PERTURB (gestoert,
               aber im Einzugsgebiet des Solvers - weiter Start ist eine Solver-Frage, auch
               Vanilla, siehe Plan Phase 1).
  - double:    Sub -> Mid (BoxC geerdet -[Fixed]- Spiegel BoxA) -> Top (BoxC geerdet -[Fixed]-
               Spiegel des Mid-BoxC).
  - duplicate: Sub zweimal flexibel in Top, jede Instanz mit eigenem Fixed an BoxC.
  In allen verschachtelten Kontexten zusaetzlich: aeussere Verankerung exakt, Originale in den
  Unterdokumenten unveraendert (requirement-solver-writes-mirrors-not-originals).

Fixturen: build_fixtures.py baut je FALL__KONTEXT einen Ordner unter fixtures/ (feste,
versionierte Ausgangsdateien). run_matrix.py kopiert sie, loest, prueft und gibt eine Tabelle aus.
"""
import math
import os

import FreeCAD as App

import joint_test_utils as jtu

Z = App.Vector(0, 0, 1)
X = App.Vector(1, 0, 0)

# JointObject.JointTypes
FIXED, REVOLUTE, SLIDER = 0, 1, 3
DISTANCE, PARALLEL, PERPENDICULAR, ANGLE = 5, 6, 7, 8
RACKPINION, SCREW, GEARS, BELT = 9, 10, 11, 12

TOL_POS = 1e-6
TOL_DIR = 1e-6
TOL_DEG = 1e-4
TOL_RATIO = 1e-3

CONTEXTS = ("flat", "nested", "double", "duplicate")

OUTER_PLC1 = jtu.face_jcs_placement("Face2", flip=False, extra_z_rotation_deg=30)  # an BoxC
OUTER_PLC2 = jtu.face_jcs_placement("Face1", flip=True, extra_z_rotation_deg=15)  # an BoxA
OUTER_PLC1_B = jtu.face_jcs_placement("Face4", flip=False, extra_z_rotation_deg=50)  # 2. Instanz
LINK_PERTURB = App.Placement(App.Vector(4, -3, 2), App.Rotation(App.Vector(1, 0, 1), 10))
LINK_PERTURB_B = App.Placement(App.Vector(-2, 3, -4), App.Rotation(App.Vector(0, 1, 1), -12))
B_PERTURB = App.Placement(App.Vector(3, -2, 4), App.Rotation(App.Vector(1, 1, 0), 20))


def zdir(plc):
    return plc.Rotation.multVec(Z)


# ------------------------------------------------------------------ Paar-Pruefungen
# j1/j2: JCS-Weltlagen der Seiten Reference1 (an BoxA) und Reference2 (an BoxB).

def check_parallel(j1, j2, props):
    c = zdir(j1).cross(zdir(j2)).Length
    return c <= TOL_DIR, f"|z1 x z2| = {c:.2e}"


def check_perpendicular(j1, j2, props):
    d = abs(zdir(j1).dot(zdir(j2)))
    return d <= TOL_DIR, f"|z1 . z2| = {d:.2e}"


def check_angle(j1, j2, props):
    cosv = max(-1.0, min(1.0, zdir(j1).dot(zdir(j2))))
    deg = math.degrees(math.acos(cosv))
    want = props["Angle"]
    return abs(deg - want) <= TOL_DEG, f"Winkel z1/z2 = {deg:.6f} Grad (soll {want})"


def check_point_point(j1, j2, props):
    d = (j2.Base - j1.Base).Length
    return abs(d - props["Distance"]) <= TOL_POS, f"Abstand = {d:.9f} (soll {props['Distance']})"


def check_plane_plane(j1, j2, props):
    c = zdir(j1).cross(zdir(j2)).Length
    d = abs((j2.Base - j1.Base).dot(zdir(j1)))
    ok = c <= TOL_DIR and abs(d - props["Distance"]) <= TOL_POS
    return ok, f"|n1 x n2| = {c:.2e}, Ebenenabstand = {d:.9f} (soll {props['Distance']})"


def check_point_plane(j1, j2, props):
    d = abs((j2.Base - j1.Base).dot(zdir(j1)))
    return abs(d - props["Distance"]) <= TOL_POS, f"Abstand Punkt-Ebene = {d:.9f} (soll {props['Distance']})"


def check_line_line(j1, j2, props):
    z1, z2 = zdir(j1), zdir(j2)
    w = j2.Base - j1.Base
    n = z1.cross(z2)
    if n.Length <= TOL_DIR:
        d = w.cross(z1).Length
    else:
        d = abs(w.dot(n)) / n.Length
    return abs(d - props["Distance"]) <= TOL_POS, f"Abstand Linie-Linie = {d:.9f} (soll {props['Distance']})"


def check_line_plane(j1, j2, props):
    n, z2 = zdir(j1), zdir(j2)
    par = abs(n.dot(z2))
    d = abs((j2.Base - j1.Base).dot(n))
    ok = par <= TOL_DIR and abs(d - props["Distance"]) <= TOL_POS
    return ok, f"|n . Linie| = {par:.2e}, Abstand = {d:.9f} (soll {props['Distance']})"


# Kante-Ebene: bei deckungsgleichen JCS steht die Kante senkrecht auf der Ebene - dort hat
# "Linie parallel zur Ebene" keine Steigung (singulaer), 20 Grad daneben bleibt Newton je nach
# Lage haengen. Startlage deshalb mit der Kante in der Ebene (90 Grad um x), dann gestoert.
LINE_IN_PLANE = App.Placement(App.Vector(), App.Rotation(App.Vector(1, 0, 0), 90))


def fjcs(face, flip=False, rot=0):
    return jtu.face_jcs_placement(face, flip=flip, extra_z_rotation_deg=rot)


def ejcs(edge, flip=False, rot=0):
    return jtu.edge_jcs_placement(edge, flip=flip, extra_z_rotation_deg=rot)


def vjcs(vertex):
    return jtu.vertex_jcs_placement(vertex)


PAIR_CASES = {
    "parallel": dict(type=PARALLEL, elem=("Face6", "Face5"), plc=(fjcs("Face6", rot=10), fjcs("Face5", True, 25)),
                     props={}, check=check_parallel),
    "perpendicular": dict(type=PERPENDICULAR, elem=("Face6", "Face1"), plc=(fjcs("Face6"), fjcs("Face1", rot=15)),
                          props={}, check=check_perpendicular),
    "angle": dict(type=ANGLE, elem=("Face6", "Face1"), plc=(fjcs("Face6"), fjcs("Face1", rot=15)),
                  props={"Angle": 35.0}, check=check_angle),
    "distance_pointpoint": dict(type=DISTANCE, elem=("Vertex1", "Vertex2"), plc=(vjcs("Vertex1"), vjcs("Vertex2")),
                                props={"Distance": 15.0}, check=check_point_point),
    "distance_pointplane": dict(type=DISTANCE, elem=("Face6", "Vertex2"), plc=(fjcs("Face6"), vjcs("Vertex2")),
                                props={"Distance": 8.0}, check=check_point_plane),
    "distance_planeplane": dict(type=DISTANCE, elem=("Face6", "Face5"), plc=(fjcs("Face6"), fjcs("Face5", True)),
                                props={"Distance": 12.0}, check=check_plane_plane),
    "distance_lineline": dict(type=DISTANCE, elem=("Edge10", "Edge9"), plc=(ejcs("Edge10"), ejcs("Edge9", rot=20)),
                              props={"Distance": 6.0}, check=check_line_line),
    "distance_lineplane": dict(type=DISTANCE, elem=("Face6", "Edge9"), plc=(fjcs("Face6"), ejcs("Edge9")),
                               props={"Distance": 4.0}, check=check_line_plane, start=LINE_IN_PLANE),
    # Punkt bzw. Kante als ERSTE Referenz: getDistanceType() tauscht dann per swapJCS() die Seiten
    # im Gelenkobjekt selbst (Plan Phase 2) - bei verschachtelten Gelenken das Original im
    # Unterdokument. Gebaut ohne Solve (SolveInJointCreation/SolveOnRecompute aus), sonst tauscht
    # schon das eigene Dokument. reverse: Referenz 1 = Punkt/Kante an der beweglichen BoxB,
    # Referenz 2 = Flaeche an der geerdeten BoxA (Flaeche am beweglichen Teil weit vom Ursprung
    # konvergiert in OndselSolver nicht, auch Vanilla - eigenes Thema, nicht der Tausch).
    "distance_pointfirst": dict(type=DISTANCE, elem=("Vertex2", "Face6"), plc=(vjcs("Vertex2"), fjcs("Face6")),
                                props={"Distance": 8.0}, check=lambda j1, j2, p: check_point_plane(j2, j1, p),
                                no_solve_on_create=True, reverse=True),
    "distance_edgefirst": dict(type=DISTANCE, elem=("Edge9", "Face6"), plc=(ejcs("Edge9"), fjcs("Face6")),
                               props={"Distance": 4.0}, check=lambda j1, j2, p: check_line_plane(j2, j1, p),
                               no_solve_on_create=True, reverse=True, start=LINE_IN_PLANE),
}


# ------------------------------------------------------------------ Mechanismen
# Gestell BoxA (Ursprung, geerdet). BoxB/BoxD mit Unterseitenmitte (5,5,0) an ihrem Gelenk.
UP = App.Rotation()
ALONG_X = App.Rotation(Z, X)


def P(x, y, z, rot=UP):
    return App.Placement(App.Vector(x, y, z), rot)


BOTTOM = P(5, 5, 0)
BOTTOM_X = P(5, 5, 0, ALONG_X)

MECH_CASES = {
    # Zahnraeder: Achsen im Abstand r1 + r2 = 15, aussen kaemmend.
    "gears": dict(
        carriers=[("BoxB", REVOLUTE, P(0, 0, 10), BOTTOM), ("BoxD", REVOLUTE, P(15, 0, 10), BOTTOM)],
        coupling=(GEARS, "BoxB", "BoxD", BOTTOM, BOTTOM, {"Distance": 10.0, "Distance2": 5.0}),
        expect=("rotation", 5.0 / 10.0), drive_axis_on_A=P(15, 0, 10)),
    # Koaxiale Zahnraeder (Upstream "Coaxial gear joint" #31155): beide Gelenk-JCS auf einer Achse,
    # der Solver braucht den Traeger-Marker (findRotationCarrierForGearSide()).
    "gears_coaxial": dict(
        carriers=[("BoxB", REVOLUTE, P(0, 0, 10), BOTTOM), ("BoxD", REVOLUTE, P(0, 0, 10), BOTTOM)],
        coupling=(GEARS, "BoxB", "BoxD", BOTTOM, BOTTOM, {"Distance": 10.0, "Distance2": 5.0}),
        expect=("rotation", 5.0 / 10.0), drive_axis_on_A=P(0, 0, 10)),
    "belt": dict(
        carriers=[("BoxB", REVOLUTE, P(0, 0, 10), BOTTOM), ("BoxD", REVOLUTE, P(40, 0, 10), BOTTOM)],
        coupling=(BELT, "BoxB", "BoxD", BOTTOM, BOTTOM, {"Distance": 10.0, "Distance2": 5.0}),
        expect=("rotation", 5.0 / 10.0), drive_axis_on_A=P(40, 0, 10)),
    # Zahnstange BoxB schiebt entlang x, Ritzel BoxD dreht um z, Teilkreisradius 5.
    "rackpinion": dict(
        carriers=[("BoxB", SLIDER, P(0, 0, 10, ALONG_X), BOTTOM_X), ("BoxD", REVOLUTE, P(0, 20, 10), BOTTOM)],
        coupling=(RACKPINION, "BoxB", "BoxD", BOTTOM_X, BOTTOM, {"Distance": 5.0}),
        expect=("translation", 5.0), drive_axis_on_A=P(0, 20, 10)),
    # Mutter BoxB schiebt entlang z, Spindel BoxD dreht koaxial, Steigung 2 mm/Umdrehung.
    "screw": dict(
        carriers=[("BoxB", SLIDER, P(0, 0, 10), BOTTOM), ("BoxD", REVOLUTE, P(0, 0, 10), BOTTOM)],
        coupling=(SCREW, "BoxB", "BoxD", BOTTOM, BOTTOM, {"Distance": 2.0}),
        expect=("translation", 2.0 / (2 * math.pi)), drive_axis_on_A=P(0, 0, 10)),
    # Wie BG37: die Mutter BoxB hat keinen eigenen Slider, sie haengt per Fixed am Schlitten BoxC,
    # erst der gleitet. Die Slider-Suche des Schrauben-Gelenks muss der starren Gruppe folgen.
    "screw_fixed_chain": dict(
        carriers=[("BoxC", SLIDER, P(0, 0, 10), BOTTOM), ("BoxD", REVOLUTE, P(0, 0, 10), BOTTOM)],
        fixed=[("BoxB", "BoxC", P(5, 5, 10), BOTTOM)],
        coupling=(SCREW, "BoxB", "BoxD", BOTTOM, BOTTOM, {"Distance": 2.0}),
        expect=("translation", 2.0 / (2 * math.pi)), drive_axis_on_A=P(0, 0, 10)),
}

DRIVE_STEPS = 4
DRIVE_STEP_DEG = 6.0


def all_cases():
    return list(PAIR_CASES) + list(MECH_CASES)


def case_id(case, context):
    return f"{case}__{context}"


# ------------------------------------------------------------------ Aufbau

def _box(doc, asm, label, directory):
    paths = jtu.ensure_box_bodies_in(directory, [label])
    obj = jtu.link_external_box(doc, label, paths[label])
    asm.addObject(obj)
    return obj


def _joint(asm, type_index, a, b, plc1, plc2, elems=("Face1", "Face1"), props=None):
    joint = jtu.make_joint(asm, type_index, a, b, plc1, plc2, elem1=elems[0], elem2=elems[1])
    for key, value in (props or {}).items():
        setattr(joint, key, value)
    return joint


def build_content(case, doc, asm, directory):
    """Gelenke des Falls in 'asm'. BoxA wird geerdet. Rueckgabe: {Label: Objekt}."""
    a = _box(doc, asm, "BoxA", directory)
    doc.recompute()
    jtu.ground_object(a)
    objs = {"BoxA": a}
    if case in PAIR_CASES:
        spec = PAIR_CASES[case]
        b = _box(doc, asm, "BoxB", directory)
        objs["BoxB"] = b
        doc.recompute()
        first, second = (b, a) if spec.get("reverse") else (a, b)
        _joint(asm, spec["type"], first, second, spec["plc"][0], spec["plc"][1], spec["elem"], spec["props"])
        # Start: JCS deckungsgleich, dann gestoert - der Solver muss die Bedingung herstellen.
        start = spec.get("start", App.Placement())
        if spec.get("reverse"):
            b.Placement = a.Placement.multiply(spec["plc"][1]).multiply(start).multiply(spec["plc"][0].inverse()).multiply(B_PERTURB)
        else:
            b.Placement = a.Placement.multiply(spec["plc"][0]).multiply(start).multiply(spec["plc"][1].inverse()).multiply(B_PERTURB)
    else:
        spec = MECH_CASES[case]
        for label, jtype, plc_a, plc_part in spec["carriers"]:
            part = _box(doc, asm, label, directory)
            objs[label] = part
            doc.recompute()
            _joint(asm, jtype, a, part, plc_a, plc_part, ("Face6", "Face5"))
            part.Placement = a.Placement.multiply(plc_a).multiply(plc_part.inverse())
        for label, other, plc_other, plc_part in spec.get("fixed", []):
            part = _box(doc, asm, label, directory)
            objs[label] = part
            doc.recompute()
            _joint(asm, FIXED, objs[other], part, plc_other, plc_part, ("Face6", "Face5"))
            part.Placement = objs[other].Placement.multiply(plc_other).multiply(plc_part.inverse())
        ctype, l1, l2, plc1, plc2, props = spec["coupling"]
        _joint(asm, ctype, objs[l1], objs[l2], plc1, plc2, ("Face5", "Face5"), props)
    doc.recompute()
    return objs


def _new_doc(path, name):
    doc = App.newDocument(name)
    doc.saveAs(path)
    return doc


def _new_asm(doc):
    return doc.addObject("Assembly::AssemblyObject", "Assembly")


def _link(asm, linked_asm, label, placement):
    link = asm.newObject("Assembly::AssemblyLink", linked_asm.Label)
    link.LinkedObject = linked_asm
    link.Rigid = False
    link.Label = label
    asm.addObject(link)
    link.Placement = placement
    return link


def _mirror(container, label):
    for child in container.Group:
        if child.Label == label:
            return child
    raise AssertionError(f"Spiegel {label} nicht in {container.Label}")


def _place_anchored(link, mirror_label, anchor, plc1, perturb):
    """Link so legen, dass sein Spiegel 'mirror_label' die Verankerung an 'anchor' (JCS plc1 /
    OUTER_PLC2) erfuellt, dann um 'perturb' (im Spiegel-Rahmen) stoeren."""
    mirror = _mirror(link, mirror_label)
    want = anchor.Placement.multiply(plc1).multiply(OUTER_PLC2.inverse()).multiply(perturb)
    link.Placement = want.multiply(mirror.Placement.inverse())


SOLVE_PREFS = ("SolveInJointCreation", "SolveOnRecompute")


def build_fixture(case, context, directory):
    """Baut FALL x KONTEXT in 'directory' (top.FCStd + ggf. sub/mid + Koerperdateien)."""
    spec = PAIR_CASES.get(case, {})
    if not spec.get("no_solve_on_create"):
        return _build_fixture(case, context, directory)
    # Ohne Solve bauen (beide Nutzereinstellungen aus): das Gelenk wird ungetauscht gespeichert,
    # der erste Solve - ggf. erst im verschachtelten Kontext - tauscht.
    prefs = App.ParamGet("User parameter:BaseApp/Preferences/Mod/Assembly")
    saved = {k: prefs.GetBool(k, True) for k in SOLVE_PREFS}
    for k in SOLVE_PREFS:
        prefs.SetBool(k, False)
    try:
        return _build_fixture(case, context, directory)
    finally:
        for k, v in saved.items():
            prefs.SetBool(k, v)


def _build_fixture(case, context, directory):
    os.makedirs(directory, exist_ok=True)
    App.ParamGet("User parameter:BaseApp/Preferences/Document").SetBool("DuplicateLabels", True)
    top_path = os.path.join(directory, "top.FCStd")
    if context == "flat":
        doc = _new_doc(top_path, "JTMTop")
        asm = _new_asm(doc)
        build_content(case, doc, asm, directory)
        doc.save()
        return top_path

    sub_doc = _new_doc(os.path.join(directory, "sub.FCStd"), "JTMSub")
    sub_asm = _new_asm(sub_doc)
    build_content(case, sub_doc, sub_asm, directory)
    sub_doc.save()

    if context in ("nested", "duplicate"):
        top = _new_doc(top_path, "JTMTop")
        top_asm = _new_asm(top)
        c = _box(top, top_asm, "BoxC", directory)
        top.recompute()
        jtu.ground_object(c)
        link1 = _link(top_asm, sub_asm, "SubLink", App.Placement())
        top.recompute()
        _place_anchored(link1, "BoxA", c, OUTER_PLC1, LINK_PERTURB)
        top.recompute()
        _joint(top_asm, FIXED, c, _mirror(link1, "BoxA"), OUTER_PLC1, OUTER_PLC2, ("Face2", "Face1"))
        if context == "duplicate":
            link2 = _link(top_asm, sub_asm, "SubLink2", App.Placement())
            top.recompute()
            _place_anchored(link2, "BoxA", c, OUTER_PLC1_B, LINK_PERTURB_B)
            top.recompute()
            _joint(top_asm, FIXED, c, _mirror(link2, "BoxA"), OUTER_PLC1_B, OUTER_PLC2, ("Face4", "Face1"))
        top.recompute()
        top.save()
        sub_doc.save()
        return top_path

    # double
    mid = _new_doc(os.path.join(directory, "mid.FCStd"), "JTMMid")
    mid_asm = _new_asm(mid)
    cm = _box(mid, mid_asm, "BoxC", directory)
    mid.recompute()
    jtu.ground_object(cm)
    sublink = _link(mid_asm, sub_asm, "SubLink", App.Placement())
    mid.recompute()
    _place_anchored(sublink, "BoxA", cm, OUTER_PLC1, LINK_PERTURB)
    mid.recompute()
    _joint(mid_asm, FIXED, cm, _mirror(sublink, "BoxA"), OUTER_PLC1, OUTER_PLC2, ("Face2", "Face1"))
    mid.recompute()
    mid.save()
    top = _new_doc(top_path, "JTMTop")
    top_asm = _new_asm(top)
    c = _box(top, top_asm, "BoxC", directory)
    top.recompute()
    jtu.ground_object(c)
    midlink = _link(top_asm, mid_asm, "MidLink", App.Placement())
    top.recompute()
    _place_anchored(midlink, "BoxC", c, OUTER_PLC1, LINK_PERTURB_B)
    top.recompute()
    _joint(top_asm, FIXED, c, _mirror(midlink, "BoxC"), OUTER_PLC1, OUTER_PLC2, ("Face2", "Face1"))
    top.recompute()
    top.save()
    mid.save()
    sub_doc.save()
    return top_path


# ------------------------------------------------------------------ Pruefung

def world_index(asm):
    """{Objekt: (Weltlage, Weltlage des Containers)} ueber alle AssemblyLink-Ebenen."""
    out = {}

    def rec(container, plc):
        for obj in container.Group:
            if "Placement" not in obj.PropertiesList:
                continue
            out[obj] = (plc.multiply(obj.Placement), plc)
            if obj.TypeId == "Assembly::AssemblyLink":
                rec(obj, plc.multiply(obj.Placement))

    rec(asm, App.Placement())
    return out


def content_instances(top_asm, context):
    """Je Instanz des Inhalts: (Bezeichnung, Container mit BoxA/BoxB/BoxD)."""
    if context == "flat":
        return [("flach", top_asm)]
    if context == "nested":
        return [("SubLink", _find(top_asm, "SubLink"))]
    if context == "duplicate":
        return [("SubLink", _find(top_asm, "SubLink")), ("SubLink2", _find(top_asm, "SubLink2"))]
    midlink = _find(top_asm, "MidLink")
    return [("MidLink.SubLink", _find(midlink, "SubLink"))]


def _find(container, label):
    for obj in container.Group:
        if obj.Label == label:
            return obj
    raise AssertionError(f"{label} nicht in {container.Label}")


def check_outer(top_asm, context, worlds):
    """Aeussere Verankerung (Fixed an BoxC) exakt."""
    if context == "flat":
        return True, []
    msgs, ok = [], True
    c = _find(top_asm, "BoxC")
    targets = []
    if context in ("nested", "duplicate"):
        targets.append(("SubLink", _find(_find(top_asm, "SubLink"), "BoxA"), OUTER_PLC1))
        if context == "duplicate":
            targets.append(("SubLink2", _find(_find(top_asm, "SubLink2"), "BoxA"), OUTER_PLC1_B))
    else:
        targets.append(("MidLink", _find(_find(top_asm, "MidLink"), "BoxC"), OUTER_PLC1))
    for name, obj, plc1 in targets:
        want = worlds[c][0].multiply(plc1)
        got = worlds[obj][0].multiply(OUTER_PLC2)
        dp = (want.Base - got.Base).Length
        dr = math.degrees(abs(want.Rotation.multiply(got.Rotation.inverted()).Angle))
        good = dp <= TOL_POS and min(dr, 360 - dr) <= TOL_DEG
        ok &= good
        msgs.append(f"Verankerung {name}: {dp:.2e} mm / {min(dr, 360 - dr):.2e} Grad -> {'OK' if good else 'FEHLER'}")
    return ok, msgs


JOINT_PROPS = ("Reference1", "Reference2", "Placement1", "Placement2")
_joint_snapshot = {}


def _joint_state(obj):
    out = []
    for prop in JOINT_PROPS:
        value = getattr(obj, prop)
        if prop.startswith("Reference"):
            out.append((value[0].Name if value and value[0] else None, tuple(value[1]) if value else ()))
        else:
            out.append(App.Placement(value))
    return out


def remember_joint_originals(top_doc):
    """Gelenk-Eigenschaften (Referenzen, JCS) in allen Unterdokumenten sichern."""
    _joint_snapshot.clear()
    for doc in App.listDocuments().values():
        if doc is top_doc:
            continue
        for obj in doc.Objects:
            if all(p in obj.PropertiesList for p in JOINT_PROPS):
                _joint_snapshot[(doc.Name, obj.Name)] = _joint_state(obj)


def joint_originals_unchanged():
    changed = []
    for (docname, objname), before in _joint_snapshot.items():
        obj = App.getDocument(docname).getObject(objname)
        for prop, a, b in zip(JOINT_PROPS, before, _joint_state(obj)):
            same = a.isSame(b, 1e-12) if isinstance(a, App.Placement) else a == b
            if not same:
                changed.append(f"{docname}#{objname}.{prop}")
    return not changed, changed


def check_pair(case, container, worlds):
    spec = PAIR_CASES[case]
    a, b = _find(container, "BoxA"), _find(container, "BoxB")
    if spec.get("reverse"):
        a, b = b, a
    j1 = worlds[a][0].multiply(spec["plc"][0])
    j2 = worlds[b][0].multiply(spec["plc"][1])
    return spec["check"](j1, j2, spec["props"])


def _angle_about(rot_start, rot_now, axis):
    delta = rot_now.multiply(rot_start.inverted())
    ang = delta.Angle
    if delta.Axis.dot(axis) < 0:
        ang = -ang
    # auf (-pi, pi] bringen: 339 Grad um die Gegenachse sind +21 Grad
    return math.atan2(math.sin(ang), math.cos(ang))


def _measure_mechanism(case, container, worlds_before, worlds_after):
    """Drehung BoxD um ihre Achse vs. Drehung/Weg BoxB zwischen zwei Weltlagen-Staenden."""
    spec = MECH_CASES[case]
    a, b, d = (_find(container, l) for l in ("BoxA", "BoxB", "BoxD"))
    axis = zdir(worlds_before[a][0].multiply(spec["drive_axis_on_A"]))
    theta_d = _angle_about(worlds_before[d][0].Rotation, worlds_after[d][0].Rotation, axis)
    kind, ratio = spec["expect"]
    carrier_axis = zdir(worlds_after[a][0].multiply(spec["carriers"][0][2]))
    if kind == "rotation":
        moved = _angle_about(worlds_before[b][0].Rotation, worlds_after[b][0].Rotation, carrier_axis)
        what = "Drehung BoxB"
    else:
        moved = (worlds_after[b][0].Base - worlds_before[b][0].Base).dot(carrier_axis)
        what = "Weg BoxB"
    if abs(theta_d) < math.radians(1):
        return False, f"BoxD hat sich nicht gedreht ({math.degrees(theta_d):.3f} Grad)"
    got = abs(moved / theta_d)
    sign = "+" if moved * theta_d >= 0 else "-"
    ok = abs(got - ratio) <= TOL_RATIO * max(1.0, ratio)
    return ok, (f"BoxD {math.degrees(theta_d):.3f} Grad, {what} {moved:.6f} -> |Verhaeltnis| {got:.6f} "
                f"(soll {ratio:.6f}), Vorzeichen {sign}")


def drive_mechanism(case, top_asm, container):
    """BoxD um seine Drehachse ziehen (preDrag/doDragStep, nur Sandbox), Verhaeltnis pruefen."""
    spec = MECH_CASES[case]
    before = world_index(top_asm)
    a, d = _find(container, "BoxA"), _find(container, "BoxD")
    axis_plc = before[a][0].multiply(spec["drive_axis_on_A"])
    axis = zdir(axis_plc)
    d_start, d_parent = before[d]
    top_asm.preDrag([d])
    for step in range(1, DRIVE_STEPS + 1):
        turn = App.Placement(App.Vector(), App.Rotation(axis, DRIVE_STEP_DEG * step), axis_plc.Base)
        d.Placement = d_parent.inverse().multiply(turn.multiply(d_start))
        top_asm.doDragStep()
    top_asm.postDrag()
    return _measure_mechanism(case, container, before, world_index(top_asm))


SIM_RATE = 0.4  # rad pro Sekunde fuer die Motion am Drehgelenk von BoxD


def _drive_joint_instance(top_asm, container):
    """Drehgelenk BoxA-BoxD im Dokument des Inhalts (+ nestingPrefix der Instanz, falls die
    Sandbox Gelenk-Instanzen kennt; Vanilla kennt nur das Gelenk selbst)."""
    import UtilsAssembly

    source = container
    while source is not top_asm and source.TypeId == "Assembly::AssemblyLink":
        source = source.LinkedObject  # Spiegel -> Link im Mid-Dokument -> Unterbaugruppe
    content_doc = source.Document
    joint = None
    for obj in content_doc.Objects:
        ref2 = getattr(obj, "Reference2", None)
        if getattr(obj, "JointType", None) == "Revolute" and ref2 and ref2[0] and ref2[0].Label == "BoxD":
            joint = obj
    if joint is None:
        return None, None
    if container is top_asm or not hasattr(UtilsAssembly, "getJointInstancesOfType"):
        return joint, ""
    # Das Praefix nennt die Link-Namen der Quelldokumente (z.B. "Assembly001.Assembly001."), nicht
    # die der Spiegel im obersten Dokument. Eine Instanz: eindeutig; mehrere (duplicate): die
    # Links haengen direkt in top_asm, dort stimmt der Name.
    prefixes = [p for inst, p in UtilsAssembly.getJointInstancesOfType(top_asm, ["Revolute"]) if inst == joint]
    if len(prefixes) == 1:
        return joint, prefixes[0]
    for prefix in prefixes:
        if prefix.endswith(container.Name + "."):
            return joint, prefix
    return joint, None


def simulate_mechanism(case, top_asm, container):
    """BoxD per Simulation (Motion "Angular" am Drehgelenk) antreiben, Verhaeltnis zu BoxB pruefen
    zwischen erstem und letztem Bild. Laeuft auch in Vanilla (dort fehlt preDrag in Python)."""
    import CommandCreateSimulation as CCS
    import UtilsAssembly

    joint, prefix = _drive_joint_instance(top_asm, container)
    if joint is None or prefix is None:
        return False, "Antriebsgelenk (Instanz) nicht gefunden"
    sim = UtilsAssembly.getSimulationGroup(top_asm).newObject("App::FeaturePython", "Simulation")
    CCS.Simulation(sim)
    motion = top_asm.newObject("App::FeaturePython", "Motion")
    CCS.Motion(motion, "Angular", (joint, [prefix]) if prefix else joint, f"initialValue + {SIM_RATE}*time")
    sim.Group = [motion]
    rc = top_asm.generateSimulation(sim)
    frames = top_asm.numberOfFrames() if rc == 0 else 0
    if rc != 0 or frames < 2:
        return False, f"generateSimulation rc={rc}, Bilder={frames}"
    top_asm.updateForFrame(0)
    before = world_index(top_asm)
    top_asm.updateForFrame(frames - 1)
    after = world_index(top_asm)
    top_asm.updateForFrame(0)
    ok, text = _measure_mechanism(case, container, before, after)
    return ok, "Simulation: " + text
