"""
nested_test_utils.py - gemeinsame Bausteine fuer Stufe 2 der Kinematik-Testmatrix: EINE Ebene
Verschachtelung ueber eine FLEXIBLE Assembly::AssemblyLink (Rigid=False).

Nicht selbst ausfuehrbar - wird von den einzelnen test_*_nested_flex.py-Skripten importiert.
Liegt seit der Ordner-Umstrukturierung 2026-09-09 unter kinematic-tests/common/, waehrend jeder
Testfall in seinem eigenen Unterordner liegt (siehe dortiges sys.path.insert()).

Struktur, die diese Helper aufbauen (siehe docs/ARCHITECTURE.md Abschnitt 3 fuer die
zugrundeliegende Nomenklatur "GrandTop"/"Sub"):

    Sub (eigenes Dokument): BoxA (GEERDET) -[innerer Joint]- BoxB
        - BoxA's Erdung ist NICHT willkuerlich, sondern das uebliche Konstruktionsmuster
          (Nutzer-Korrektur 2026-09-08): eine Unterbaugruppe hat ihren eigenen internen
          Referenz-/Befestigungspunkt (hier BoxA), unabhaengig davon, ob/wo sie spaeter in
          eine groessere Baugruppe eingebaut wird - das ist normale Konstrukteurspraxis, kein
          Konflikt mit einem aeusseren Joint. Live verifiziert: eine geerdete BoxA INNERHALB
          von Sub UND ein aeusserer Joint auf BoxB (in GrandTop) sind NICHT redundant - der
          Solver behandelt Sub's Erdung als Definition des lokalen Referenzrahmens der
          Unterbaugruppe, den der aeussere Joint dann als Ganzes im Weltkoordinatensystem von
          GrandTop positioniert (frueherer Versuch dieser Datei, die Erdung wegzulassen "um
          Redundanz zu vermeiden", beruhte auf einer falschen Annahme - siehe Korrektur unten).

    GrandTop (eigenes Dokument): BoxC (geerdet) -[aeusserer Joint]- SubLink.BoxB (Spiegel)
        - SubLink = Assembly::AssemblyLink, LinkedObject=Sub#Assembly, Rigid=False
        - Der aeussere Joint positioniert die GESAMTE eingebettete Sub-Baugruppe (ueber den
          Spiegel von BoxB) im Weltkoordinatensystem von GrandTop.

Analytische Referenz (reine Placement-Algebra, Solver-unabhaengig, wie bei den flachen
Tests - siehe [[test_fixed_joint_flat]]-Pendant `test_fixed_joint_flat.py`):
    BoxB_erwartet = BoxC.Placement * outerPlc1 * outerPlc2.inverse()
    BoxA_erwartet = BoxB_erwartet  * innerPlc2 * innerPlc1.inverse()
(zweite Zeile ist die nach BoxA aufgeloeste Form von "BoxB = BoxA * innerPlc1 * innerPlc2.inv()",
 dem bekannten Fixed-Joint-Muster aus der flachen Testmatrix - gilt UNABHAENGIG davon, ob BoxA
 in Sub zusaetzlich geerdet ist, siehe oben.)

Live per Xvfb-Probe verifiziert (2026-09-08, mit UND ohne Sub-eigene Erdung von BoxA getestet):
der Spiegel (`SubLink.Group`-Kind gleichen Namens, `App::Link` auf das Original in Sub) traegt
nach einem vollen GrandTop-Solve in BEIDEN Faellen (patched wie vanilla) exakt das analytisch
erwartete Placement - auf die reine Positionsberechnung hat die Erdung also keinen Einfluss,
beide Versionen rechnen hier richtig.

**Fixture-Methodik (Nutzerauftrag 2026-09-08):** die Ausgangs-.FCStd-Dateien (Sub+GrandTop)
werden GENAU EINMAL gebaut (per `build_fixture_<testfall>.py`) und unter `fixtures/<testfall>/`
GIT-GETRACKT abgelegt - NICHT bei jedem Testlauf per Skript unter der jeweils getesteten
FreeCAD-Version neu erzeugt. Grund: sonst vermischen sich zwei unabhaengige Fragen - "baut das
Python-Skript unter Version X ueberhaupt dieselbe Dokumentstruktur wie unter Version Y" und
"loest Version X denselben Ausgangszustand anders als Version Y" - nur die zweite Frage soll
diese Testmatrix beantworten. Jeder Testlauf (`run-test.sh`) kopiert die feste Fixture-Datei in
sein eigenes `fcstd-output/<testfall>/<installationsname>/`-Unterverzeichnis (siehe
`copy_fixture_to_output()`) - MIT den ORIGINALEN Dateinamen "sub.FCStd"/"grand.FCStd", NICHT
umbenannt - und oeffnet NUR diese Kopie.

**Wichtige Lektion zur Kopie-Benennung (live gefunden, 2026-09-08):** `grand.FCStd` verweist
intern per BLOSSEM Dateinamen ("sub.FCStd") auf Sub, relativ zum eigenen Verzeichnis. Eine
Kopie mit ANDEREM Dateinamen (z.B. "freecad-sandbox_install__sub.FCStd", um patched/vanilla-
Laeufe im selben Ordner zu unterscheiden) macht daraus einen GEBROCHENEN Link ("Link broken!
Object: BoxB File: sub.FCStd") - der innere Joint faellt dabei UNBEMERKT (keine Exception, nur
ein falsches Ergebnis) aus dem Gesamtsystem. Deshalb: pro Installation ein eigener
UNTERORDNER mit den unveraenderten Original-Dateinamen, niemals eine Umbenennung der Dateien
selbst.

**Ergebnis nach Behebung dieses Bugs (wichtig fuer die Einordnung frueherer Befunde dieser
Session):** eine fruehere Version dieses Testfalls hatte - VOR der festen Fixture und VOR dem
Link-Benennungs-Fix - einen scheinbaren Vanilla-Bug gefunden (Sub-Dokument bleibt beim
Neuladen ~10-13mm unsynchron zu seinem Spiegel). Mit korrekter Fixture UND intaktem Link
verschwindet dieser Unterschied vollstaendig: BEIDE Staende (patched und vanilla) sind exakt
synchron, sowohl direkt nach dem Solve als auch nach einem reinen `recompute()` (ohne
erneuten expliziten `solve()`-Aufruf) nach dem Neuladen - genau das Szenario, in dem der
scheinbare Bug urspruenglich auftauchte. Der fruehere Befund war ein Test-Artefakt (entweder
durch die pro-Version-neu-gebaute Ausgangsdatei oder durch den gebrochenen Link, oder beides),
KEIN echter Unterschied zwischen den FreeCAD-Staenden fuer diesen einfachsten
Verschachtelungsfall. Siehe README.md "Methodik-Korrektur" fuer die volle Chronologie.
"""
import os
import shutil

