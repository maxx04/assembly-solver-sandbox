import FreeCAD as App
import Materials
mm = Materials.MaterialManager()
try:
    m = mm.getMaterial("3ca74cf2-e188-4438-af08-aa9d6b5b955d")
    print("REPRO getMaterial OK", m.Name)
except Exception as e:
    print("REPRO getMaterial FAIL", repr(e))
try:
    n = len(mm.Materials)
    print("REPRO Materials-Liste OK", n)
except Exception as e:
    print("REPRO Materials-Liste FAIL", repr(e))
print("RESULT: PASS")
