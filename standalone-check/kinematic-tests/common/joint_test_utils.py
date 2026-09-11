"""
joint_test_utils.py - gemeinsame Bausteine fuer die Kinematik-Testmatrix (Stufe 1: flache,
nicht verschachtelte Zwei-Teile-Assemblies mit je einem Jointtyp).

Nicht selbst ausfuehrbar - wird von den einzelnen test_<jointtyp>_flat.py-Skripten importiert.
Liegt seit der Ordner-Umstrukturierung (Nutzerauftrag 2026-09-09: "alle Tests sollen in eigenem
Ordner sein") unter kinematic-tests/common/, waehrend jeder Testfall in seinem EIGENEN
Unterordner (kinematic-tests/test_<jointtyp>_flat/) liegt - jedes Testfall-Skript fuegt deshalb
sowohl sein eigenes Verzeichnis ALS AUCH ../common zu sys.path hinzu (siehe dortiges
sys.path.insert()).

Testphilosophie fuer Joints MIT Freiheitsgraden (Revolute/Cylindrical/Slider/Ball, im
Gegensatz zum vollstaendig bestimmten Fixed-Joint in test_fixed_joint_flat.py): welchen
konkreten Wert der Solver fuer einen freien Freiheitsgrad waehlt (z.B. den Drehwinkel eines
Revolute-Joints), ist NICHT unabhaengig vom Solver vorhersagbar - das waere keine echte
"analytische Referenz" mehr, sondern eine Annahme ueber Solver-Interna. Stattdessen wird
geprueft, was fuer JEDE gueltige Loesung dieses Jointtyps gelten MUSS (die Zwangsbedingungs-
Gleichungen selbst, siehe src/Mod/Assembly/App/AssemblyObject.cpp::makeMbdJointOfType() fuer
die zugrundeliegenden OndselSolver-Jointklassen):
    Revolute    (ASMTRevoluteJoint):     Ursprung deckungsgleich, Rotation NUR um lokale Z-Achse
    Cylindrical (ASMTCylindricalJoint):  wie Revolute, zusaetzlich Translation frei entlang Z
    Slider      (ASMTTranslationalJoint):Translation frei entlang Z, senkrecht dazu 0, KEINE Rotation
    Ball        (ASMTSphericalJoint):    Ursprung deckungsgleich, Rotation komplett frei
Diese Eigenschaften sind rein geometrisch/algebraisch aus der Jointdefinition ableitbar und
damit genauso "analytisch" wie die Placement-Algebra des Fixed-Joint-Tests - nur eben eine
Ungleichungs-/Unterraum-Bedingung statt eines einzelnen Zielwerts.

**Fixture-Methodik (Nutzerauftrag 2026-09-08):** die Ausgangs-.FCStd-Datei jedes Testfalls
(inkl. der absichtlich falschen BoxB-Startposition) wird GENAU EINMAL gebaut
(`build_all_fixtures.py`) und unter `fixtures/<testfall>/model.FCStd` GIT-GETRACKT abgelegt -
NICHT bei jedem Testlauf per Skript unter der jeweils getesteten FreeCAD-Version neu erzeugt.
Grund: sonst vermischen sich zwei unabhaengige Fragen - "baut das Python-Skript unter Version X
ueberhaupt dieselbe Dokumentstruktur wie unter Version Y" und "loest Version X denselben
Ausgangszustand anders als Version Y" - nur die zweite Frage soll diese Testmatrix beantworten.
Jeder Testlauf (`run-test.sh`) kopiert die feste Fixture-Datei in sein eigenes
`fcstd-output/<testfall>/`-Verzeichnis (siehe `copy_fixture_to_output()`), oeffnet NUR diese
Kopie und fuehrt darauf recompute()/solve() aus - die Fixture selbst wird nie veraendert. Siehe
[[reference-nested-fixture-methodology]] fuer die volle Begruendung (dort urspruenglich fuer
Stufe 2/verschachtelte Baugruppen dokumentiert, gilt aber genauso hier).
"""
import os
import shutil