import FreeCAD as App

import joint_test_utils as jtu


def fixture_paths(test_dir):
    """Pfade der festen, git-getrackten Ausgangsdateien fuer einen Testfall:
    <test_dir>/fixtures/<testname>_sub.FCStd + <testname>_grand.FCStd, wobei test_dir das
    EIGENE Verzeichnis des jeweiligen Testfall-Skripts ist (uebergeben als
    os.path.dirname(os.path.abspath(__file__)) - seit der Ordner-Umstrukturierung 2026-09-09
    liegt jeder Testfall in seinem eigenen Unterordner). Der Testname im Dateinamen
    (Nutzerauftrag 2026-09-09) macht die Dateien auch ausserhalb ihres Ordnerkontexts
    unterscheidbar - WICHTIG: das ist der einzig sichere Weg, diese Dateien umzubenennen, da
    grand.FCStd intern per BLOSSEM Dateinamen auf sub.FCStd verweist (siehe Moduldocstring
    "Wichtige Lektion") - eine nachtraegliche Umbenennung bereits gebauter Dateien wuerde diesen
    Link OHNE Fehlermeldung brechen. Der Name muss deshalb schon beim Bauen (build_fixture.py)
    feststehen, nicht erst hinterher per einfachem Umbenennen. Von build_fixture.py beschrieben,
    von den eigentlichen Testlaeufen nur GELESEN (nie direkt veraendert - siehe
    copy_fixture_to_output())."""
    fixtures_dir = os.path.join(test_dir, "fixtures")
    test_name = os.path.basename(test_dir)
    return (
        os.path.join(fixtures_dir, f"{test_name}_sub.FCStd"),
        os.path.join(fixtures_dir, f"{test_name}_grand.FCStd"),
    )


