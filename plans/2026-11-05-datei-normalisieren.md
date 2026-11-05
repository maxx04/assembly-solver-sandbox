# Plan: Befehl „Für FreeCAD normalisieren“ (Assembly → Sandbox)

Stand 2026-11-05. Nutzerauftrag: Befehl im Assembly-Arbeitsbereich, der eine Datei samt Projekt
und Verknüpfungen mit normalem FreeCAD kompatibel macht. Anlass: AppImage zum Weitergeben —
Empfänger sollen Sandbox-Dateien gefahrlos an normale FreeCAD-Nutzer geben können.

## 1. Befund: Was unterscheidet eine Sandbox-Datei?

Grundsatz (feste Bedingung, `docs/CONSTRAINTS.md`): Die Sandbox führt keine neuen gespeicherten
Eigenschaften ein. Jede Sandbox-Datei öffnet in normalem FreeCAD und umgekehrt. Unterschiede gibt
es nur in **Struktur, die FreeCAD selbst nachbaut**, und in der **Deutung** beim Lösen.

### 1a. Struktur — regelt sich selbst (gemessen 2026-11-05)

| Unterschied | Sandbox → Weekly | Weekly → Sandbox |
|---|---|---|
| Gelenkkopien in flexibler Unterbaugruppe (`Joints001`/`Joint001` im AssemblyLink; Sandbox: „Adressieren statt Kopieren“) | Weekly legt sie **beim Laden** neu an, Lagen identisch | Sandbox entfernt sie beim Laden, Lagen identisch |
| `Rigid` des Spiegels (Regel C1 übernimmt ihn vom Original) | normale Eigenschaft, gleiche Bedeutung | — |
| Dateikopf `ProgramVersion` mit „-sandbox“ | nur Text | — |
| XLink-Pfade relativ (Sandbox) / absolut (Weekly seit 24ea8589aa) | beide lesbar | beide lesbar |

Gemessen mit `test_revolute_nested_flex`: Sandbox-Datei in der Weekly geöffnet, neu berechnet,
gelöst und gespeichert sowie umgekehrt.

**Schritt 0 erledigt (2026-11-05)**, alle 20 verschachtelten Testdateien, drei Richtungen:

| Richtung | Ladefehler | Gelenkkopien | Bewegung beim Lösen |
|---|---|---|---|
| Sandbox → FreeCAD 1.1.4 (stabil) | keine | beim Laden **nie**, Neuberechnen nur teilweise | `nested_rigid_group_flex`: BoxB 65 mm / 20° |
| Sandbox → Weekly 2026-11-04 | keine | beim Laden vollständig | `nested_rigid_group_flex` (wie oben), `nested_new_part_in_displaced_instance`: BoxA 17 mm / 73° |
| Weekly → Sandbox | keine | Sandbox entfernt sie | keine |

Folgerungen:
- In 1.1.4 sind flexible Unterbaugruppen aus Sandbox-Dateien innen ohne Gelenk. Die Teile bleiben
  liegen, fallen aber beim Ziehen auseinander. → Normalisieren lässt ein normales FreeCAD die
  Kopien anlegen (`reference-finish`: Unterbaugruppen anstoßen, neu berechnen, speichern).
- Beim Neuberechnen löst normales FreeCAD teilweise schon und verschiebt Teile. `reference-finish`
  setzt deshalb die Sandbox-Lagen vor dem Speichern zurück; die Abweichung meldet die Gegenprobe.
- Ob eine Abweichung sichtbar wird, hängt von der FreeCAD-Version ab (1.1.4 ohne Gelenkkopien
  zeigt den zweiten Fall nicht). → Die Gegenprobe gehört zur Version des Empfängers.
- Ohne Oberfläche (`freecadcmd`) funktionieren Öffnen und Lösen auch in 1.1.4 → kein Xvfb nötig.

### 1b. Deutung — normales FreeCAD löst anders (nicht reparierbar, nur meldbar)

| Sandbox-Funktion | Verhalten in normalem FreeCAD |
|---|---|
| Punkt-auf-Kurve (Abstand 0, Punkt–Kurve/Kurvenkette) | anderes Gelenk (Punkt–Linie bzw. nicht unterstützt) → Teil springt |
| Mehrfach eingefügte gleiche Unterbaugruppe (Instanz-Identität, Bug C) | Gelenke an der 2. Instanz gehen verloren |
| Flexible Unterbaugruppe an beliebigem Anschlusspunkt, Erdung als Kontext-Eigenschaft | Umklappen/Wegfliegen möglich |
| Eindeutige Gelenke (Fixed/Slider/Revolute/Cylindrical, C8 bündig) | 180°-Mehrdeutigkeit; meist stabil, weil von gespeicherten Lagen aus gelöst wird |
| Gelenkgrenzen beim Lösen und in der Simulation | werden ignoriert |
| Redundanz mit Toleranz (C7) | Fehlermeldung „redundant“ statt Warnung |

Diese Punkte kann keine Datei-Änderung beheben, weil normales FreeCAD die Logik nicht hat. Der
Befehl kann sie nur **finden und melden**.

## 2. Was der Befehl tut