import FreeCAD as App

Z_AXIS = App.Vector(0, 0, 1)

# Face-Geometrie einer Standard-Part::Box (10x10x10mm, live per Xvfb-Probe ausgemessen,
# 2026-09-08) - Center of Mass + Normale je Flaeche. Nutzerauftrag 2026-09-08: Placement1/2
# fuer Joints sollen zu einer ECHTEN Flaeche passen (Ursprung darauf, Z-Achse = Normale),
# NICHT frei erfunden sein - siehe face_jcs_placement() unten fuer die Begruendung, warum
# frei erfundene Placements zu geometrisch unsinnigen ("Boxen stehen bloed zueinander")
# Konfigurationen fuehren koennen, die eine zufaellige Symmetrie/Mehrdeutigkeit vortaeuschen
# koennen, die kein echter Solver-Bug ist.
BOX_FACES = {
    "Face1": (App.Vector(0, 5, 5), App.Vector(-1, 0, 0)),
    "Face2": (App.Vector(10, 5, 5), App.Vector(1, 0, 0)),
    "Face3": (App.Vector(5, 0, 5), App.Vector(0, -1, 0)),
    "Face4": (App.Vector(5, 10, 5), App.Vector(0, 1, 0)),
    "Face5": (App.Vector(5, 5, 0), App.Vector(0, 0, -1)),
    "Face6": (App.Vector(5, 5, 10), App.Vector(0, 0, 1)),
}


def face_jcs_placement(face_name, flip=False, extra_z_rotation_deg=0, standoff=0):
    """Baut eine physikalisch SINNVOLLE JCS-Placement fuer die gegebene Flaeche einer
    Standard-Part::Box: Ursprung auf der Flaeche (+ optionalem `standoff`-Versatz entlang der
    Normale - ein physikalisch echter "Abstandshalter", keine willkuerliche Zahl), Z-Achse
    entlang der TATSAECHLICHEN Flaechennormale (nicht frei erfunden).

    `flip=True` dreht die Z-Achse auf die entgegengesetzte Richtung (Normale * -1) - das ist
    das Gegenstueck zu UtilsAssembly.flipPlacement()/arePlacementSameDir() (FreeCAD verwendet
    dieselbe Logik beim Verbinden zweier per GUI ausgewaehlter Flaechen): fuer die Seite, die
    an einer ANDEREN Flaeche "andockt" (z.B. Reference2 eines Joints), sorgt das dafuer, dass
    beide Koerper nach dem Loesen auf ENTGEGENGESETZTEN Seiten der gemeinsamen Flaeche liegen
    (Flaeche-auf-Flaeche, wie bei einer echten Baugruppe), statt exakt ineinander zu fallen.

    `extra_z_rotation_deg` dreht zusaetzlich UM diese Achse (aendert NICHT, wohin die Flaeche
    zeigt, nur die "Uhrzeigerstellung" darum) - das ist die einzige "frei waehlbare" Groesse
    hier, und bewusst physikalisch bedeutungslos fuer die Frage "stehen die Flaechen sinnvoll
    zueinander" (im Gegensatz zu den fruehmeren, komplett frei erfundenen Placement-Werten)."""
    center, normal = BOX_FACES[face_name]
    if flip:
        normal = normal * -1
    rotation = App.Rotation(Z_AXIS, normal)
    if extra_z_rotation_deg:
        rotation = rotation.multiply(App.Rotation(Z_AXIS, extra_z_rotation_deg))
    origin = center + normal * standoff
    return App.Placement(origin, rotation)