def copy_fixture_to_output(
    fixture_sub_path, fixture_grand_path, out_sub_path, out_grand_path,
    box_labels=("BoxA", "BoxB", "BoxC")
):
    """Kopiert die feste Fixture (sub.FCStd + grand.FCStd) in die per-Installation eigenen
    Ausgabepfade (fcstd-output/<testfall>/<installationsname>__{sub,grand}.FCStd) - der
    eigentliche Testlauf arbeitet NUR auf dieser Kopie, damit die Fixture selbst unangetastet
    bleibt und jeder Installations-Lauf sein eigenes Endergebnis behaelt (analog zu den
    flachen Tests, die ebenfalls je Installation eine eigene Ausgabedatei behalten).

    FCPROJECT-PATCH (Nutzerauftrag 2026-09-15): box_labels-Parameter ergaenzt - die externen
    Koerper-Dateien (BoxA/BoxB/BoxC, siehe joint_test_utils.py::ensure_box_bodies_in()) liegen
    im selben Verzeichnis wie fixture_sub_path/fixture_grand_path und muessen bei JEDEM
    Kopiervorgang mit umziehen (gleicher Dateiname, gleiches Verzeichnis wie die Assembly-
    Dokumente - sonst bricht der relative XLink, siehe "Wichtige Lektion zur
    Kopie-Benennung" oben im Moduldocstring)."""
    for p in (out_sub_path, out_grand_path):
        os.makedirs(os.path.dirname(p), exist_ok=True)
    shutil.copy2(fixture_sub_path, out_sub_path)
    shutil.copy2(fixture_grand_path, out_grand_path)
    fixtures_dir = os.path.dirname(fixture_sub_path)
    out_dir = os.path.dirname(out_grand_path)
    for label in box_labels:
        shutil.copy2(
            os.path.join(fixtures_dir, f"{label}.FCStd"), os.path.join(out_dir, f"{label}.FCStd")
        )


def new_sub_assembly_doc(doc_name, save_path, box_a_name="BoxA", box_b_name="BoxB"):
    """Baut die Sub-Baugruppe: zwei Koerper (seit Nutzerauftrag 2026-09-15 externe App::Link-
    Referenzen statt nativer Part::Box, siehe joint_test_utils.py::new_flat_two_box_assembly())
    in einem EIGENEN Dokument, BoxA GEERDET (der interne Referenz-/Befestigungspunkt dieser
    Unterbaugruppe - siehe Moduldocstring fuer die Begruendung, warum das kein Widerspruch zum
    aeusseren Joint in GrandTop ist). Reine Delegation an
    joint_test_utils.new_flat_two_box_assembly() (identisches Muster wie die flachen Tests),
    unter eigenem Namen hier fuer die Lesbarkeit des Verschachtelungs-Codes. 'save_path' wird
    direkt durchgereicht (das Dokument muss VOR dem Setzen der Box-XLinks bereits gespeichert
    sein). Der eigentliche innere Joint wird vom Aufrufer per jtu.make_joint() ergaenzt."""
    doc, assembly, boxA, boxB = jtu.new_flat_two_box_assembly(doc_name, save_path)
    if box_a_name != "BoxA":
        boxA.Label = box_a_name
    if box_b_name != "BoxB":
        boxB.Label = box_b_name
    return doc, assembly, boxA, boxB


def allow_duplicate_labels():
    """Erlaubt identische Labels mehrerer Objekte IM SELBEN Dokument (FreeCAD-Voreinstellung
    "DuplicateLabels" unter BaseApp/Preferences/Document, per Default False - erzwingt sonst
    automatisch eindeutige Labels, z.B. "Joint" -> "Joint001"). Noetig fuer verschachtelte
    Baugruppen: AssemblyLink::updateContents() spiegelt Sub's Joint/JointGroup als ECHTE Kopien
    INS GrandTop-Dokument (siehe docs/ARCHITECTURE.md Abschnitt 2.3, "Kopier-Pipeline") - die
    tragen automatisch dieselben Labels ("Joint"/"Joints") wie die Originale in Sub, was mit
    GrandTops EIGENEN, gleichnamigen Joint/JointGroup-Objekten kollidiert (Namen bleiben trotzdem
    eindeutig, siehe App-Objekt.Name - nur das Label kollidiert). Nutzerauftrag 2026-09-08:
    programmatisch statt ueber die GUI-Voreinstellung setzen, damit der Testfall unabhaengig vom
    Zustand des jeweils genutzten FreeCAD-Profils reproduzierbar bleibt (der Parametername
    "UniqueLabel" war ein falscher erster Versuch - der tatsaechliche Name, live aus dem
    eigenen Sandbox-Profil (freecad-sandbox-profile/user.cfg) verifiziert, ist
    "DuplicateLabels")."""
    App.ParamGet("User parameter:BaseApp/Preferences/Document").SetBool("DuplicateLabels", True)


