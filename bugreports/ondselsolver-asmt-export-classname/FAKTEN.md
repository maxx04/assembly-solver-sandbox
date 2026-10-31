# Faktenblatt: ASMT-Export schreibt unter GCC/Clang kaputte Gelenk-Typnamen („dJointE“)

Stand 2026-10-31. Unterlage für den Nutzer (FreeCAD-Regel: Issue-/PR-Texte schreibt ein Mensch).
Kein fertiger Text. Betrifft OndselSolver (FreeCAD-Submodul `src/3rdParty/OndselSolver`, Stand
`4be80ee`), sichtbar über FreeCADs `AssemblyObject.exportAsASMT()`.

## 1. Befund (Vanilla nachgewiesen)

Ein ASMT-Export unter Linux enthält statt der Gelenk-Typen Bruchstücke:

| Gelenk | erwartet in der .asmt | tatsächlich (GCC) |
|---|---|---|
| Fixed (auch Erdung) | `FixedJoint` | `dJointE` |
| Revolute | `RevoluteJoint` | `luteJointE` |
| ParallelAxes | `ParallelAxesJoint` | `llelAxesJointE` |

Repro `asmt_export_check.py` (flache Testdateien der Sandbox, Export per `exportAsASMT`),
FreeCAD Weekly 2026.10.21 und Sandbox identisch:

    EXPORT test_fixed_joint_flat: Gelenk-Typzeilen ['dJointE', 'dJointE', 'dJointE']
    EXPORT test_revolute_joint_flat: Gelenk-Typzeilen ['dJointE', 'dJointE', 'luteJointE']

## 2. Ursache

`ASMTItem::classname()` (und gleich `Item::classname()`) schneidet den Typnamen mit fester
Zeichenzahl aus `typeid(*this).name()`:

```cpp
std::string str = typeid(*this).name();
auto answer = str.substr(11, str.size() - 11);   // "class MbD::" weg
```

`ASMTJoint::storeOnLevel()` schneidet danach noch 4 Zeichen ab („ASMT“):

```cpp
auto jointType = classname();
jointType = jointType.substr(4, jointType.size() - 4);
```

Das passt nur zum MSVC-Format `class MbD::ASMTFixedJoint`. GCC und Clang liefern den
verschlüsselten Namen `N3MbD14ASMTFixedJointE`: nach 11 Zeichen bleibt `FixedJointE`, nach
weiteren 4 `dJointE`. Die Länge des Präfixes hängt zudem von der Stellenzahl der Namenslänge ab
(`14` vs. `22`), deshalb sind die Bruchstücke je Gelenk verschieden.

Dieselbe Technik steckt in `ASMTJoint::storeOnTimeSeries()` (`substr(15)`) und in den
Log-Meldungen über `Item::classname()` (z.B. „has the following constraint(s) removed“).

## 3. Folgen

- `ASMTAssembly::readJoints()` vergleicht die Typzeile mit festen Namen (`"\t\t\tFixedJoint"` …)
  und wirft bei allem anderen `SimulationStoppingError("To be implemented.")`. **Eine unter
  Linux/macOS exportierte .asmt lässt sich mit OndselSolver nicht wieder einlesen.** Damit taugt
  der Export dort nicht zum Nachstellen eines Solver-Problems (sein Hauptzweck).
- Wer die Datei liest (Mensch oder Skript), sieht den Gelenktyp nicht.
- Die Solver-Meldungen nennen verstümmelte Klassennamen.

## 4. Lösungsvorschlag (nicht umgesetzt)

Typnamen nicht aus `typeid` ableiten, sondern je Klasse fest angeben, z.B. eine virtuelle
Methode `std::string asmtTypeName() const` mit `return "FixedJoint";` in jeder
ASMT-Gelenkklasse (das Gegenstück zu den festen Namen in `readJoints()`). Alternativ unter
GCC/Clang `abi::__cxa_demangle()` und dann den Namensraum `MbD::` abtrennen; das bliebe aber
compilerabhängig.

## Dateien

- `asmt_export_check.py` – Repro (über `standalone-check/kinematic-tests/common/run-test.sh`).