### Stufe A — Prüfen (schreibt nichts)
1. **Projekt einsammeln:** aktives Dokument plus rekursiv alle verknüpften Dateien
   (XLinks/AssemblyLinks). Dateien außerhalb des Projektordners werden als „extern“ markiert
   und nicht verändert.
2. **Statische Prüfung je Datei** (Regeln aus 1b): betroffene Gelenke mit Label, Typ und Grund.
3. **Gegenprobe mit normalem FreeCAD (optional, wenn vorhanden):**
   - normales FreeCAD (Pfad in den Einstellungen, Standard: Weekly-AppImage) startet ohne
     Oberfläche;
   - es öffnet eine Kopie jeder Datei, löst sie und vergleicht die Lagen mit dem Sandbox-Stand.
   - Ergebnis: „Teil X verschiebt sich um 12 mm / dreht 180°“. Das ist die einzige verlässliche
     Antwort auf die Frage „sieht es beim Empfänger gleich aus?“.
4. **Bericht** in einem Dialog: grün = identisch, gelb = Hinweis, rot = verhält sich anders.
   Zusätzlich als Text kopierbar.

### Stufe B — Normalisieren (schreibt in eine Kopie)
Ziel ist ein **neuer Ordner** (Standard: `<Projekt>_FreeCAD/`). Die Originale bleiben unberührt.
1. alle Projektdateien mit gleicher Ordnerstruktur kopieren, damit relative Verknüpfungen gültig
   bleiben;
2. je Datei in der Sandbox:
   - öffnen, lösen, Lagen sichern;
   - Gelenkkopien für flexible Unterbaugruppen so anlegen, wie normales FreeCAD sie anlegt
     (nur falls Schritt 0 zeigt, dass die stabile Version sie nicht selbst nachbaut — sonst
     entfällt das);
   - Kennung „-sandbox“ aus dem Dateikopf nehmen;
   - Verknüpfungspfade relativ speichern;
3. danach automatisch die Gegenprobe (A3) auf den Kopien → Bericht „nach Normalisierung“.

**Bewusst nicht:**
- keine Änderung der Originale;
- kein stilles Umbauen von Gelenken, die normales FreeCAD nicht kann (z. B. Punkt-auf-Kurve).
  Diese werden gemeldet, der Nutzer entscheidet.
- **Später möglich:** Option „solche Gelenke in der Kopie deaktivieren“ (Suppressed), damit
  normales FreeCAD nichts verbiegt.

### Einordnung in der Oberfläche
- Untermenü **Assembly → Sandbox** mit „Für FreeCAD prüfen…“, „Für FreeCAD normalisieren…“ und
  dem vorhandenen IdentityGraph-Befehl.
- Python-Befehl, Modul `CommandSandboxNormalize.py`. Keine gespeicherte Eigenschaft, kein
  C++-Eingriff, also kein Risiko für das Dateiformat.

## 3. Schritte

| # | Schritt | Ergebnis |
|---|---|---|
| 0 | Letzte stabile FreeCAD-AppImage besorgen; Messung 1a mit ihr wiederholen (alle verschachtelten Fixtures, beide Richtungen) | weiß, ob Gelenkkopien nötig sind |
| 1 | Projekt einsammeln + statische Regeln + Berichtsdialog (Stufe A ohne Gegenprobe) | Befehl „prüfen“ nutzbar |
| 2 | Gegenprobe: Unterprozess mit normalem FreeCAD, Lagen-Vergleich | verlässliche Aussage je Teil |
| 3 | Stufe B: Kopie in neuen Ordner, Kopf säubern, ggf. Gelenkkopien | Befehl „normalisieren“ |
| 4 | Kinematik-Test `test_normalize_project` (verschachtelte Fixture + Punkt-auf-Kurve-Fixture: rot/grün erwartet) + Doku `docs/ARCHITECTURE.md` | abgesichert |

## 4. Entscheidungen (Nutzer 2026-11-05: „3 mal ja“)
1. Normalisieren schreibt nur in einen neuen Ordner (`<Projektordner>_FreeCAD`).
2. Gegenprobe: Standard ist die Weekly-AppImage, im Dialog änderbar (Einstellung
   `SandboxCompatReferenceFreeCAD`).
3. Gelenke, die normales FreeCAD nicht kann (Punkt auf Kurve), werden in der Kopie optional
   deaktiviert (Ankreuzfeld, Standard an).

## 5. Umsetzung
- `src/Mod/Assembly/SandboxCompat.py`: Projekt einsammeln, statische Regeln, Abläufe, Bericht.
- `src/Mod/Assembly/SandboxCompatWorker.py`: Arbeitsskript für eigene FreeCAD-Prozesse; läuft in
  Sandbox UND normalem FreeCAD, nutzt nur dort vorhandene API.
- `src/Mod/Assembly/CommandSandboxCompat.py`: Dialog + Befehle `Assembly_SandboxCheck`,
  `Assembly_SandboxNormalize`, Menü Assembly → Sandbox.
- Test `standalone-check/kinematic-tests/test_sandbox_compat` (Zeitlimit per Datei `TIMEOUT`,
  neu in `run-test.sh`).