def new_grand_assembly_with_sublink(
    doc_name, sub_assembly, sub_doc_save_path, grand_doc_save_path, box_name="BoxC", link_name="SubLink"
):
    """Baut die GrandTop-Baugruppe: ein Part::Box (per Default "BoxC", geerdet) + eine flexible
    Assembly::AssemblyLink (per Default "SubLink") auf sub_assembly. Speichert BEIDE Dokumente
    vorab - ein dokumentuebergreifender Link (LinkedObject) verlangt ein bereits gespeichertes
    Zieldokument (siehe CommandInsertLink.py's eigener Save-Check: "Owner document not
    saved" sonst live reproduziert).

    box_name/link_name (Nutzerauftrag "doppelt verschachtelte Tests", 2026-09-09): bei EINER
    Verschachtelungsebene sind die Defaults immer eindeutig - bei ZWEI Ebenen (Sub -> Mid ->
    GrandTop, diese Funktion zweimal aufgerufen) wuerden Mid's EIGENE Objekte beim Einbetten in
    GrandTop sonst mit GLEICHNAMIGEN Objekten kollidieren. Seit dem Mehrfachinstanz-Fix
    (Nutzerauftrag 2026-09-14, siehe docs/ARCHITECTURE.md Abschnitt 4.1) ist das kein Problem
    mehr, das umgangen werden muss - box_name/link_name landen nur noch im LABEL (Lesbarkeit,
    Lookup ueber get_mirror()/get_by_label()), waehrend der interne NAME jetzt IMMER FreeCAD
    selbst ueberlaesst: boxC bekommt den generischen Part::Box-Namen "Box" (kollidiert bewusst
    mit BoxC einer aeusseren Ebene, FreeCAD haengt selbst einen Zaehler an), und sublink bekommt
    exakt den Namen, den auch CommandInsertLink.py::onItemClicked() fuer den Nutzer anfragen
    wuerde (das Label der verlinkten Baugruppe, typischerweise "Assembly" - kollidiert bewusst
    mit GrandTops EIGENEM Top-Level-AssemblyObject und wird von FreeCAD selbst umbenannt, z.B.
    zu "Assembly001"). Genau diese vom Nutzer beobachtete, per Hand nie gewaehlte Namensvergabe
    ist Teil dessen, was Bug A/B abdecken sollten - ein test-eigener, "sauberer" Name haette das
    verdeckt.

    FCPROJECT-PATCH (Nutzerauftrag 2026-09-15, "jede Koerper... ein eigenes Datei... damit
    xlinks spielen mit"): boxC ist seitdem KEIN natives Part::Box mehr, sondern ein App::Link
    auf eine externe Koerper-Datei (BoxC.FCStd bzw. BoxD.FCStd fuer die Mid-Ebene der doppelt
    verschachtelten Tests, siehe box_name) - ensure_box_bodies_in()/link_external_box() aus
    joint_test_utils.py, identisches Muster wie new_flat_two_box_assembly(). Grounding jetzt
    ueber ground_object() statt eines rohen setPropertyStatus("Placement", ...) - ein
    App::Link braucht ZUSAETZLICH LinkPlacement.ReadOnly, siehe dortige Begruendung."""
    allow_duplicate_labels()
    sub_assembly.Document.saveAs(sub_doc_save_path)

    grand_doc = App.newDocument(doc_name)
    # Frueh speichern - noetig fuer JEDEN XLink unten (sowohl boxC als auch sublink weiter
    # unten verlangen "Owner document not saved" andernfalls).
    grand_doc.saveAs(grand_doc_save_path)

    box_paths = jtu.ensure_box_bodies_in(os.path.dirname(grand_doc_save_path), [box_name])
    boxC = jtu.link_external_box(grand_doc, box_name, box_paths[box_name])
    grand_doc.recompute()

    grand_asm = grand_doc.addObject("Assembly::AssemblyObject", "Assembly")
    grand_asm.addObject(boxC)
    grand_doc.recompute()
    jtu.ground_object(boxC)

    # FCPROJECT-PATCH (Mehrfachinstanz-Fix, Nutzerauftrag 2026-09-14): exakt wie
    # CommandInsertLink.py::onItemClicked() - Name-Wunsch ist das Label der verlinkten
    # Baugruppe, nicht ein test-eigener Bezeichner. link_name geht nur noch ins Label.
    sublink = grand_asm.newObject("Assembly::AssemblyLink", sub_assembly.Label)
    sublink.LinkedObject = sub_assembly
    sublink.Rigid = False
    sublink.Label = link_name
    grand_asm.addObject(sublink)
    grand_doc.recompute()

    return grand_doc, grand_asm, boxC, sublink


