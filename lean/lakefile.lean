import Lake
open Lake DSL

package OxrSys where

require LeanSlang from git
  "https://github.com/V-Sekai-fire/contract-lean-slang.git" @ "60532aef8ed70cc669ecab481182d0636c9e1ac3"

@[default_target] lean_lib OxrSys

lean_exe emit_oxrsys where
  root := `EmitOxrSys