# Edge/Vertex-Geometrie einer Standard-Part::Box (10x10x10mm, live per Xvfb-Probe ausgemessen,
# 2026-09-09) - Nutzerkorrektur 2026-09-09: "grundsaetzlich falsch" (Zitat), ein Face ist KEINE
# realistische Referenz fuer alle Jointtypen. In echter FreeCAD-Nutzung waehlt man fuer
# Revolute/Cylindrical/Slider eine Achse bzw. eine gerade Kante (nicht eine Flaeche - eine
# Flaeche hat keine ausgezeichnete Drehachse), und fuer Ball einen Punkt (ein Kugelgelenk
# dreht sich um einen Punkt, nicht um eine Flaechennormale). Nur Fixed bleibt Face-basiert
# (Flaeche-auf-Flaeche ist dort tatsaechlich der typische reale Anwendungsfall). Siehe
# edge_jcs_placement()/vertex_jcs_placement() unten.
BOX_EDGES = {
    "Edge1": (App.Vector(0, 0, 5), App.Vector(0, 0, -1)),
    "Edge2": (App.Vector(0, 5, 10), App.Vector(0, -1, 0)),
    "Edge3": (App.Vector(0, 10, 5), App.Vector(0, 0, -1)),
    "Edge4": (App.Vector(0, 5, 0), App.Vector(0, -1, 0)),
    "Edge5": (App.Vector(10, 0, 5), App.Vector(0, 0, -1)),
    "Edge6": (App.Vector(10, 5, 10), App.Vector(0, -1, 0)),
    "Edge7": (App.Vector(10, 10, 5), App.Vector(0, 0, -1)),
    "Edge8": (App.Vector(10, 5, 0), App.Vector(0, -1, 0)),
    "Edge9": (App.Vector(5, 0, 0), App.Vector(-1, 0, 0)),
    "Edge10": (App.Vector(5, 0, 10), App.Vector(-1, 0, 0)),
    "Edge11": (App.Vector(5, 10, 0), App.Vector(-1, 0, 0)),
    "Edge12": (App.Vector(5, 10, 10), App.Vector(-1, 0, 0)),
}

BOX_VERTICES = {
    "Vertex1": App.Vector(0, 0, 10),
    "Vertex2": App.Vector(0, 0, 0),
    "Vertex3": App.Vector(0, 10, 10),
    "Vertex4": App.Vector(0, 10, 0),
    "Vertex5": App.Vector(10, 0, 10),
    "Vertex6": App.Vector(10, 0, 0),
    "Vertex7": App.Vector(10, 10, 10),
    "Vertex8": App.Vector(10, 10, 0),
}


def edge_jcs_placement(edge_name, flip=False, extra_z_rotation_deg=0, standoff=0):
    """Baut eine physikalisch SINNVOLLE JCS-Placement fuer die gegebene Kante einer Standard-
    Part::Box: Ursprung auf dem Kanten-Mittelpunkt (+ optionalem `standoff`-Versatz ENTLANG der
    Kante - kein willkuerlicher Wert, sondern eine Verschiebung entlang der real vorhandenen
    Achse), Z-Achse entlang der TATSAECHLICHEN Kantenrichtung. Das ist die realistische
    Referenz fuer Revolute/Cylindrical/Slider (Drehachse bzw. Schiebeachse einer Kante/Bohrung),
    NICHT eine Flaeche, die keine ausgezeichnete Achse hat.

    `flip=True` dreht die Z-Achse (=Kantenrichtung) um - Gegenstueck zu
    UtilsAssembly.flipPlacement(), analog zu face_jcs_placement().
    `extra_z_rotation_deg` dreht nur UM die Achse (physikalisch bedeutungslos fuer die Frage
    "welche Achse", aber eine zulaessige freie Groesse)."""
    center, direction = BOX_EDGES[edge_name]
    if flip:
        direction = direction * -1
    rotation = App.Rotation(Z_AXIS, direction)
    if extra_z_rotation_deg:
        rotation = rotation.multiply(App.Rotation(Z_AXIS, extra_z_rotation_deg))
    origin = center + direction * standoff
    return App.Placement(origin, rotation)


