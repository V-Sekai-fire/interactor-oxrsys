#!/usr/bin/env bash
# The simulator's kernels, from Lean to both targets (contract-anny-kernels' gen.sh shape).
#
#   kernels/simulator/gen.sh            # emit from Lean, then cpp + spirv
#   kernels/simulator/gen.sh --no-emit  # use the committed slang/
#
#   Lean (lean/, `lake exe emit_oxrsys`)  ->  slang/<k>.slang     (committed)
#     slangc -target cpp                  ->  cpp/<k>_emit.cpp    (committed)
#     slangc -target spirv                ->  spirv/<k>.spv       (build artefact)
#       embedded as a uint32_t array      ->  spirv/<k>_spv.h     (committed)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
SLANGC="${SLANGC:-slangc}"
command -v "$SLANGC" >/dev/null 2>&1 || SLANGC="$HOME/scoop/apps/vulkan/current/Bin/slangc"
KERNELS="yuv420_to_rgbx"

if [ "${1:-}" != "--no-emit" ]; then
	( cd "$ROOT/lean" && lake exe emit_oxrsys "$HERE/slang" >/dev/null )
fi
mkdir -p "$HERE/cpp" "$HERE/spirv"
for k in $KERNELS; do
	( cd "$HERE" && "$SLANGC" -target cpp -stage compute -entry main -o "cpp/${k}_emit.cpp" "slang/$k.slang" )
	"$SLANGC" -target spirv -profile sm_6_5 -stage compute -entry main -fp-mode precise \
		-o "$HERE/spirv/$k.spv" "$HERE/slang/$k.slang"
	python3 "$HERE/embed_spv.py" "$HERE/spirv/$k.spv" "$HERE/spirv/${k}_spv.h" "$k"
done
echo "== $(echo $KERNELS | wc -w) kernel(s): slang, cpp, spirv =="