def get_mirror(sublink, label):
    """Findet das Spiegel-Kind (App::Link, von AssemblyLink::updateContents() automatisch
    angelegt) mit gegebenem LABEL in sublink.Group - z.B. get_mirror(sublink, "BoxB").

    FCPROJECT-PATCH (Mehrfachinstanz-Fix, Nutzerauftrag 2026-09-14): vormals namensbasiert
    (child.Name == name) - seit new_flat_two_box_assembly()/new_grand_assembly_with_sublink()
    den internen Namen nicht mehr selbst vergeben, sondern FreeCAD ueberlassen, ist der Name
    keine stabile Testreferenz mehr. Das LABEL wird von AssemblyLink::synchronizeComponents()
    vom Quellobjekt uebernommen (newLink->Label = obj->Label) - bleibt deshalb "BoxA"/"BoxB",
    unabhaengig davon, wie der interne Name lautet."""
    for child in sublink.Group:
        if child.Label == label:
            return child
    return None


def output_paths(test_name):
    """Ausgabepfade fuer einen Testlauf: ein eigener Unterordner PRO INSTALLATION mit den
    ORIGINALEN Dateinamen "sub.FCStd"/"grand.FCStd" (siehe Moduldocstring, "Wichtige Lektion
    zur Kopie-Benennung" - eine Umbenennung der Dateien selbst wuerde den internen
    Dokument-Link brechen). run-test.sh setzt KINEMATIC_TEST_OUTPUT_FCSTD auf
    fcstd-output/<test_name>/<installationsname>.FCStd - der Installationsname wird hier als
    UNTERORDNER verwendet, nicht als Dateipraefix."""
    base = os.environ.get("KINEMATIC_TEST_OUTPUT_FCSTD")
    if base:
        install_dir = os.path.splitext(base)[0]
    else:
        install_dir = os.path.join(
            os.path.dirname(os.path.abspath(__file__)), "fcstd-output", test_name, "default"
        )
    os.makedirs(install_dir, exist_ok=True)
    return os.path.join(install_dir, "sub.FCStd"), os.path.join(install_dir, "grand.FCStd")


def build_fixture_generic(test_name, inner_type_index, inner_plc1, inner_plc2, outer_plc1, outer_plc2, wrong_placement):
    """Baut die feste Ausgangsdatei EINMALIG fuer einen Stufe-2-Testfall mit BELIEBIGEM innerem
    Jointtyp: Sub (BoxA geerdet -[innerer Joint vom gegebenen Typ]- BoxB), GrandTop (BoxC
    geerdet -[aeusserer FIXED-Joint]- Spiegel von BoxB). Der aeussere Joint ist IMMER Fixed
    (definiert die Verankerung der gesamten Unterbaugruppe eindeutig, analog zu BoxAs Erdung in
    der flachen Testmatrix) - variiert wird NUR der innere Jointtyp, das ist die eigentliche
    Testdimension dieser Stufe (analog zu Stufe 1s Testmatrix je Jointtyp).

    wrong_placement: die absichtlich falsche Startposition fuer den Spiegel von BoxB (siehe
    test_fixed_joint_flat.py fuer die Begruendung) - je Jointtyp ggf. unterschiedlich (siehe
    z.B. test_ball_joint_flat.py's Konvergenzradius-Einschraenkung)."""
    sub_path, grand_path = fixture_paths(test_name)
    os.makedirs(os.path.dirname(sub_path), exist_ok=True)

    sub_doc, sub_asm, subA, subB = new_sub_assembly_doc(f"{test_name}Sub", sub_path)
    jtu.make_joint(sub_asm, inner_type_index, subA, subB, inner_plc1, inner_plc2)
    sub_doc.recompute()

    grand_doc, grand_asm, boxC, sublink = new_grand_assembly_with_sublink(
        f"{test_name}Grand", sub_asm, sub_path, grand_path
    )
    mirror_boxA = get_mirror(sublink, subA.Label)
    mirror_boxB = get_mirror(sublink, subB.Label)
    assert mirror_boxA is not None, "Spiegel von BoxA nicht in SubLink.Group gefunden"
    assert mirror_boxB is not None, "Spiegel von BoxB nicht in SubLink.Group gefunden"

    outer_joint = jtu.make_joint(grand_asm, 0, boxC, mirror_boxB, outer_plc1, outer_plc2)  # Fixed
    # Label-Kollision siehe allow_duplicate_labels() - beide sollen "Joint"/"Joints" heissen
    # duerfen (Nutzerauftrag 2026-09-08).
    outer_joint.Label = "Joint"
    for child in grand_asm.OutList:
        if child.TypeId == "Assembly::JointGroup":
            child.Label = "Joints"
    grand_doc.recompute()

    # Absichtlich falsche Position NACH dem (automatischen Presolve-)Aufbau setzen - siehe
    # test_fixed_nested_flex.py. WIRD NICHT MEHR KORRIGIERT, bevor gespeichert wird.
    mirror_boxB.Placement = wrong_placement

    grand_doc.save()
    sub_doc.save()

    print(f"Fixture geschrieben: {sub_path}")
    print(f"Fixture geschrieben: {grand_path}")
    return sub_path, grand_path