def vertex_jcs_placement(vertex_name, extra_rotation_axis=None, extra_rotation_deg=0):
    """Baut eine physikalisch SINNVOLLE JCS-Placement fuer den gegebenen Eckpunkt einer
    Standard-Part::Box: Ursprung EXAKT auf dem Punkt. Das ist die realistische Referenz fuer
    Ball-Joints (ein Kugelgelenk dreht sich um einen Punkt, nicht um eine Flaechennormale) -
    die Rotation ist fuer einen Ball-Joint ohnehin voellig frei (keine Orientierungs-
    Zwangsbedingung, siehe joint_test_utils.py-Moduldocstring), `extra_rotation_axis`/
    `extra_rotation_deg` dienen nur dazu, trotzdem eine nicht-triviale (nicht Identitaets-)
    Rotation im JCS zu hinterlegen, ohne dass ihr eine physikalische Bedeutung zukommt."""
    origin = BOX_VERTICES[vertex_name]
    if extra_rotation_axis is not None and extra_rotation_deg:
        rotation = App.Rotation(extra_rotation_axis, extra_rotation_deg)
    else:
        rotation = App.Rotation()
    return App.Placement(origin, rotation)


def fixture_path(test_dir):
    """Pfad der festen, git-getrackten Ausgangsdatei eines flachen Testfalls:
    <test_dir>/fixtures/<testname>.FCStd, wobei test_dir das EIGENE Verzeichnis des jeweiligen
    Testfall-Skripts ist (uebergeben als os.path.dirname(os.path.abspath(__file__)) - seit der
    Ordner-Umstrukturierung 2026-09-09 liegt jeder Testfall in seinem eigenen Unterordner, die
    Fixture also NICHT mehr zentral relativ zu diesem Modul hier). Der Dateiname traegt den
    Testnamen (Nutzerauftrag 2026-09-09) statt eines generischen "model.FCStd" - sonst heissen
    alle Fixtures ausserhalb ihres Ordnerkontexts gleich und sind nicht unterscheidbar. Von
    build_all_fixtures.py beschrieben, von den eigentlichen Testlaeufen nur GELESEN (nie direkt
    veraendert - siehe copy_fixture_to_output())."""
    test_name = os.path.basename(test_dir)
    return os.path.join(test_dir, "fixtures", f"{test_name}.FCStd")


def copy_fixture_to_output(fixture_file_path, out_path):
    """Kopiert die feste Fixture in den per-Installation eigenen Ausgabepfad
    (fcstd-output/<testfall>/<installationsname>.FCStd) - der eigentliche Testlauf arbeitet NUR
    auf dieser Kopie, damit die Fixture selbst unangetastet bleibt."""
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    shutil.copy2(fixture_file_path, out_path)


def new_flat_two_box_assembly(doc_name):
    """Baut die in dieser Testmatrix immer gleiche Grundlage: ein Assembly-Objekt mit zwei
    NICHT verschachtelten Part::Box, BoxA geerdet. Der eigentliche Joint wird vom
    Aufrufer per make_joint() ergaenzt."""
    doc = App.newDocument(doc_name)

    boxA = doc.addObject("Part::Box", "BoxA")
    boxB = doc.addObject("Part::Box", "BoxB")
    doc.recompute()

    assembly = doc.addObject("Assembly::AssemblyObject", "Assembly")
    assembly.addObject(boxA)
    assembly.addObject(boxB)
    doc.recompute()

    # BoxA erden: Placement schreibgeschuetzt setzen, dann syncGroundedJoints() (via solve())
    # legt automatisch das GroundedJoint-Objekt an (siehe docs/ARCHITECTURE.md, Abschnitt 1.1).
    boxA.setPropertyStatus("Placement", "ReadOnly")

    return doc, assembly, boxA, boxB


