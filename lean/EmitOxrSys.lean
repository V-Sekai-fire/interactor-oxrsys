import LeanSlang
import OxrSys

/-!
# `emit_oxrsys` — write the oxrsys kernels as Slang

    lake exe emit_oxrsys /path/to/output/dir
-/

open LeanSlang

private def kernels : List (String × SlangShaderModule) :=
  [ ("yuv420_to_rgbx", OxrSys.Yuv420ToRgbx.shader) ]

def main (args : List String) : IO UInt32 := do
  let outDir := args.headD "."
  IO.FS.createDirAll outDir
  for (name, m) in kernels do
    let path := outDir ++ "/" ++ name ++ ".slang"
    IO.FS.writeFile path (LeanSlang.emit m ++ "\n")
    IO.println s!"wrote {path}"
  return 0