def load_fixture_and_solve_generic(test_name):
    """Kopiert die feste Fixture in den eigenen Ausgabepfad, oeffnet die Kopie und loest sie -
    das ist der einzige Teil, der sich zwischen einem patched- und einem vanilla-Lauf
    unterscheiden kann. Gibt (grand_doc, grand_asm, boxC, sublink, mirror_boxA, mirror_boxB,
    out_sub_path, out_grand_path) zurueck."""
    fixture_sub, fixture_grand = fixture_paths(test_name)
    out_sub, out_grand = output_paths(test_name)
    copy_fixture_to_output(fixture_sub, fixture_grand, out_sub, out_grand)

    grand_doc = App.openDocument(out_grand)
    grand_asm = grand_doc.getObject("Assembly")
    boxC = jtu.get_by_label(grand_doc, "BoxC")
    sublink = jtu.get_by_label(grand_doc, "SubLink")

    grand_doc.recompute()
    grand_asm.solve(False)
    grand_doc.recompute()

    mirror_boxA = get_mirror(sublink, "BoxA")
    mirror_boxB = get_mirror(sublink, "BoxB")
    assert mirror_boxA is not None, "Spiegel von BoxA nicht in SubLink.Group gefunden"
    assert mirror_boxB is not None, "Spiegel von BoxB nicht in SubLink.Group gefunden"

    return grand_doc, grand_asm, boxC, sublink, mirror_boxA, mirror_boxB, out_sub, out_grand


def check_outer_fixed(label, boxC_plc, mirror_boxB_plc, outer_plc1, outer_plc2, tol_pos=1e-6, tol_rot=1e-4):
    """Der aeussere Joint ist IMMER Fixed (0 DOF) - dessen Ziel-Placement ist deshalb, anders
    als beim inneren Joint (der je nach Typ Freiheitsgrade hat), vollstaendig analytisch
    vorhersagbar. Exakt dasselbe Muster wie test_fixed_joint_flat.py."""
    expected = boxC_plc.multiply(outer_plc1).multiply(outer_plc2.inverse())
    pos_diff = (expected.Base - mirror_boxB_plc.Base).Length
    rot_diff_deg = abs(
        expected.Rotation.multiply(mirror_boxB_plc.Rotation.inverted()).Angle
    ) * 180.0 / 3.141592653589793

    print(f"--- {label}: aeusserer Fixed-Joint (BoxC-BoxB) ---")
    print("BoxB (analytisch erwartet):", expected)
    print("BoxB (tatsaechlich):       ", mirror_boxB_plc)
    print(f"Positions-/Rotations-Differenz: {pos_diff:.9f} mm / {rot_diff_deg:.9f} deg")

    return pos_diff <= tol_pos and rot_diff_deg <= tol_rot


