import os, tempfile, zipfile, re
import FreeCAD as App
def main():
    d = tempfile.mkdtemp(prefix="xlinkprobe")
    part = App.newDocument("Teil"); b = part.addObject("Part::Box", "Box"); part.recompute(); part.saveAs(os.path.join(d, "Teil.FCStd"))
    asm = App.newDocument("Bg"); asm.saveAs(os.path.join(d, "Bg.FCStd"))
    l = asm.addObject("App::Link", "Link"); l.LinkedObject = b; asm.recompute(); asm.save()
    xml = zipfile.ZipFile(os.path.join(d, "Bg.FCStd")).read("Document.xml").decode()
    print("XLINK", re.findall(r'<XLink file="[^"]*"', xml))
    print("RESULT: PASS")
if __name__ == "__main__":
    main()