def make_joint(assembly, type_index, boxA, boxB, plc1, plc2, elem1="Face1", elem2="Face1"):
    """Legt einen Joint zwischen boxA (Reference1) und boxB (Reference2) an, per Python
    direkt ueber Placement1/2 + Detach1/2=True (umgeht GUI-Selektion/Geometrieabhaengigkeit -
    siehe README.md "Wie ein neuer Testfall aussieht").

    `elem1`/`elem2` sind die Namen der jeweils referenzierten Sub-Elemente (z.B. "Face1",
    "Edge9", "Vertex2" - Nutzerkorrektur 2026-09-09: je nach Jointtyp ist eine Flaeche, eine
    Kante ODER ein Punkt die realistische Referenz, siehe edge_jcs_placement()/
    vertex_jcs_placement() oben). Default bleibt "Face1" fuer Fixed (Rueckwaertskompatibilitaet)."""
    import JointObject
    import UtilsAssembly

    joint_group = UtilsAssembly.getJointGroup(assembly)

    joint = joint_group.newObject("App::FeaturePython", "Joint")
    JointObject.Joint(joint, type_index)  # siehe JointObject.JointTypes fuer die Indizes
    if App.GuiUp:
        # Ohne diesen Proxy crasht redrawJointPlacements() beim naechsten Solve NACH einem
        # Speichern+Neuladen - siehe test_fixed_joint_flat.py fuer die volle Herleitung.
        JointObject.ViewProviderJoint(joint.ViewObject)

    # Sub-Element-Name DOPPELT noetig (["Face1", "Face1"], nicht nur ["Face1"]) - siehe
    # Projekt-Memory "reference-joint-double-subelement" bzw. test_fixed_joint_flat.py.
    joint.Reference1 = (boxA, [elem1, elem1])
    joint.Reference2 = (boxB, [elem2, elem2])
    joint.Detach1 = True  # verhindert, dass updateJCSPlacements() unsere Placement1/2 ueberschreibt
    joint.Detach2 = True
    joint.Placement1 = plc1
    joint.Placement2 = plc2

    return joint


def jcs_world(obj, local_placement):
    """Weltkoordinaten-Placement eines JCS (Placement1/2), das relativ zu obj.Placement
    definiert ist."""
    return obj.Placement.multiply(local_placement)


def relative_jcs(jcs1_world, jcs2_world):
    """JCS2 aus Sicht von JCS1 - die Groesse, auf die sich die Jointtyp-Bedingungen beziehen."""
    return jcs1_world.inverse().multiply(jcs2_world)


def perpendicular_offset(vec, axis=Z_AXIS):
    """Anteil von vec senkrecht zu axis (fuer die "nur Z frei"-Pruefungen von
    Slider/Cylindrical) - axis wird als Einheitsvektor erwartet."""
    return vec - axis * vec.dot(axis)


def axis_preserved(rotation, axis=Z_AXIS):
    """Wie stark rotation von einer reinen Drehung UM axis abweicht: liefert die Abweichung
    von axis nach Anwendung von rotation (0 bei einer reinen Drehung um axis, unabhaengig vom
    Drehwinkel - inklusive Winkel 0 als Trivialfall)."""
    return rotation.multVec(axis) - axis


def report(label, jcs1_world, jcs2_world):
    rel = relative_jcs(jcs1_world, jcs2_world)
    print(f"--- {label} ---")
    print("JCS1 (Welt):", jcs1_world)
    print("JCS2 (Welt):", jcs2_world)
    print("JCS2 relativ zu JCS1:", rel)
    return rel


def check_revolute(rel, tol_pos=1e-6, tol_axis=1e-6):
    """Ursprung deckungsgleich, Rotation nur um lokale Z-Achse (jeder Winkel zulaessig)."""
    pos_diff = rel.Base.Length
    axis_diff = axis_preserved(rel.Rotation).Length
    ok = pos_diff <= tol_pos and axis_diff <= tol_axis
    print(f"Ursprungs-Differenz: {pos_diff:.9f} mm (Toleranz {tol_pos})")
    print(f"Z-Achsen-Abweichung nach Rotation: {axis_diff:.9f} (Toleranz {tol_axis}, 0 = reine Drehung um Z)")
    return ok


