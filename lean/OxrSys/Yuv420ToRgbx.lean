import OxrSys.Dsl

/-!
# `OxrSys.Yuv420ToRgbx` — a decoded PyroWave frame to the simulator's preview pixels

One thread per pixel. The planes are full-range BT.709 with chroma centred on 128, as PyroWave's
scaled encode writes them, packed four bytes to a word; the output is one RGBX8888 word per pixel.

Bindings (set 0):

  0  ConstantBuffer<Yuv420Params> { uint width, height; }
  1  StructuredBuffer<uint>   luma   (width·height / 4)
  2  StructuredBuffer<uint>   cb     (width·height / 16)
  3  StructuredBuffer<uint>   cr     (width·height / 16)
  4  RWStructuredBuffer<uint> rgbx   (width·height)
-/

namespace OxrSys.Yuv420ToRgbx

open LeanSlang
open OxrSys.Dsl

def byteAt (buf : String) (i : E) : E :=
  toF (band (shr (at_ buf (shr i (u 2))) (band i (u 3) * u 8)) (u 255))

def channel (e : E) : E := toU (fmin (fmax (e + fl 0.5) (fl 0.0)) (fl 255.0))

def shader : SlangShaderModule :=
  { structs := [ { name := "Yuv420Params", fields := [fld "width" uT, fld "height" uT] } ]
  , globals := [ paramsCB "Yuv420Params", roU "luma" 1, roU "cb" 2, roU "cr" 3, rwU "rgbx" 4 ]
  , functions :=
      [ entry 64 [dtid]
          [ let_ uT "i" (.member (v "tid") "x")
          , if_ (ge (v "i") (p "width" * p "height")) [ ret ]
          , let_ uT "x" (.bin "%" (v "i") (p "width"))
          , let_ uT "y" (.bin "/" (v "i") (p "width"))
          , let_ uT "c" (shr (v "y") (u 1) * shr (p "width") (u 1) + shr (v "x") (u 1))
          , let_ fT "yy" (byteAt "luma" (v "i"))
          , let_ fT "pb" (byteAt "cb" (v "c") - fl 128.0)
          , let_ fT "pr" (byteAt "cr" (v "c") - fl 128.0)
          , let_ uT "r" (channel (v "yy" + fl 1.5748 * v "pr"))
          , let_ uT "g" (channel (v "yy" - fl 0.1873 * v "pb" - fl 0.4681 * v "pr"))
          , let_ uT "b" (channel (v "yy" + fl 1.8556 * v "pb"))
          , setAt "rgbx" (v "i")
              (bor (bor (bor (v "r") (shl (v "g") (u 8))) (shl (v "b") (u 16))) (u 4278190080)) ] ] }

end OxrSys.Yuv420ToRgbx