def check_inner_joint(label, check_fn, mirror_boxA_plc, mirror_boxB_plc, inner_plc1, inner_plc2):
    """Prueft die Zwangsbedingung des inneren Joints (Revolute/Cylindrical/Slider/Ball,
    beliebiger Typ) ueber die Spiegel-Objekte in GrandTop - dieselbe Methodik wie bei den
    flachen Tests (siehe joint_test_utils.py's check_revolute/cylindrical/slider/ball), nur
    dass die JCS-Weltplacements hier ueber die Spiegel-Objekte statt direkt ueber BoxA/BoxB
    berechnet werden. check_fn ist eine der jtu.check_*-Funktionen."""
    # mirror_boxA_plc/mirror_boxB_plc sind hier Placement-Objekte (nicht die App::Link-Objekte
    # selbst) - jtu.jcs_world() erwartet ein Objekt mit .Placement, deshalb rechnen wir hier
    # direkt mit der Placement-Algebra statt jtu.jcs_world() wiederzuverwenden.
    jcs1 = mirror_boxA_plc.multiply(inner_plc1)
    jcs2 = mirror_boxB_plc.multiply(inner_plc2)
    rel = jtu.report(f"{label}: innerer Joint (Sub, BoxA-BoxB)", jcs1, jcs2)
    return check_fn(rel)


def check_sync(label, mirror_boxA, mirror_boxB):
    """Prueft, ob Sub's EIGENES Dokumentobjekt mit seinem Spiegel in GrandTop uebereinstimmt
    (`syncLocalMirrorPlacement()`, docs/ARCHITECTURE.md Abschnitt 3, Bug 8) - siehe
    Moduldocstring fuer die Einordnung (frueher vermuteter Vanilla-Bug erwies sich als
    Testartefakt, siehe [[reference-nested-fixture-methodology]]).

    FCPROJECT-PATCH (Mehrfachinstanz-Fix, Nutzerauftrag 2026-09-14): das echte Quellobjekt wird
    jetzt direkt ueber LinkedObject genommen statt per Namens-Nachschlagen im Sub-Dokument -
    seit der interne Name nicht mehr per Hand auf "BoxA"/"BoxB" gesetzt wird, ist er zwischen
    Spiegel und Original ohnehin nicht mehr identisch (jedes Dokument vergibt unabhaengig eigene
    Namen), ein Namens-Lookup ueber Dokumentgrenzen war also nie eine stabile Referenz."""
    subA = mirror_boxA.LinkedObject
    subB = mirror_boxB.LinkedObject
    diff_A = (subA.Placement.Base - mirror_boxA.Placement.Base).Length
    diff_B = (subB.Placement.Base - mirror_boxB.Placement.Base).Length
    print(f"--- {label}: Sub<->Spiegel-Sync ---")
    print(f"Sync-Differenz BoxA: {diff_A:.9f} mm")
    print(f"Sync-Differenz BoxB: {diff_B:.9f} mm")
    return diff_A <= 1e-6 and diff_B <= 1e-6


def save_close_reopen_recompute_nested(grand_doc, grand_doc_path, box_c_name="BoxC", link_name="SubLink"):
    """Wie joint_test_utils.save_close_reopen_recompute(), aber fuer den verschachtelten Fall:
    schliesst GrandTop (das Sub-Dokument haengt als externe Referenz daran und wird von FreeCAD
    beim Schliessen/Neuladen automatisch mitverwaltet) und oeffnet es neu. Gibt
    (grand_doc2, boxC2, sublink2, exception_oder_None) zurueck.

    link_name (Nutzerauftrag "doppelt verschachtelte Tests", 2026-09-09): bei zwei
    Verschachtelungsebenen heisst GrandTops eigener Link nicht mehr zwangslaeufig "SubLink"
    (siehe new_grand_assembly_with_sublink()'s box_name/link_name-Parameter, dort wird z.B.
    "MidLink" vergeben, um Namenskollisionen beim Spiegeln zu vermeiden) - Default bleibt
    "SubLink" fuer Abwaertskompatibilitaet mit den 5 bestehenden Stufe-2-Tests."""
    grand_doc.save()
    docname = grand_doc.Name
    App.closeDocument(docname)

    grand_doc2 = App.openDocument(grand_doc_path)
    boxC2 = jtu.get_by_label(grand_doc2, box_c_name)
    grand_asm2 = grand_doc2.getObject("Assembly")
    sublink2 = jtu.get_by_label(grand_doc2, link_name)

    exc = None
    try:
        grand_doc2.recompute()
    except Exception as e:  # noqa: BLE001 - bewusst breit, das IST der Test
        exc = e

    return grand_doc2, boxC2, sublink2, exc