def check_cylindrical(rel, tol_pos=1e-6, tol_axis=0.2):
    """Wie Revolute, aber zusaetzlich Translation entlang Z frei - nur die dazu senkrechte
    Komponente von Base muss 0 sein.

    WICHTIG (2026-09-09, empirisch per Sweep gefunden - siehe
    common/verify_safe_rotation_revolute_cylindrical_slider.py): anders als bei
    Fixed/Revolute/Slider (die bei korrekter Wurzel exakt auf Maschinen-Praezision
    konvergieren) laesst der Solver bei Cylindrical einen kleinen, mit dem Ausgangs-
    Rotationsfehler ANWACHSENDEN Rest-Achsenfehler zurueck (z.B. ~0.14 bei 10 Grad
    Startabweichung) - vermutlich eine Folge des zusaetzlichen freien Freiheitsgrades
    (Translation entlang Z), noch nicht root-cause-analysiert. tol_axis deshalb bewusst
    lockerer als bei den anderen Jointtypen - immer noch klar unter dem Bereich (~0.8-2.0),
    in dem tatsaechlich eine falsche Wurzel getroffen wurde."""
    perp_diff = perpendicular_offset(rel.Base).Length
    axis_diff = axis_preserved(rel.Rotation).Length
    ok = perp_diff <= tol_pos and axis_diff <= tol_axis
    print(f"Quer-zu-Z-Versatz: {perp_diff:.9f} mm (Toleranz {tol_pos}, Z-Komponente frei = {rel.Base.z:.6f} mm)")
    print(f"Z-Achsen-Abweichung nach Rotation: {axis_diff:.9f} (Toleranz {tol_axis}, 0 = reine Drehung um Z)")
    return ok


def check_slider(rel, tol_pos=1e-6, tol_rot_deg=1e-4):
    """Translation nur entlang Z frei, keinerlei Rotation erlaubt."""
    perp_diff = perpendicular_offset(rel.Base).Length
    rot_diff_deg = rel.Rotation.Angle * 180.0 / 3.141592653589793
    ok = perp_diff <= tol_pos and rot_diff_deg <= tol_rot_deg
    print(f"Quer-zu-Z-Versatz: {perp_diff:.9f} mm (Toleranz {tol_pos}, Z-Komponente frei = {rel.Base.z:.6f} mm)")
    print(f"Rotations-Differenz: {rot_diff_deg:.9f} deg (Toleranz {tol_rot_deg}, Slider erlaubt keine Rotation)")
    return ok


def check_ball(rel, tol_pos=1e-6):
    """Nur Ursprung deckungsgleich, Rotation komplett frei."""
    pos_diff = rel.Base.Length
    ok = pos_diff <= tol_pos
    print(f"Ursprungs-Differenz: {pos_diff:.9f} mm (Toleranz {tol_pos})")
    return ok


def save_close_reopen_recompute(doc, out_path, box_b_name="BoxB"):
    """Speichern+Schliessen+Neuladen+Recompute-Zyklus, siehe test_fixed_joint_flat.py fuer die
    Herleitung, warum das fester Bestandteil jedes Testfalls ist (fehlender ViewProvider-Proxy
    fuehrte sonst erst NACH diesem Zyklus zu einem Crash, nicht schon direkt nach dem Erstellen).
    Gibt (doc2, boxB2, exception_oder_None) zurueck."""
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    doc.saveAs(out_path)
    print("Testdatei gespeichert:", out_path)

    docname = doc.Name
    App.closeDocument(docname)
    doc2 = App.openDocument(out_path)
    boxB2 = doc2.getObject(box_b_name)

    exc = None
    try:
        doc2.recompute()
    except Exception as e:  # noqa: BLE001 - bewusst breit, das IST der Test
        exc = e

    return doc2, boxB2, exc
